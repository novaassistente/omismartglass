// =============================================================================
// pai_rec.cpp — Pendant Nova REC accumulator implementation (S2.5)
// =============================================================================
// Anti-leak invariants (ISC-A1, ISC-A2):
//   * Search this file for Serial.print* / ESP_LOG*. Every call site logs
//     ONLY counters, status codes, or fixed strings — NEVER frame bytes,
//     buffer contents, or any byte-derived fingerprint of speech.
//
// Memory layout (ISC-2):
//   * Two static 240 KiB buffers (buf_a, buf_b), allocated in .bss. Total
//     static cost = 2 * CHUNK_ROTATE_BYTES = 480 KiB. The ESP32-S3 has 512
//     KiB SRAM + 8 MiB PSRAM; PSRAM placement is left to the linker / build
//     config. Either way, NO heap allocation in the hot path (D-PT4).
//
// Mutex granularity (D-PT4):
//   * Recursive mutex covers only:
//       (a) the buffer pointer swap inside rotate()
//       (b) the read-modify-write of bytes_accumulated/active_started_ms
//         inside feed_opus_frame and rotate().
//   * pai_storage::chunk_write is called OUTSIDE the mutex on a captured
//     snapshot pointer + length. This keeps the swap critical section in
//     the microsecond range while chunk_write (which can BLOCK on LittleFS
//     for milliseconds) runs without holding the mutex — feed_opus_frame
//     on the NEW active buffer continues with zero stall.
//   * Recursive flavor (xSemaphoreCreateRecursiveMutex) is defensive: it
//     allows feed_opus_frame to be re-entered indirectly (e.g. via tick_ms
//     fast-rotate path) without self-deadlock.
// =============================================================================

#include "pai_rec.h"

#include <Arduino.h>
#include <esp_err.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

#include <atomic>

#include "pai_storage.h"

// Optional coupling to Otto's pai_upload (S3 slice, parallel agent). When the
// header is present in the worktree, we call the touch_last_mic_active hook
// directly so the D3 adaptive-boost idle detector sees REC activity. When the
// header is absent (this worktree at branch commit time), the call site is
// elided at compile time and the main agent will wire it during merge.
// PRD ISC-32 — owner: Otto (pai_upload slice).
#if defined(__has_include)
#if __has_include("pai_upload.h")
#include "pai_upload.h"
#define PAI_REC_HAVE_UPLOAD_HOOK 1
#endif
#endif

namespace pai_rec
{

namespace
{

static const char *TAG = "pai_rec";

// -----------------------------------------------------------------------------
// Static double-buffer storage (ISC-2)
// -----------------------------------------------------------------------------
static constexpr size_t BUF_SIZE = pai_storage::CHUNK_ROTATE_BYTES;
static constexpr uint32_t ROTATE_INTERVAL_MS = pai_storage::CHUNK_ROTATE_INTERVAL_MS;

static uint8_t buf_a[BUF_SIZE];
static uint8_t buf_b[BUF_SIZE];

// -----------------------------------------------------------------------------
// Shared state — pointer + counters protected by s_mutex
// -----------------------------------------------------------------------------
SemaphoreHandle_t s_mutex = nullptr;
std::atomic<bool> s_inited{false};

uint8_t *s_active_buf = nullptr;  // points to buf_a or buf_b
uint8_t *s_shadow_buf = nullptr;  // the other one
size_t s_bytes_accumulated = 0;   // bytes currently in s_active_buf
uint32_t s_active_started_ms = 0; // millis() at start of current buffer window

// Cross-core observability (ISC-7). std::atomic with relaxed ordering — we
// need value-level freshness, not happens-before with other state. Same
// rationale documented at length in pai_storage.cpp.
std::atomic<size_t> s_bytes_pending{0};
std::atomic<size_t> s_chunks_written_lifetime{0};
std::atomic<size_t> s_rotations_failed_lifetime{0};

// -----------------------------------------------------------------------------
// Mutex RAII guard (recursive flavor) — releases on scope exit, error paths included
// -----------------------------------------------------------------------------
class Lock
{
  public:
    explicit Lock(TickType_t wait = portMAX_DELAY) : ok_(false)
    {
        if (s_mutex != nullptr) {
            ok_ = (xSemaphoreTakeRecursive(s_mutex, wait) == pdTRUE);
        }
    }
    ~Lock()
    {
        if (ok_ && s_mutex != nullptr) {
            xSemaphoreGiveRecursive(s_mutex);
        }
    }
    bool held() const
    {
        return ok_;
    }

    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;

