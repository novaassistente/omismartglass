// =============================================================================
// pai_upload.cpp — Pendant Nova HTTPS chunk uploader (S3a + S3b)
// =============================================================================
// See pai_upload.h for the full module contract. This translation unit
// implements the FreeRTOS task body, HMAC-SHA256, the HTTPS POST, the
// retry / poison policy, the NVS-configurable cadence, and the adaptive
// boost trigger.
// =============================================================================

#include "pai_upload.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <mbedtls/md.h>
#include <nvs.h> // ESP_ERR_NVS_NOT_FOUND

#include <atomic>
#include <cstring>

#include "pai_certs.h"
#include "pai_nvs.h"
#include "pai_storage.h"
#include "pai_wifi.h"

namespace pai_upload
{

// =============================================================================
// Constants
// =============================================================================

// Default cadence (D2). 15 min batch; min clamp 30 s so a misconfigured
// NVS value can never tight-loop the server. NVS slot name lives in
// pai_nvs schema as a future blob; reading is best-effort here (slot
// may not exist on a device provisioned before S3 — falls back to
// DEFAULT_TICK_MS).
static constexpr uint32_t DEFAULT_TICK_MS = 15u * 60u * 1000u; // 15 min
static constexpr uint32_t MIN_TICK_MS = 30u * 1000u;           // 30 s
static constexpr const char *NVS_KEY_TICK = "upload_tick";

// D3 adaptive boost thresholds. Wrap-safe pattern: (uint32_t)(now-last)
// >= threshold; modular arithmetic survives millis() wrap at 49 d.
static constexpr uint32_t IDLE_MIN_MS = 60u * 1000u;      // mic+drain idle
static constexpr uint32_t DRAIN_MIN_GAP_MS = 60u * 1000u; // ISC-30

// HTTPS / retry policy.
static constexpr const char *DEFAULT_ENDPOINT = "https://pendant.futuretools.today/upload";
// User-Agent aligned to hmac-contract.md §Wire-format — the E2E-verified value
// known to bypass Cloudflare Bot Fight Mode against pendant.futuretools.today.
// "PAI-Pendant/*" is NOT yet on any documented allowlist; using the contract
// value avoids regression if CF tightens to a UA allowlist.
static constexpr const char *USER_AGENT = "pendant-nova/0.1";
static constexpr uint32_t HTTP_TIMEOUT_MS = 15u * 1000u;
static constexpr uint8_t MAX_RETRY_PER_CHUNK = 3;
static constexpr uint32_t BACKOFF_BASE_MS = 1000u;
static constexpr uint32_t BACKOFF_CAP_MS = 60u * 1000u;

// Task config — 16 KiB stack to give mbedtls TLS handshake + WiFiClientSecure
// + HTTPClient headroom. 8 KiB was tight: mbedtls handshake alone runs ~6 KiB
// (cipher state machine + record buffers); adding HTTPClient header build +
// our local sig_hex[65] + chunk_id_str[32] frames left ~0-2 KiB margin in the
// worst case. Stack overflow on first POST after long idle was plausible.
static constexpr uint32_t TASK_STACK_BYTES = 16u * 1024u;
static constexpr UBaseType_t TASK_PRIO = 5;
static constexpr BaseType_t TASK_CORE = 0; // core 1 reserved for REC

// Poison tracker — small ring buffer of chunk_ids that have hit 401 at
// least once in this boot. 16 slots is plenty: 401s indicate contract
// drift, not normal operation, so we expect ≤ a handful before the
// operator notices and re-provisions.
static constexpr size_t AUTH_FAIL_RING_LEN = 16;

// HMAC self-test (ISC-43) — vector 1 from hmac-contract.md.
static constexpr const char *HMAC_VECTOR1_HEX = "4352B26E33FE0D769A8922A6BA29004109F01688E26ACC9E6CB347E5A5AFC4DA";

// =============================================================================
// Cross-core state (ISC-44)
//
// memory_order_relaxed is correct here. We need cross-core *visibility*
// (no stale reads after another core has written), not happens-before
// *ordering* (no other variables depend on these reads/writes in any
// algorithm). 32-bit aligned loads/stores on Xtensa LX7 are lock-free
// and atomic at the hardware level — these atomics compile to plain
// l32i.n / s32i.n with a `memw` fence only where the standard demands.
// Cost vs volatile: zero in practice; correctness vs UB: large.
// =============================================================================
static std::atomic<uint32_t> s_last_mic_active_ms{0};
static std::atomic<uint32_t> s_last_drain_ms{0};
static std::atomic<uint32_t> s_uploaded_lifetime{0};
static std::atomic<uint32_t> s_auth_fail_lifetime{0};
static std::atomic<uint32_t> s_chunks_poisoned_lifetime{0};
static std::atomic<bool> s_task_started{false};
static std::atomic<bool> s_boost_eligible_hint{false};
static std::atomic<bool> s_in_drain{false};

// =============================================================================
// Task-local state (touched only on core 0)
// =============================================================================
static uint8_t s_token[pai_nvs::UPLOAD_TOKEN_LEN] = {0};
static char s_endpoint[pai_nvs::NVS_MAX_ENDPOINT_LEN] = {0};
static uint32_t s_tick_ms = DEFAULT_TICK_MS;
static SemaphoreHandle_t s_boost_sem = nullptr;
static TimerHandle_t s_boost_tick_timer = nullptr;
static uint64_t s_auth_fail_ring[AUTH_FAIL_RING_LEN] = {0};
static size_t s_auth_fail_ring_idx = 0;

// Static chunk body buffer; pai_storage::CHUNK_MAX_BYTES = 16 KiB.
// Lives in .bss to avoid per-cycle heap thrash.
static uint8_t s_chunk_buf[pai_storage::CHUNK_MAX_BYTES];

// File-scope HTTPS client + HTTPClient singletons (C#5 mitigation). The
// upload task is single-threaded, so these are safely shared across all
// drains within a single boot. Hoisting them out of post_chunk_once means
// the internal mbedtls SSL context + HTTPClient header buffers are
// allocated/freed at most once per HTTP transaction (via http.end()) and
// the C++ object lifecycle cost is paid at .bss init time, not per call.
// Without this, retry storms (3 attempts × 3 chunks) thrashed heap with
// ~9 fresh mbedtls SSL contexts in succession — long-uptime devices
// eventually saw ESP_ERR_NO_MEM on TLS handshake.
static WiFiClientSecure s_https_client;
static HTTPClient s_http;
static bool s_https_initialized = false;

// =============================================================================
// Helpers
// =============================================================================

// Constant-time uppercase hex encode of `in[0..in_len)` into out[2*in_len+1].
static void hex_upper(const uint8_t *in, size_t in_len, char *out)
{
    static const char kHex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < in_len; ++i) {
        out[2 * i + 0] = kHex[(in[i] >> 4) & 0x0F];
        out[2 * i + 1] = kHex[in[i] & 0x0F];
    }
    out[2 * in_len] = '\0';
}

