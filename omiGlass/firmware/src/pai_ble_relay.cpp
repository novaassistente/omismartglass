// =============================================================================
// pai_ble_relay.cpp — Pendant Nova BLE-relay transport implementation
// =============================================================================
// See pai_ble_relay.h for the wire protocol. Mirrors pai_upload's HMAC + token
// handling; swaps the HTTPS POST for a BLE notify + ack drain. Bluedroid stack
// (matches app.cpp). Relay task pinned to core 0.
// =============================================================================

#include "config.h"

#if PAI_RELAY_MODE

#include <Arduino.h>
#include <BLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mbedtls/md.h>
#include <string.h>

#include "pai_ble_relay.h"
#include "pai_nvs.h"
#include "pai_storage.h"

namespace pai_ble_relay
{

// =============================================================================
// Tunables
// =============================================================================
static constexpr uint32_t RELAY_TASK_STACK = 6144;
static constexpr UBaseType_t RELAY_TASK_PRIO = 1;
static constexpr BaseType_t RELAY_TASK_CORE = 0; // same core the old upload task used
static constexpr uint32_t ACK_TIMEOUT_MS = 12000;
static constexpr uint32_t IDLE_POLL_MS = 1000;      // re-check the queue this often when idle
static constexpr uint32_t FRAG_PACE_DELAY_MS = 3;   // backpressure: yield every FRAG_PACE_EVERY frames
static constexpr uint16_t FRAG_PACE_EVERY = 8;      // ~one yield per few connection-interval windows
static constexpr size_t MIN_PAYLOAD_FOR_START = 48; // START frame size; require MTU >= 51

// HMAC self-test vector 1 (32-zero key, body="hello") — identical contract to
// pai_upload. Refuse to run on mismatch (catches a broken mbedtls config).
static const char *HMAC_VECTOR1_HEX = "4352B26E33FE0D769A8922A6BA29004109F01688E26ACC9E6CB347E5A5AFC4DA";

// =============================================================================
// State (relay task owns most; a few touched from the BLE host task)
// =============================================================================
enum class State : uint8_t { IDLE, SENDING, AWAIT_ACK };

static volatile State s_state = State::IDLE;
static volatile bool s_connected = false;
static volatile uint16_t s_payload_size = 20; // mtu(23) - 3 until negotiated

static BLECharacteristic *s_chunk_char = nullptr;
static BLECharacteristic *s_ack_char = nullptr;

static SemaphoreHandle_t s_ack_sem = nullptr;
static volatile uint64_t s_inflight_id = 0;
static volatile uint64_t s_ack_id = 0;
static volatile uint8_t s_ack_status = 0;

static uint8_t s_token[pai_nvs::UPLOAD_TOKEN_LEN] = {0};
static uint8_t s_chunk_buf[pai_storage::CHUNK_MAX_BYTES]; // .bss, avoids heap thrash
static uint32_t s_relayed_lifetime = 0;

// =============================================================================
// Helpers (HMAC + hex — same contract as pai_upload)
// =============================================================================

// Little-endian pack/unpack for the wire framing (one definition, used 3x).
static inline void put_u64le(uint8_t *dst, uint64_t v)
{
    for (int i = 0; i < 8; ++i) {
        dst[i] = (uint8_t) ((v >> (8 * i)) & 0xFF);
    }
}
static inline void put_u32le(uint8_t *dst, uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        dst[i] = (uint8_t) ((v >> (8 * i)) & 0xFF);
    }
}
static inline uint64_t get_u64le(const uint8_t *src)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= (uint64_t) src[i] << (8 * i);
    }
    return v;
}

static void hex_upper(const uint8_t *in, size_t in_len, char *out)
{
    static const char kHex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < in_len; ++i) {
        out[2 * i + 0] = kHex[(in[i] >> 4) & 0x0F];
        out[2 * i + 1] = kHex[in[i] & 0x0F];
    }
    out[2 * in_len] = '\0';
}

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

static bool hmac_smoke()
{
    uint8_t key[32] = {0};
    const char *body = "hello";
    uint8_t sig[32] = {0};
    if (hmac_sha256(key, sizeof(key), reinterpret_cast<const uint8_t *>(body), 5, sig) != 0) {
        return false;
    }
    char sig_hex[65] = {0};
    hex_upper(sig, 32, sig_hex);
    return strcmp(sig_hex, HMAC_VECTOR1_HEX) == 0;
}

static bool token_is_all_zeros(const uint8_t *tok, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        if (tok[i] != 0) {
            return false;
        }
    }
    return true;
}

// =============================================================================
// BLE lifecycle hooks
// =============================================================================
void register_characteristics(BLECharacteristic *chunk_char, BLECharacteristic *ack_char)
{
    s_chunk_char = chunk_char;
    s_ack_char = ack_char;
}