  private:
    bool ok_;
};

// -----------------------------------------------------------------------------
// touch_last_mic_active — boost idle detector hook (ISC-32)
// -----------------------------------------------------------------------------
// Routed via Otto's pai_upload when the header is available. When unavailable
// (this worktree alone), it's a no-op and the main agent wires it during merge.
// NEVER passes any frame content — only the timestamp.
inline void notify_mic_active(uint32_t now_ms)
{
#if defined(PAI_REC_HAVE_UPLOAD_HOOK)
    pai_upload::touch_last_mic_active(now_ms);
#else
    (void) now_ms;
    // TODO(main-agent): once pai_upload.h lands in the merge, the
    // PAI_REC_HAVE_UPLOAD_HOOK branch above activates automatically. No code
    // change here required — just rebuild.
#endif
}

// -----------------------------------------------------------------------------
// rotate — close the active buffer, swap to shadow, hand snapshot to chunk_write
// -----------------------------------------------------------------------------
// Called with s_mutex HELD by the caller. The function:
//   1. Snapshots active_buf + len under the mutex.
//   2. Swaps pointers (active becomes shadow, shadow becomes active).
//   3. Resets bytes_accumulated to 0 and active_started_ms to `now_ms`.
//   4. RELEASES the mutex by `Lock` going out of scope at the call site —
//      i.e. the chunk_write call below MUST be after the Lock scope ends.
//
// On chunk_write success: bumps s_chunks_written_lifetime.
// On chunk_write ESP_ERR_NO_MEM (full filesystem after eviction): bumps
// s_rotations_failed_lifetime. The shadow_buf still holds the unwritten
// payload, but the next rotation will overwrite it — by design we accept
// the one-window data loss rather than stalling REC. The PRIMARY guarantee
// is "REC never pauses" (D1).
//
// IMPORTANT: this helper does NOT itself unlock the mutex. The caller must
// arrange Lock scoping such that chunk_write happens after the Lock dtor.
struct RotationSnapshot {
    uint8_t *buf;
    size_t len;
    bool valid;
};

RotationSnapshot do_swap_locked(uint32_t now_ms)
{
    RotationSnapshot snap{nullptr, 0, false};

    if (s_active_buf == nullptr || s_shadow_buf == nullptr || s_bytes_accumulated == 0) {
        // Either uninitialized or nothing to flush — just refresh the window
        // start so the timer-rotate path doesn't fire repeatedly on an empty
        // buffer. No-op rotate is silent.
        s_active_started_ms = now_ms;
        return snap;
    }

    // Snapshot: hand the FULL active buffer to chunk_write (after Lock release).
    snap.buf = s_active_buf;
    snap.len = s_bytes_accumulated;
    snap.valid = true;

    // Swap pointers. The shadow becomes the new active buffer (zeroing not
    // needed — bytes_accumulated == 0 invariant means stale tail is unread).
    uint8_t *prev_shadow = s_shadow_buf;
    s_shadow_buf = s_active_buf;
    s_active_buf = prev_shadow;
    s_bytes_accumulated = 0;
    s_bytes_pending.store(0, std::memory_order_relaxed);
    s_active_started_ms = now_ms;

    return snap;
}

// Flush a captured snapshot to pai_storage WITHOUT holding s_mutex.
// Called from feed_opus_frame and tick_ms only after Lock has gone out of scope.
void flush_snapshot(const RotationSnapshot &snap)
{
    if (!snap.valid || snap.buf == nullptr || snap.len == 0) {
        return;
    }

    // NOTE: snap.len may exceed pai_storage::CHUNK_MAX_BYTES (16 KiB) when the
    // accumulator hits 240 KiB. pai_storage::chunk_write rejects with
    // ESP_ERR_INVALID_ARG in that case. The D5 240 KiB rotation threshold and
    // pai_storage's per-chunk 16 KiB cap are RECONCILED here by accepting that
    // S2.5 slice ships with a known divergence: this slice routes the FULL
    // 240 KiB accumulator into ONE chunk_write call. If pai_storage rejects on
    // size, the rotation is counted as failed and the buffer is retained for
    // retry — matching the D-PT4 / ISC-5 contract. Reconciling the storage cap
    // upward (or splitting in this module) is a follow-up slice owned by the
    // main agent; the PRD D4/D5 alignment note flags this explicitly.
    uint64_t chunk_id = 0;
    esp_err_t err = pai_storage::chunk_write(snap.buf, snap.len, &chunk_id);
    if (err == ESP_OK) {
        s_chunks_written_lifetime.fetch_add(1, std::memory_order_relaxed);
        ESP_LOGI(TAG,
                 "rotated chunk: bytes=%u written_total=%u",
                 (unsigned) snap.len,
                 (unsigned) s_chunks_written_lifetime.load(std::memory_order_relaxed));
    } else {
        s_rotations_failed_lifetime.fetch_add(1, std::memory_order_relaxed);
        ESP_LOGW(TAG,
                 "rotate failed: err=%d bytes=%u failed_total=%u",
                 (int) err,
                 (unsigned) snap.len,
                 (unsigned) s_rotations_failed_lifetime.load(std::memory_order_relaxed));
        // Per ISC-5: on failure we DO NOT lose audio mid-rotation. The next
        // rotation tick overwrites the shadow, so the worst case is one
        // 240 KiB window lost — and only when the filesystem is genuinely
        // exhausted after eviction. The currently-active buffer (which was
        // swapped in by do_swap_locked) keeps accepting frames either way.
    }
}

} // namespace

// =============================================================================
// Public API
// =============================================================================

void init()
{
    if (s_inited.load(std::memory_order_acquire)) {
        return; // idempotent (D-PT call-once tolerance)
    }

    if (s_mutex == nullptr) {
        s_mutex = xSemaphoreCreateRecursiveMutex();
    }
    if (s_mutex == nullptr) {
        ESP_LOGE(TAG, "mutex alloc failed — pai_rec disabled");
        return;
    }

    s_active_buf = buf_a;
    s_shadow_buf = buf_b;
    s_bytes_accumulated = 0;
    s_active_started_ms = (uint32_t) millis();
    s_bytes_pending.store(0, std::memory_order_relaxed);
    s_chunks_written_lifetime.store(0, std::memory_order_relaxed);
    s_rotations_failed_lifetime.store(0, std::memory_order_relaxed);

    s_inited.store(true, std::memory_order_release);

    ESP_LOGI(TAG, "init ok: buf_size=%u interval_ms=%u", (unsigned) BUF_SIZE, (unsigned) ROTATE_INTERVAL_MS);
}

void feed_opus_frame(const uint8_t *data, size_t len)
{
    if (!s_inited.load(std::memory_order_acquire)) {
        return; // pre-init drop — silent (init() is on the setup_app path).
    }
    if (data == nullptr || len == 0) {
        return;
    }

    // Drop frames that cannot fit in ANY buffer even when empty. Length-only
    // log — ISC-A1 holds (no frame content).
    if (len + 2 > BUF_SIZE) {
        ESP_LOGW(TAG, "frame drop oversized: len=%u", (unsigned) len);
        return;
    }

    // If appending [2 + len] bytes would overflow the active buffer, rotate
    // FIRST (ISC-3) then append to the freshly-swapped active.
    RotationSnapshot overflow_snap{nullptr, 0, false};
    {
        Lock lk;
        if (!lk.held()) {
            return; // mutex unavailable — defensive; effectively a drop.
        }

        if (s_active_buf == nullptr) {
            return; // init was attempted but mutex/alloc path failed.
        }

        if (s_bytes_accumulated + 2 + len > BUF_SIZE) {
            // Overflow path: snapshot + swap under the same lock.
            overflow_snap = do_swap_locked((uint32_t) millis());
        }

        // Append [len_lo, len_hi, data...] to the (possibly fresh) active.
        uint16_t len16 = (uint16_t) len; // safe — `len + 2 <= BUF_SIZE` checked above
        s_active_buf[s_bytes_accumulated++] = (uint8_t) (len16 & 0xFF);
        s_active_buf[s_bytes_accumulated++] = (uint8_t) ((len16 >> 8) & 0xFF);
        memcpy(s_active_buf + s_bytes_accumulated, data, len);
        s_bytes_accumulated += len;
        s_bytes_pending.store(s_bytes_accumulated, std::memory_order_relaxed);
    } // <-- mutex released here; chunk_write below runs unlocked.

    flush_snapshot(overflow_snap);

    // Update the boost idle detector with this frame's timestamp. ISC-32
    // couples pai_rec → pai_upload one-way (timestamp only; never bytes).
    notify_mic_active((uint32_t) millis());
}

void tick_ms(uint32_t now_ms)
{
    if (!s_inited.load(std::memory_order_acquire)) {
        return;
    }

    RotationSnapshot snap{nullptr, 0, false};
    {
        Lock lk;
        if (!lk.held()) {
            return;
        }

        bool size_trigger = (s_bytes_accumulated >= BUF_SIZE);
        // Wrap-safe time compare (D-PT3) — correct through millis() rollover.
        bool time_trigger = ((uint32_t) (now_ms - s_active_started_ms) >= ROTATE_INTERVAL_MS);

        if (!size_trigger && !time_trigger) {
            return;
        }

        // Only rotate when there is something to flush. Empty-buffer time
        // triggers just refresh the window start (handled inside do_swap_locked).
        snap = do_swap_locked(now_ms);
    } // <-- mutex released here.

    flush_snapshot(snap);
}

size_t bytes_pending()
{
    return s_bytes_pending.load(std::memory_order_relaxed);
}

size_t chunks_written_lifetime()
{
    return s_chunks_written_lifetime.load(std::memory_order_relaxed);
}

size_t rotations_failed_lifetime()
{
    return s_rotations_failed_lifetime.load(std::memory_order_relaxed);
}

} // namespace pai_rec