// HMAC-SHA256(key[0..key_len), msg[0..msg_len)) → sig[32]. Returns 0 on
// success, mbedtls error code otherwise. Uses mbedtls shipped with
// arduino-esp32 (no extra platformio.ini lib).
static int hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len, uint8_t sig[32])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == nullptr) {
        return -1;
    }
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    int rc = mbedtls_md_setup(&ctx, info, 1 /* hmac */);
    if (rc != 0) {
        mbedtls_md_free(&ctx);
        return rc;
    }
    rc = mbedtls_md_hmac_starts(&ctx, key, key_len);
    if (rc == 0) {
        rc = mbedtls_md_hmac_update(&ctx, msg, msg_len);
    }
    if (rc == 0) {
        rc = mbedtls_md_hmac_finish(&ctx, sig);
    }
    mbedtls_md_free(&ctx);
    return rc;
}

// HMAC self-test against canonical vector 1 (32-zero key, body="hello").
// Returns true on byte-for-byte match. ISC-43.
static bool hmac_smoke()
{
    uint8_t key[32] = {0};
    const char *body = "hello";
    uint8_t sig[32] = {0};
    int rc = hmac_sha256(key, sizeof(key), reinterpret_cast<const uint8_t *>(body), 5, sig);
    if (rc != 0) {
        Serial.printf("[UPLOAD] hmac_self_test mbedtls_rc=%d\n", rc);
        return false;
    }
    char sig_hex[65] = {0};
    hex_upper(sig, 32, sig_hex);
    bool ok = (strcmp(sig_hex, HMAC_VECTOR1_HEX) == 0);
    if (!ok) {
        // NEVER log sig_hex itself (it's the literal vector and not a
        // secret, but the diagnostic-via-substring habit is exactly how
        // a real-key smoke could leak in a future copy-paste).
        Serial.println("[UPLOAD] hmac_self_test vector_mismatch");
    }
    return ok;
}