void on_connect()
{
    s_connected = true;
    Serial.println("[RELAY] central connected");
}

void on_disconnect()
{
    s_connected = false;
    s_payload_size = 20; // MTU resets to default on a new connection
    // Abandon any in-flight transfer; the chunk was NOT acked so it stays in
    // the queue and retransmits on reconnect. Nudge the task out of its wait.
    if (s_state == State::AWAIT_ACK && s_ack_sem != nullptr) {
        s_ack_status = ACK_NACK;
        s_ack_id = 0; // non-matching → task treats as "no ack", keeps chunk
        xSemaphoreGive(s_ack_sem);
    }
    Serial.println("[RELAY] central disconnected");
}

void on_mtu(uint16_t negotiated_mtu)
{
    if (negotiated_mtu < 23) {
        negotiated_mtu = 23;
    }
    s_payload_size = negotiated_mtu - 3;
    Serial.printf("[RELAY] mtu=%u payload=%u\n", (unsigned) negotiated_mtu, (unsigned) s_payload_size);
}

void on_ack(const uint8_t *data, size_t len)
{
    if (data == nullptr || len < 9) {
        return;
    }
    uint8_t status = data[0];
    uint64_t id = get_u64le(&data[1]);
    s_ack_status = status;
    s_ack_id = id;
    Serial.printf("[RELAY] ack status=%u chunk_id=%llu\n", (unsigned) status, (unsigned long long) id);
    if (s_ack_sem != nullptr) {
        xSemaphoreGive(s_ack_sem);
    }
}

bool is_busy()
{
    return s_state != State::IDLE;
}

// =============================================================================
// Transfer of one chunk over BLE
// =============================================================================

// Notify a single frame on the CHUNK characteristic. Returns immediately; the
// Bluedroid controller queues it. We pace via vTaskDelay in the caller to keep
// the tx buffer from overflowing (backpressure).
static void notify_frame(const uint8_t *buf, size_t len)
{
    if (s_chunk_char == nullptr) {
        return;
    }
    s_chunk_char->setValue(const_cast<uint8_t *>(buf), len);
    s_chunk_char->notify();
}

// Send one already-read chunk (body in s_chunk_buf[0..body_len)). Computes the
// HMAC on-device, frames START/DATA/END, paces notifications. Returns true if
// the full frame sequence was emitted (does NOT mean acked).
static bool send_chunk(uint64_t chunk_id, size_t body_len)
{
    if (s_payload_size < MIN_PAYLOAD_FOR_START) {
        // MTU not raised yet (phone must requestMtu). Skip this pass; retry.
        Serial.printf("[RELAY] payload=%u too small for START — waiting for MTU\n", (unsigned) s_payload_size);
        return false;
    }

    // HMAC-SHA256 over the body with the NVS token (key never leaves device).
    uint8_t hmac[32] = {0};
    if (hmac_sha256(s_token, sizeof(s_token), s_chunk_buf, body_len, hmac) != 0) {
        Serial.println("[RELAY] hmac compute failed — skipping chunk");
        return false;
    }

    const size_t frag_payload = (size_t) s_payload_size - 4; // DATA header = 4 B
    const uint16_t frag_count = (uint16_t) ((body_len + frag_payload - 1) / frag_payload);

    // ---- START frame (48 B) ----
    uint8_t start[48];
    start[0] = FRAME_MAGIC;
    start[1] = FRAME_START;
    put_u64le(&start[2], chunk_id);
    put_u32le(&start[10], (uint32_t) body_len);
    start[14] = (uint8_t) (frag_count & 0xFF);
    start[15] = (uint8_t) ((frag_count >> 8) & 0xFF);
    memcpy(&start[16], hmac, 32);
    notify_frame(start, sizeof(start));
    vTaskDelay(pdMS_TO_TICKS(FRAG_PACE_DELAY_MS));

    // ---- DATA frames ----
    // One stack frame buffer sized to the max negotiated notify (BLE_MTU_SIZE-3
    // payload; +0 since the 4-byte DATA header is counted inside payload_size).
    // Backpressure: yield every FRAG_PACE_EVERY frames rather than per-frame —
    // pdMS_TO_TICKS(3) rounds up to a full tick, so per-frame delay would cost
    // ~2 s on a 200-fragment chunk. Batched pacing keeps the controller tx
    // buffer from overflowing at a fraction of the cost.
    uint8_t frame[BLE_MTU_SIZE];
    frame[0] = FRAME_MAGIC;
    frame[1] = FRAME_DATA;
    size_t offset = 0;
    for (uint16_t idx = 0; idx < frag_count; ++idx) {
        if (!s_connected) {
            return false; // link dropped mid-send; chunk un-acked, will retransmit
        }
        size_t n = body_len - offset;
        if (n > frag_payload) {
            n = frag_payload;
        }
        frame[2] = (uint8_t) (idx & 0xFF);
        frame[3] = (uint8_t) ((idx >> 8) & 0xFF);
        memcpy(&frame[4], &s_chunk_buf[offset], n);
        notify_frame(frame, n + 4);
        offset += n;
        if ((idx % FRAG_PACE_EVERY) == (FRAG_PACE_EVERY - 1)) {
            vTaskDelay(pdMS_TO_TICKS(FRAG_PACE_DELAY_MS));
        }
    }

    // ---- END frame (10 B) ----
    uint8_t end[10];
    end[0] = FRAME_MAGIC;
    end[1] = FRAME_END;
    put_u64le(&end[2], chunk_id);
    notify_frame(end, sizeof(end));
    Serial.printf("[RELAY] sent chunk_id=%llu len=%u frags=%u\n",
                  (unsigned long long) chunk_id,
                  (unsigned) body_len,
                  (unsigned) frag_count);
    return true;
}

