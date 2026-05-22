// =============================================================================
// pai_storage.h — Pendant Nova LittleFS chunk store (S2)
// =============================================================================
// Rotating store of Opus-encoded audio chunks for offline buffering while
// WiFi is unavailable. Chunks live as ordinary files in /littlefs/chunks/
// and are produced/consumed in strict timestamp order.
//
// Threading model:
//   * All public APIs are mutex-protected (single FreeRTOS mutex). Any task
//     may call begin/chunk_write/chunk_read_next/chunk_delete safely.
//   * begin() must be called once during setup before any other entry point.
//
// Atomicity (premortem S2-P3 — power loss mid-write):
//   * Writers create chunks/<ts>_<seq>.tmp, write the full payload, close,
//     then vfs_rename(.tmp -> .opus). Readers only ever return .opus files.
//   * Boot-time scan in begin() unlinks any orphan .tmp files left by a
//     previous power loss.
//   * NOTE: LittleFS rename is atomic for the directory metadata block
//     (lfs commits via CoW), but a power loss during the metadata commit
//     itself can in the worst case lose BOTH the .tmp and target .opus
//     entry. Net effect: bounded data loss of the in-flight chunk only,
//     never silent corruption of a prior committed chunk. See
//     /simplify-xhigh A-864/D-437 for the full failure model.
//
// Filename collision guard (post-/simplify-xhigh A-844/D-82/D-407):
//   * s_boot_seq is a 14-bit counter (per chunk_id encoding); after 16384
//     writes/boot it wraps. Combined with NTP wall-clock back-jump, this
//     can produce a filename matching an existing pending chunk.
//   * chunk_write pre-checks LittleFS.exists(opus_path) before rename and
//     returns ESP_ERR_INVALID_STATE on collision — caller (S3 uploader)
//     must retry. NEVER silently overwrites a pending chunk.
//
// Eviction (premortem S2-P4 — full filesystem):
//   * Before each write, if chunks_pending_count() >= CHUNK_CAP_COUNT or
//     free space drops below CHUNK_CAP_FREE_PCT, the oldest .opus file is
//     deleted and chunks_evicted_lifetime() is incremented.
//   * Eviction is logged at INFO level WITHOUT filename or content —
//     only the counter delta is visible.
//
// Anti-leak invariants (ISC-A1, ISC-A8):
//   * NO chunk payload bytes ever reach Serial.print* in this module.
//   * Filenames embed only a timestamp + sequence number — never hashes
//     derived from content or length values that could fingerprint speech.
//   * Counters (pending, evicted_lifetime) are the ONLY observability
//     surface for chunk activity.
// =============================================================================

#ifndef PAI_STORAGE_H
#define PAI_STORAGE_H

#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>

namespace pai_storage
{

// =============================================================================
// Architecture decisions D4/D5 alignment (project_pendant_architecture_decisions_locked_2026-05-21)
// =============================================================================
// D4 specified "6MB cap DROP OLDEST". Actual partition is 4.94 MiB so the
// honest byte cap below is 4 MiB — leaves ~960 KiB for LittleFS metadata and
// safety margin. This is the PRIMARY eviction gate per D4. The count and
// percent caps below remain as defense-in-depth.
//
// D5 specified "chunk rotation 60 s OR 240 KiB". REC task (scheduled for next
// session) MUST call chunk_write at MOST every CHUNK_ROTATE_INTERVAL_MS milli-
// seconds OR after accumulating CHUNK_ROTATE_BYTES of encoded Opus, whichever
// comes first. These are pure caller-side policy hooks — pai_storage itself
// only enforces per-chunk atomicity.
static constexpr size_t CHUNK_CAP_BYTES = 4u * 1024u * 1024u;     // D4 primary
static constexpr uint32_t CHUNK_ROTATE_INTERVAL_MS = 60u * 1000u;  // D5
static constexpr size_t CHUNK_ROTATE_BYTES = 240u * 1024u;         // D5

// Defense-in-depth: count cap (D4 secondary). At 4 s/chunk × 256 chunks ≈
// 17 min of buffered audio in the worst case. Bytes cap usually trips first.
static constexpr size_t CHUNK_CAP_COUNT = 256;

// Defense-in-depth: free-space watermark. When free drops below this fraction
// of partition size, evict the oldest chunk before accepting a new write.
// 20 % = ~1 MiB on a 4.94 MiB partition, above LittleFS metadata overhead.
static constexpr uint8_t CHUNK_CAP_FREE_PCT = 20;

// Hard upper bound for a single chunk payload. Sized for ~4 s of Opus at
// 24 kbps + framing overhead. Caller is responsible for splitting larger
// inputs across multiple chunks.
static constexpr size_t CHUNK_MAX_BYTES = 16 * 1024;

// Mount /littlefs. On first mount failure the partition is formatted and
// mount is retried exactly once (premortem S2-P1 recovery). Orphan .tmp
// files from a previous power loss are unlinked here.
// Idempotent; subsequent calls return ESP_OK without remounting.
esp_err_t begin();

// Atomically write `buf[0..len)` as a new chunk. On success, `*out_chunk_id`
// receives the unique 64-bit id encoded into the filename (high 50 bits =
// unix-ms timestamp, low 14 bits = boot-local monotonic sequence). `out_chunk_id`
// may be nullptr if the caller does not need the id.
//
// Returns:
//   ESP_OK                       — chunk persisted as .opus
//   ESP_ERR_INVALID_ARG          — buf null, len==0, or len > CHUNK_MAX_BYTES
//   ESP_ERR_INVALID_STATE        — begin() not yet called
//   ESP_ERR_NO_MEM               — filesystem full after eviction
//   ESP_FAIL                     — vfs_rename or write failure (chunk left as .tmp,
//                                  will be cleaned up on next begin())
esp_err_t chunk_write(const uint8_t *buf, size_t len, uint64_t *out_chunk_id);

// Read the OLDEST pending .opus chunk into `buf[0..max_len)`. The chunk is
// NOT deleted — caller must invoke chunk_delete() after successful upload.
// `.tmp` files are never returned.
//
// Returns:
//   ESP_OK                       — payload copied; *out_len holds byte count;
//                                  *out_id holds chunk id (filename-encoded)
//   ESP_ERR_NOT_FOUND            — no pending chunks
//   ESP_ERR_INVALID_ARG          — buf/out_len/out_id null
//   ESP_ERR_INVALID_SIZE         — chunk on disk exceeds max_len (caller bug)
//   ESP_ERR_INVALID_STATE        — begin() not yet called
esp_err_t chunk_read_next(uint8_t *buf, size_t max_len, size_t *out_len, uint64_t *out_id);

// Delete chunk `chunk_id`. Idempotent — returns ESP_OK if the chunk does
// not exist (already deleted or never written). The pending-count cache
// is updated atomically with the unlink.
esp_err_t chunk_delete(uint64_t chunk_id);

// Number of .opus chunks currently pending. O(1) — backed by a cached
// counter maintained by chunk_write/chunk_delete. Re-validated against
// a directory scan in begin().
size_t chunks_pending_count();

// Total chunks evicted since boot due to CAP_COUNT or CAP_FREE_PCT.
// Monotonically increasing; resets to 0 on reboot.
size_t chunks_evicted_lifetime();

} // namespace pai_storage

#endif // PAI_STORAGE_H