// Returns true iff `tok[0..32)` is all-zero. pai_nvs::get_upload_token
// zeros the buffer on failure, so all-zeros means "never provisioned"
// OR an NVS read failed — either way, refuse.
static bool token_is_all_zeros(const uint8_t *tok, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        if (tok[i] != 0) {
            return false;
        }
    }
    return true;
}

// True if `chunk_id` has hit 401 at least once already in this boot.
// chunk_id == 0 (pre-NTP boot edge case) tracked via dedicated flag because
// the ring buffer uses 0 as the empty sentinel and would never match it.
static bool s_auth_fail_zero_seen = false;
static bool auth_fail_seen(uint64_t chunk_id)
{
    if (chunk_id == 0) {
        return s_auth_fail_zero_seen;
    }
    for (size_t i = 0; i < AUTH_FAIL_RING_LEN; ++i) {
        if (s_auth_fail_ring[i] == chunk_id) {
            return true;
        }
    }
    return false;
}

// Record a 401 against `chunk_id` for poison-on-second-fail detection.
static void auth_fail_record(uint64_t chunk_id)
{
    if (chunk_id == 0) {
        s_auth_fail_zero_seen = true;
        return;
    }
    s_auth_fail_ring[s_auth_fail_ring_idx] = chunk_id;
    s_auth_fail_ring_idx = (s_auth_fail_ring_idx + 1) % AUTH_FAIL_RING_LEN;
}

// Poison a chunk via the pai_storage API so the pending-count is decremented
// atomically AND chunks_poisoned_count is incremented (boot-scan + eviction
// fallback both see the .poisoned file consistently). Falls back to
// chunk_delete on rename failure so a bad chunk never tight-loops.
static bool poison_chunk(uint64_t chunk_id)
{
    esp_err_t e = pai_storage::poison_chunk(chunk_id);
    if (e == ESP_OK) {
        s_chunks_poisoned_lifetime.fetch_add(1, std::memory_order_relaxed);
        Serial.printf("[UPLOAD] poisoned chunk_id=%llu\n", (unsigned long long) chunk_id);
        return true;
    }
    // Last-resort: delete so the bad chunk doesn't re-loop forever. Counter
    // still goes up so the operator sees the symptom in observability.
    Serial.printf(
        "[UPLOAD] poison_rename_failed chunk_id=%llu err=%d — deleting\n", (unsigned long long) chunk_id, (int) e);
    pai_storage::chunk_delete(chunk_id);
    s_chunks_poisoned_lifetime.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// Read NVS cadence; fall back to DEFAULT_TICK_MS on any error and clamp
// to MIN_TICK_MS. A misconfigured slot (too-low value) is silently raised
// to MIN_TICK_MS so a typo can never tight-loop the server. Missing slot
// is the common case on devices provisioned before S3 — falls back to
// DEFAULT_TICK_MS without logging at error level.
static uint32_t read_tick_ms()
{
    uint32_t v = 0;
    esp_err_t err = pai_nvs::get_upload_tick_ms(&v);
    if (err != ESP_OK || v == 0) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            Serial.printf("[UPLOAD] upload_tick_ms read err=0x%x — using default\n", (unsigned) err);
        }
        v = DEFAULT_TICK_MS;
    }
    if (v < MIN_TICK_MS) {
        Serial.printf("[UPLOAD] upload_tick_ms=%u below MIN — clamping to %u\n", (unsigned) v, (unsigned) MIN_TICK_MS);
        v = MIN_TICK_MS;
    }
    return v;
}

