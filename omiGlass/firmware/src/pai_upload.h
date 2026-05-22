// =============================================================================
// pai_upload.h — Pendant Nova HTTPS chunk uploader (S3a + S3b)
// =============================================================================
// Drains closed .opus chunks from pai_storage and POSTs them to the ingest
// server. Owns one FreeRTOS task pinned to core 0 (medium prio 5, 8 KiB
// stack) per D1 (REC never pauses; UPLOAD lives on the other core).
//
// Behavior summary:
//   * Task wakes on EITHER the configurable batch tick (NVS slot
//     UPLOAD_TICK_MS, default 15 min, min 30 s) OR a "boost" semaphore
//     fired by the idle detector (D3 adaptive boost: WiFi associated +
//     mic idle ≥ 60 s + last drain ≥ 60 s).
//   * Each drain cycle loops chunk_read_next until ESP_ERR_NOT_FOUND or a
//     fatal error. Per-chunk transport: HTTPS POST with HMAC-SHA256
//     signature over the body, 4 required headers (see hmac-contract.md).
//   * 200 ⇒ chunk_delete + uploaded_lifetime++. 401 ⇒ warn + retry once;
//     on 2nd 401 for the same chunk_id within the rolling window the
//     chunk is renamed `.poisoned` and skipped on subsequent drains
//     (D-PT5; prevents tight-loop server hammer on key rotation drift).
//     5xx / network errors ⇒ exponential backoff 1→2→4→8 s capped at
//     60 s, up to 3 retries per chunk per drain cycle.
//
// Threading model:
//   * Task pinned to core 0; pai_rec (and the Arduino main loop) live
//     on core 1. All cross-core counters are std::atomic<uint32_t>
//     with memory_order_relaxed — we need cross-core *visibility*, not
//     *ordering*, and 32-bit aligned loads/stores are lock-free on
//     Xtensa LX7. See D-PT2 in PRD §Decisions.
//   * touch_last_mic_active() is the only entry point pai_rec calls
//     from core 1; it does a single relaxed atomic store and returns.
//
// HMAC contract (canonical: ~/.claude/MEMORY/PENDANT/hmac-contract.md):
//   * Algorithm: HMAC-SHA256.
//   * Key: 32 raw bytes from pai_nvs::get_upload_token().
//   * Msg: exact body bytes (no framing).
//   * Output: uppercase hex (64 chars) in X-HMAC-SHA256 header.
//
// Self-test (ISC-43):
//   * On start_task(): compute HMAC over body="hello" with 32-zero key
//     and compare to the canonical vector 1 hex. Mismatch ⇒ refuse to
//     start, log fatal `hmac_self_test_fail`. Catches mbedtls config
//     drift at boot, before any network traffic.
//   * Additionally: if the provisioned token is all zeros (defensive —
//     means never provisioned, or NVS read returned zero-buf on error
//     per pai_nvs invariant), same refusal path.
//
// Anti-leak invariants:
//   * NEVER logs the HMAC key (raw or hex), signature, or any chunk
//     body bytes. Only chunk_id, length, action, and HTTP status.
//
// Conditional compilation:
//   * Module compiles regardless of PAI_UPLOAD_MODE. start_task() is the
//     guard — app.cpp setup_app() gates the call (main agent wiring).
// =============================================================================

#ifndef PAI_UPLOAD_H
#define PAI_UPLOAD_H

#include <stddef.h>
#include <stdint.h>

namespace pai_upload
{

// Spawn the upload task. Must be called AFTER pai_nvs::begin(),
// pai_storage::begin(), and pai_wifi::begin().
//
// First action inside the spawned task is hmac_smoke() against vector 1
// from hmac-contract.md. On mismatch (OR all-zeros token) the task logs
// a fatal `hmac_self_test_fail` and self-deletes without doing any I/O.
//
// Idempotent: subsequent calls are no-ops after the first successful
// spawn.
void start_task();

// Signal that the idle detector should consider the device eligible for
// an adaptive-boost drain on its next 1 Hz check. Cheap; pai_rec calls
// this from feed_opus_frame to keep last_mic_active_ms fresh, but it is
// safe to call from any task on any core.
//
// NB: actual boost decision lives inside the upload task's check —
// notify_boost_eligible() does NOT directly give the semaphore.
void notify_boost_eligible();

// Record that the microphone was active at `now_ms` (typically the
// caller's millis() at the moment an Opus frame was produced). Single
// 32-bit relaxed atomic store; safe to call from core 1 (pai_rec) while
// the upload task on core 0 reads the same field for the idle test.
//
// Wired by main-agent: pai_rec::feed_opus_frame ⇒ this call.
void touch_last_mic_active(uint32_t now_ms);

// True while the upload task is inside a drain cycle (between dequeue and
// HTTP completion). Lock-free read of an atomic flag; safe from any core.
// Used by power-management code (light-sleep gate) to avoid interrupting
// a mid-POST TCP/TLS handshake — esp_light_sleep_start would drop the
// connection and force a fresh re-handshake on wake.
bool is_busy();

} // namespace pai_upload

#endif // PAI_UPLOAD_H