// =============================================================================
// Relay task
// =============================================================================
static void relay_task(void * /*arg*/)
{
    Serial.println("[RELAY] task started, running hmac_self_test...");
    if (!hmac_smoke()) {
        Serial.println("[RELAY] FATAL hmac_self_test_fail — task exiting");
        vTaskDelete(nullptr);
        return;
    }
    esp_err_t te = pai_nvs::get_upload_token(s_token);
    if (te != ESP_OK) {
        Serial.printf("[RELAY] FATAL token_read_err=%d — task exiting\n", (int) te);
        vTaskDelete(nullptr);
        return;
    }
    if (token_is_all_zeros(s_token, sizeof(s_token))) {
        Serial.println("[RELAY] FATAL token_all_zeros — never provisioned? — task exiting");
        vTaskDelete(nullptr);
        return;
    }
    Serial.println("[RELAY] token loaded OK — entering drain loop");

    for (;;) {
        // Idle unless connected with a usable MTU and chunks pending.
        if (!s_connected || s_payload_size < MIN_PAYLOAD_FOR_START || pai_storage::chunks_pending_count() == 0) {
            s_state = State::IDLE;
            vTaskDelay(pdMS_TO_TICKS(IDLE_POLL_MS));
            continue;
        }

        size_t out_len = 0;
        uint64_t chunk_id = 0;
        esp_err_t e = pai_storage::chunk_read_next(s_chunk_buf, sizeof(s_chunk_buf), &out_len, &chunk_id);
        if (e == ESP_ERR_NOT_FOUND) {
            s_state = State::IDLE;
            vTaskDelay(pdMS_TO_TICKS(IDLE_POLL_MS));
            continue;
        }
        if (e != ESP_OK) {
            Serial.printf("[RELAY] chunk_read_next err=%d\n", (int) e);
            vTaskDelay(pdMS_TO_TICKS(IDLE_POLL_MS));
            continue;
        }

        s_state = State::SENDING;
        s_inflight_id = chunk_id;
        // Drain any stale ack signal before sending.
        xSemaphoreTake(s_ack_sem, 0);

        if (!send_chunk(chunk_id, out_len)) {
            // Couldn't send (no MTU / link drop / hmac fail). Keep chunk, retry.
            s_state = State::IDLE;
            vTaskDelay(pdMS_TO_TICKS(IDLE_POLL_MS));
            continue;
        }

        // Await the phone's ack (it acks only after a server 2xx).
        s_state = State::AWAIT_ACK;
        if (xSemaphoreTake(s_ack_sem, pdMS_TO_TICKS(ACK_TIMEOUT_MS)) == pdTRUE && s_ack_status == ACK_OK &&
            s_ack_id == chunk_id) {
            pai_storage::chunk_delete(chunk_id);
            ++s_relayed_lifetime;
            Serial.printf("[RELAY] acked+deleted chunk_id=%llu relayed_lifetime=%u\n",
                          (unsigned long long) chunk_id,
                          (unsigned) s_relayed_lifetime);
        } else {
            // NACK / timeout / mismatch / disconnect — keep the chunk; it stays
            // oldest and retransmits next pass. Brief pause avoids a tight loop
            // hammering a phone that can't reach the server.
            Serial.printf("[RELAY] no-ack chunk_id=%llu — keeping for retry\n", (unsigned long long) chunk_id);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        s_state = State::IDLE;
    }
}

void start_task()
{
    if (s_ack_sem == nullptr) {
        s_ack_sem = xSemaphoreCreateBinary();
    }
    if (s_ack_sem == nullptr) {
        Serial.println("[RELAY] FATAL ack_sem_create_fail — relay disabled");
        return;
    }
    xTaskCreatePinnedToCore(
        relay_task, "pai_relay", RELAY_TASK_STACK, nullptr, RELAY_TASK_PRIO, nullptr, RELAY_TASK_CORE);
}

} // namespace pai_ble_relay

#endif // PAI_RELAY_MODE