// =============================================================================
// HTTPS POST
// =============================================================================

enum class PostResult : uint8_t {
    OK = 0,
    HMAC_MISMATCH = 1,  // server returned 401
    RETRYABLE = 2,      // 5xx / network / timeout
    PERMANENT_FAIL = 3, // 400 / other unrecoverable
};

// Single POST attempt for one chunk. No internal retry — that's the
// caller's job so backoff/poison policy stays in one place.
static PostResult post_chunk_once(uint64_t chunk_id, const uint8_t *body, size_t body_len)
{
    if (!pai_wifi::is_connected()) {
        return PostResult::RETRYABLE;
    }

    // HMAC over body.
    uint8_t sig[32] = {0};
    if (hmac_sha256(s_token, sizeof(s_token), body, body_len, sig) != 0) {
        // Compute failure (mbedtls). Counts as retryable — likely
        // transient resource issue (heap exhaustion in mbedtls ctx).
        return PostResult::RETRYABLE;
    }
    char sig_hex[65] = {0};
    hex_upper(sig, 32, sig_hex);

    // One-time TLS client configuration. Pinned to GTS Root R4 (see
    // pai_certs.h). Stays in effect across all subsequent transactions
    // — setCACert reconfigures the trust anchor without re-allocating
    // the mbedtls SSL context. Defense-in-depth: HMAC over body remains
    // the actual auth at the application layer.
    if (!s_https_initialized) {
        s_https_client.setCACert(pai_certs::ROOT_CA_PEM);
        // WiFiClientSecure socket-level timeout: arduino-esp32's
        // Stream::setTimeout unit is MILLISECONDS (matches HTTPClient).
        s_https_client.setTimeout(HTTP_TIMEOUT_MS);
        s_http.setConnectTimeout(HTTP_TIMEOUT_MS);
        s_http.setTimeout(HTTP_TIMEOUT_MS);
        // setUserAgent BEFORE begin(): addHeader("User-Agent",..) after
        // begin() is silently overridden by HTTPClient's internal
        // _userAgent on some arduino-esp32 versions, which would let
        // the default UA hit CF Bot Fight Mode (403/1010).
        s_http.setUserAgent(USER_AGENT);
        // setReuse(true) keeps the keep-alive socket between calls when
        // the server agrees. uvicorn (the FastAPI server) honours HTTP/1.1
        // keep-alive by default, so subsequent uploads in the same drain
        // cycle skip the full TLS handshake — significant heap + CPU win.
        s_http.setReuse(true);
        s_https_initialized = true;
    }

    const char *endpoint = (s_endpoint[0] != '\0') ? s_endpoint : DEFAULT_ENDPOINT;
    if (!s_http.begin(s_https_client, endpoint)) {
        return PostResult::RETRYABLE;
    }

    // Required headers (hmac-contract.md §Wire format). User-Agent set via
    // setUserAgent() above; Content-Type/Content-Length handled internally by
    // HTTPClient::POST(uint8_t*, size_t).
    char chunk_id_str[32] = {0};
    snprintf(chunk_id_str, sizeof(chunk_id_str), "%llu", (unsigned long long) chunk_id);
    s_http.addHeader("Content-Type", "application/octet-stream");
    s_http.addHeader("X-Chunk-Id", chunk_id_str);
    s_http.addHeader("X-HMAC-SHA256", sig_hex);

    int code = s_http.POST(const_cast<uint8_t *>(body), body_len);
    s_http.end();

    // Defense-in-depth: zero BOTH the hex and raw sig buffers on the stack
    // before return so a stack-dump tool can't recover the secret from frame
    // residue. Body is the caller's — they own their buffer lifetime.
    memset(sig_hex, 0, sizeof(sig_hex));
    memset(sig, 0, sizeof(sig));

    if (code == 200) {
        return PostResult::OK;
    }
    if (code == 401) {
        return PostResult::HMAC_MISMATCH;
    }
    if (code >= 500 || code < 0) {
        // <0 = WiFiClient / HTTPClient internal errors (timeout, etc.)
        return PostResult::RETRYABLE;
    }
    // 400, 403, 404, 41x: caller bug or server policy — don't retry.
    Serial.printf("[UPLOAD] unexpected status=%d chunk_id=%llu\n", code, (unsigned long long) chunk_id);
    return PostResult::PERMANENT_FAIL;
}

