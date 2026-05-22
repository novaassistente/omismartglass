// =============================================================================
// pai_rec.h — Pendant Nova REC accumulator (S2.5)
// =============================================================================
// Second sink for Opus frames produced by the encoder callback. Frames are
// length-prefix-framed (2-byte little-endian length + raw Opus bytes) into a
// double-buffered accumulator. When the active buffer hits the D5 rotation
// threshold (CHUNK_ROTATE_BYTES or CHUNK_ROTATE_INTERVAL_MS, whichever first)
// the buffers swap and pai_storage::chunk_write is called on the closed
// snapshot OUTSIDE the swap mutex.
//
// Architecture decisions respected (project_pendant_architecture_decisions_locked_2026-05-21):
//   * D1 REC half: this module runs on the existing Arduino loopTask
//     (core 1, high prio by default). NO new FreeRTOS task is spawned.
//   * D5: rotate at CHUNK_ROTATE_BYTES (240 KiB) OR CHUNK_ROTATE_INTERVAL_MS
//     (60 s), whichever first.
//   * D6: pai_storage already filters .tmp out of chunk_read_next, so the
//     "chunk being appended is never seen by uploader" invariant is held
//     structurally — this module just feeds chunk_write with closed buffers.
//
// Threading model:
//   * feed_opus_frame is called from the Opus encoder callback on loopTask
//     (single producer).
//   * tick_ms is called from the main loop on the same task. Despite the
//     single-task call pattern today, a FreeRTOS recursive mutex guards the
//     buffer-swap critical section — defensive for any future call site
//     migration (e.g. a dedicated REC task) and for the observability getters
//     that may be read from other cores.
//   * chunk_write runs OUTSIDE the mutex on a captured snapshot pointer
//     (D-PT4 in PRD): swap pointers fast under mutex, write asynchronously
//     outside, so feed_opus_frame on the new active buffer keeps flowing
//     with zero stall.
//
// Anti-leak invariants (ISC-A1, ISC-A2):
//   * NO Opus frame bytes or accumulated buffer contents appear in any
//     Serial.print* or ESP_LOG line. Only counters and frame/buffer lengths.
// =============================================================================

#ifndef PAI_REC_H
#define PAI_REC_H

#include <stddef.h>
#include <stdint.h>

namespace pai_rec
{

// One-time initialization. MUST be called from setup_app() AFTER
// pai_storage::begin(). Allocates the FreeRTOS recursive mutex and resets
// counters. Idempotent — subsequent calls are no-ops.
void init();

// Append a freshly-encoded Opus frame `data[0..len)` to the active buffer.
// Framing on disk = 2-byte little-endian length prefix + raw `len` bytes.
//
// Overflow guard (ISC-3): if appending [2 + len] bytes would exceed
// CHUNK_ROTATE_BYTES, rotate() is triggered FIRST then the frame is appended
// to the new active buffer.
//
// Drop policy (defensive): frames larger than CHUNK_ROTATE_BYTES - 2 cannot
// fit in any buffer and are silently dropped (length-only logged at
// ESP_LOG_WARN). Real Opus frames at 24 kbps stay well under 1 KiB so this
// branch is unreachable in normal operation.
//
// Side effect: on every successful append, the boost idle detector hook in
// pai_upload is notified (mic-active touch) so the D3 adaptive boost logic
// can distinguish "buffer has audio" from "buffer idle". See pai_upload.h
// (Otto's parallel slice) for the receiving end.
void feed_opus_frame(const uint8_t *data, size_t len);

// Tick from the main loop. Triggers rotate() if the active buffer has
// reached CHUNK_ROTATE_BYTES OR the elapsed-time since active_started_ms
// has reached CHUNK_ROTATE_INTERVAL_MS. Use millis() at the call site.
//
// Wrap-safe (D-PT3): comparisons use `(uint32_t)(now - last) >= threshold`
// which is correct through the 49-day millis() rollover (modular subtraction
// on unsigned 32-bit).
void tick_ms(uint32_t now_ms);

// -----------------------------------------------------------------------------
// Observability (ISC-7) — cross-core safe via std::atomic<size_t> internally
// -----------------------------------------------------------------------------

// Current accumulated_bytes in the active buffer (NOT including the closed
// snapshot that might be mid-flight to chunk_write). Range: 0..CHUNK_ROTATE_BYTES.
size_t bytes_pending();

// Total number of closed buffers successfully handed to pai_storage::chunk_write
// since boot. Increments only on chunk_write returning ESP_OK.
size_t chunks_written_lifetime();

// Total number of rotation attempts that FAILED at the chunk_write boundary
// (e.g. ESP_ERR_NO_MEM from a full filesystem after eviction). The active
// buffer is retained for retry on the next rotation tick — audio is not lost.
size_t rotations_failed_lifetime();

} // namespace pai_rec

#endif // PAI_REC_H