// Tri-state outcome of upload_one_chunk so the drain loop can distinguish
// individual-chunk failure (poison — keep draining) from transport-wide
// failure (retryable-exhausted — stop the drain so we don't hammer a flapping
// server with the next chunk).
enum class ChunkOutcome : uint8_t {
    UPLOADED,            // 200 — caller deletes
    POISONED,            // 401 repeat OR 4xx PERMANENT_FAIL — chunk renamed .poisoned
    RETRYABLE_EXHAUSTED, // 3× 5xx/network — chunk left in place for next cycle
};

// Upload one chunk with full retry / backoff / poison policy applied.
static ChunkOutcome upload_one_chunk(uint64_t chunk_id, const uint8_t *body, size_t body_len)
{
    uint32_t backoff_ms = BACKOFF_BASE_MS;
    for (uint8_t attempt = 0; attempt < MAX_RETRY_PER_CHUNK; ++attempt) {
        PostResult r = post_chunk_once(chunk_id, body, body_len);
        if (r == PostResult::OK) {
            return ChunkOutcome::UPLOADED;
        }
        if (r == PostResult::HMAC_MISMATCH) {
            s_auth_fail_lifetime.fetch_add(1, std::memory_order_relaxed);
            if (auth_fail_seen(chunk_id)) {
                // Second 401 for the same chunk in this boot — contract
                // is drifting OR the chunk body is corrupt. Poison and
                // continue with the rest of the drain.
                Serial.printf("[UPLOAD] hmac_mismatch_repeat chunk_id=%llu — poisoning\n",
                              (unsigned long long) chunk_id);
                poison_chunk(chunk_id);
                return ChunkOutcome::POISONED;
            }
            // First 401: record, warn, retry once more (next attempt of
            // the loop). NEVER log sig or key bytes.
            auth_fail_record(chunk_id);
            Serial.printf("[UPLOAD] hmac_mismatch chunk_id=%llu attempt=%u — retrying\n",
                          (unsigned long long) chunk_id,
                          (unsigned) attempt);
            // Light delay so we don't immediately re-spam server.
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (r == PostResult::PERMANENT_FAIL) {
            // 400 / 403 etc — chunk will never succeed. Poison.
            poison_chunk(chunk_id);
            return ChunkOutcome::POISONED;
        }
        // RETRYABLE: 5xx or network glitch. Exponential backoff.
        Serial.printf("[UPLOAD] retryable chunk_id=%llu attempt=%u backoff=%ums\n",
                      (unsigned long long) chunk_id,
                      (unsigned) attempt,
                      (unsigned) backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        backoff_ms = (backoff_ms * 2 > BACKOFF_CAP_MS) ? BACKOFF_CAP_MS : (backoff_ms * 2);
    }
    // Exhausted retries — leave chunk in place; next drain cycle will
    // try again (with a fresh retry budget). DO NOT poison here:
    // exhausting retries on 5xx/network is a server-side or transport
    // condition that should self-resolve. Caller stops the drain because
    // continuing would just hammer the same flapping server.
    return ChunkOutcome::RETRYABLE_EXHAUSTED;
}

// Drain all available .opus chunks. Bounded by chunks_pending_count()
// at entry; new chunks written during the drain wait for the next
// cycle (so a chatty mic can't starve the drain).
static void drain_available_chunks()
{
    // is_busy flag — power-management code reads this to skip light-sleep
    // while we hold a TCP/TLS connection. Cleared via RAII at scope exit.
    s_in_drain.store(true, std::memory_order_release);
    struct DrainGuard {
        ~DrainGuard()
        {
            s_in_drain.store(false, std::memory_order_release);
        }
    } drain_guard;
    const size_t initial_pending = pai_storage::chunks_pending_count();
    size_t drained = 0;
    for (size_t i = 0; i < initial_pending; ++i) {
        size_t out_len = 0;
        uint64_t chunk_id = 0;
        esp_err_t e = pai_storage::chunk_read_next(s_chunk_buf, sizeof(s_chunk_buf), &out_len, &chunk_id);
        if (e == ESP_ERR_NOT_FOUND) {
            break;
        }
        if (e != ESP_OK) {
            Serial.printf("[UPLOAD] chunk_read_next err=%d — breaking\n", (int) e);
            break;
        }

        ChunkOutcome oc = upload_one_chunk(chunk_id, s_chunk_buf, out_len);
        if (oc == ChunkOutcome::UPLOADED) {
            pai_storage::chunk_delete(chunk_id);
            s_uploaded_lifetime.fetch_add(1, std::memory_order_relaxed);
            ++drained;
        } else if (oc == ChunkOutcome::POISONED) {
            // Individual chunk is unrecoverable; the rest of the drain may
            // still succeed. Continue (NOT break) so a single corrupt or
            // contract-drifted chunk doesn't cap throughput at one chunk
            // per 15min cycle.
            continue;
        } else {
            // RETRYABLE_EXHAUSTED: transport/server-side flap. Stop the
            // drain to avoid hammering — let backoff+next cycle recover.
            break;
        }

        // Cooperative yield between chunks — REC on core 1 is on its
        // own core and won't starve, but this lets core 0 timers /
        // WiFi housekeeping breathe.
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // Zero the body buffer when leaving the drain — defense-in-depth
    // against any future caller reading stale .bss.
    memset(s_chunk_buf, 0, sizeof(s_chunk_buf));

    s_last_drain_ms.store(static_cast<uint32_t>(millis()), std::memory_order_relaxed);
    if (drained > 0) {
        Serial.printf("[UPLOAD] drained=%u uploaded_lifetime=%u poisoned_lifetime=%u\n",
                      (unsigned) drained,
                      (unsigned) s_uploaded_lifetime.load(std::memory_order_relaxed),
                      (unsigned) s_chunks_poisoned_lifetime.load(std::memory_order_relaxed));
    }
}

// =============================================================================
// Adaptive boost (D3)
// =============================================================================

// True iff the device is in a "quiet, online" state and the min-gap
// since last drain has elapsed. Wrap-safe via unsigned modular
// arithmetic on uint32_t (D-PT3).
static bool boost_condition_met(uint32_t now_ms)
{
    if (!pai_wifi::is_connected()) {
        return false;
    }
    const uint32_t since_drain = now_ms - s_last_drain_ms.load(std::memory_order_relaxed);
    const uint32_t since_mic = now_ms - s_last_mic_active_ms.load(std::memory_order_relaxed);
    if (since_drain < DRAIN_MIN_GAP_MS) {
        return false; // ISC-30 — even repeated boost can't violate gap
    }
    if (since_drain < IDLE_MIN_MS) {
        return false;
    }
    if (since_mic < IDLE_MIN_MS) {
        return false;
    }
    return true;
}

// 1 Hz timer callback. Cheap: just checks the condition and gives the
// semaphore if met. Runs on the FreeRTOS timer service task (not the
// upload task), so the upload task is free to be inside an HTTP POST
// when this fires.
static void boost_tick_cb(TimerHandle_t /*xTimer*/)
{
    if (s_boost_sem == nullptr) {
        return;
    }
    // Cheap check at 1 Hz: if quiet + online + min-gap-elapsed, give the
    // semaphore. The boost_eligible_hint is set by pai_rec callers as a
    // courtesy signal but is NOT a gate — the boost decision is driven
    // strictly by the wrap-safe time deltas in boost_condition_met.
    if (boost_condition_met(static_cast<uint32_t>(millis()))) {
        xSemaphoreGive(s_boost_sem);
    }
}

// =============================================================================
// Task body
// =============================================================================

static void upload_task(void * /*arg*/)
{
    Serial.println("[UPLOAD] task started, running hmac_self_test...");

    // ISC-43 — refuse to start on self-test mismatch.
    if (!hmac_smoke()) {
        Serial.println("[UPLOAD] FATAL hmac_self_test_fail — task exiting");
        vTaskDelete(nullptr);
        return;
    }

    // Load NVS token (ISC-45 — pai_nvs::get_upload_token returns 32
    // raw bytes directly; no string/hex confusion possible).
    esp_err_t te = pai_nvs::get_upload_token(s_token);
    if (te != ESP_OK) {
        Serial.printf("[UPLOAD] FATAL token_read_err=%d — task exiting\n", (int) te);
        vTaskDelete(nullptr);
        return;
    }
    if (token_is_all_zeros(s_token, sizeof(s_token))) {
        // Defensive: pai_nvs zeros on failure, so all-zeros means
        // either (a) never provisioned, or (b) silent read failure
        // that returned ESP_OK with zeroed buf. Either way, refuse.
        Serial.println("[UPLOAD] FATAL token_all_zeros — never provisioned? — task exiting");
        memset(s_token, 0, sizeof(s_token));
        vTaskDelete(nullptr);
        return;
    }

    // Endpoint: prefer NVS, fall back to canonical hardcoded URL.
    esp_err_t ee = pai_nvs::get_upload_endpoint(s_endpoint, sizeof(s_endpoint));
    if (ee != ESP_OK || s_endpoint[0] == '\0') {
        // NVS slot missing or empty — fine for older provisioned
        // devices. DEFAULT_ENDPOINT will be used at POST time.
        s_endpoint[0] = '\0';
    }

    s_tick_ms = read_tick_ms();
    Serial.printf(
        "[UPLOAD] tick_ms=%u endpoint=%s\n", (unsigned) s_tick_ms, (s_endpoint[0] != '\0') ? "nvs" : "default");

    // Boost semaphore + 1 Hz tick timer (ISC-28, ISC-29).
    s_boost_sem = xSemaphoreCreateBinary();
    if (s_boost_sem == nullptr) {
        Serial.println("[UPLOAD] FATAL boost_sem_create_fail — task exiting");
        vTaskDelete(nullptr);
        return;
    }
    s_boost_tick_timer = xTimerCreate("pai_boost", pdMS_TO_TICKS(1000), pdTRUE, nullptr, boost_tick_cb);
    if (s_boost_tick_timer != nullptr) {
        xTimerStart(s_boost_tick_timer, 0);
    } else {
        Serial.println("[UPLOAD] boost_tick_timer_create_fail — D3 boost disabled");
        // Non-fatal: tick-only cadence still works (D2).
    }

    // Primary loop. Wake on EITHER timer tick OR boost semaphore.
    // xSemaphoreTake returns pdTRUE on semaphore, pdFALSE on timeout —
    // we treat both identically (drain). Min-gap enforcement happens
    // inside boost_condition_met for the boost path; the tick path is
    // self-paced by definition.
    for (;;) {
        xSemaphoreTake(s_boost_sem, pdMS_TO_TICKS(s_tick_ms));
        // Final guard before drain: connected AND a valid IP assigned. The L2
        // CONNECTED state can briefly precede a usable DHCP/DNS lease; gating on
        // is_got_ip() too prevents the first POST from hitting "DNS Failed".
        if (pai_wifi::is_connected() && pai_wifi::is_got_ip()) {
            drain_available_chunks();
        }
    }
}

// =============================================================================
// Public API
// =============================================================================

void start_task()
{
    bool expected = false;
    if (!s_task_started.compare_exchange_strong(expected, true)) {
        return; // idempotent
    }
    BaseType_t rc =
        xTaskCreatePinnedToCore(upload_task, "pai_upload", TASK_STACK_BYTES, nullptr, TASK_PRIO, nullptr, TASK_CORE);
    if (rc != pdPASS) {
        Serial.printf("[UPLOAD] xTaskCreatePinnedToCore rc=%d\n", (int) rc);
        s_task_started.store(false, std::memory_order_relaxed);
    }
}

void notify_boost_eligible()
{
    // Hint only — the 1 Hz boost_tick_cb is the actual decision point.
    s_boost_eligible_hint.store(true, std::memory_order_relaxed);
}

bool is_busy()
{
    return s_in_drain.load(std::memory_order_acquire);
}

void touch_last_mic_active(uint32_t now_ms)
{
    s_last_mic_active_ms.store(now_ms, std::memory_order_relaxed);
}

} // namespace pai_upload
