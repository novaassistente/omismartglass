// =============================================================================
// pai_storage.cpp — Pendant Nova LittleFS chunk store (S2)
// =============================================================================
// Anti-leak invariants (ISC-A1, ISC-A8):
//   * Search this file for Serial.print*. Every call site logs ONLY counters,
//     status codes, or fixed strings — NEVER buf/len/filename of chunk data.
//   * Filenames are derived from a monotonic clock + sequence, never from
//     content. No hashes, no checksums in log output.
// =============================================================================

#include "pai_storage.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

namespace pai_storage
{

namespace
{

// -----------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------
constexpr const char *MOUNT_POINT = "/littlefs";
constexpr const char *PARTITION_LABEL = "littlefs";
constexpr const char *CHUNKS_DIR = "/littlefs/chunks";
constexpr const char *CHUNK_EXT = ".opus";
constexpr const char *TMP_EXT = ".tmp";
constexpr size_t MAX_OPEN_FILES = 5;

// Filename schema: <unix_ms_zero_padded_13>_<seq_4digits>.opus
//   "0000123456789_0000.opus"  → 13 + 1 + 4 + 5 = 23 bytes + NUL
constexpr size_t FILENAME_BUF_LEN = 32;
constexpr size_t FULL_PATH_BUF_LEN = 64; // "/littlefs/chunks/" + FILENAME

// -----------------------------------------------------------------------------
// Shared state — protected by s_mutex
// -----------------------------------------------------------------------------
SemaphoreHandle_t s_mutex = nullptr;
bool s_mounted = false;
size_t s_pending_count = 0;
size_t s_evicted_lifetime = 0;
uint16_t s_boot_seq = 0; // monotonic per-boot sequence number

// -----------------------------------------------------------------------------
// Mutex RAII guard — releases on scope exit, including error paths
// -----------------------------------------------------------------------------
class Lock
{
public:
    explicit Lock(TickType_t wait = portMAX_DELAY) : ok_(false)
    {
        if (s_mutex != nullptr) {
            ok_ = (xSemaphoreTake(s_mutex, wait) == pdTRUE);
        }
    }
    ~Lock()
    {
        if (ok_ && s_mutex != nullptr) {
            xSemaphoreGive(s_mutex);
        }
    }
    bool held() const { return ok_; }

    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;

private:
    bool ok_;
};

// -----------------------------------------------------------------------------
// Filename + id helpers
// -----------------------------------------------------------------------------
uint64_t now_ms()
{
    timeval tv;
    gettimeofday(&tv, nullptr);
    // If wall-clock time has not been set (no WiFi/NTP yet), gettimeofday
    // still returns monotonically increasing values via esp_timer underneath,
    // so this remains useful for ordering even pre-NTP.
    return (uint64_t) tv.tv_sec * 1000ULL + (uint64_t) tv.tv_usec / 1000ULL;
}

// Encode chunk id as: high 50 bits = unix-ms timestamp (truncated to 50b),
// low 14 bits = boot-local sequence (0..16383). 14 bits of seq covers
// CHUNK_CAP_COUNT (256) with vast headroom.
uint64_t encode_id(uint64_t ts_ms, uint16_t seq)
{
    return (ts_ms << 14) | (uint64_t) (seq & 0x3FFF);
}

void decode_id(uint64_t id, uint64_t *ts_ms, uint16_t *seq)
{
    *ts_ms = id >> 14;
    *seq = (uint16_t) (id & 0x3FFF);
}

// Build "0000123456789_0000.opus" or ".tmp" form into `out`.
// Returns false on snprintf truncation.
bool build_filename(uint64_t ts_ms, uint16_t seq, const char *ext, char *out, size_t out_max)
{
    int n = snprintf(out, out_max, "%013llu_%04u%s", (unsigned long long) ts_ms, (unsigned) (seq & 0x3FFF), ext);
    return n > 0 && (size_t) n < out_max;
}

bool build_full_path(const char *fname, char *out, size_t out_max)
{
    int n = snprintf(out, out_max, "%s/%s", CHUNKS_DIR, fname);
    return n > 0 && (size_t) n < out_max;
}

// Parse "0000123456789_0000.opus" or ".tmp". Returns true on success.
bool parse_filename(const char *name, uint64_t *ts_ms, uint16_t *seq, bool *is_tmp)
{
    if (name == nullptr) {
        return false;
    }
    // Expected lengths: 13 digits + "_" + 4 digits + ".opus" (5) or ".tmp" (4)
    size_t len = strlen(name);
    if (len < 13 + 1 + 4 + 4) { // shortest is ".tmp"
        return false;
    }
    // Validate digit prefix
    for (size_t i = 0; i < 13; ++i) {
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
    }
    if (name[13] != '_') {
        return false;
    }
    for (size_t i = 14; i < 18; ++i) {
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
    }
    if (strcmp(name + 18, CHUNK_EXT) == 0) {
        *is_tmp = false;
    } else if (strcmp(name + 18, TMP_EXT) == 0) {
        *is_tmp = true;
    } else {
        return false;
    }

    uint64_t t = 0;
    for (size_t i = 0; i < 13; ++i) {
        t = t * 10ULL + (uint64_t) (name[i] - '0');
    }
    uint16_t s = 0;
    for (size_t i = 14; i < 18; ++i) {
        s = (uint16_t) (s * 10u + (uint16_t) (name[i] - '0'));
    }
    *ts_ms = t;
    *seq = s;
    return true;
}

// -----------------------------------------------------------------------------
// Directory walks (caller must hold s_mutex)
// -----------------------------------------------------------------------------

// Count .opus entries, optionally scrub .tmp entries on the way. Updates
// caller-supplied counters. Returns false on directory-open failure.
bool scan_chunks_dir(size_t *out_opus_count, size_t *out_tmp_scrubbed)
{
    if (out_opus_count != nullptr) {
        *out_opus_count = 0;
    }
    if (out_tmp_scrubbed != nullptr) {
        *out_tmp_scrubbed = 0;
    }

    File dir = LittleFS.open(CHUNKS_DIR);
    if (!dir || !dir.isDirectory()) {
        return false;
    }

    File entry = dir.openNextFile();
    while (entry) {
        const char *full = entry.name(); // returns "/littlefs/chunks/xxx" or "xxx" depending on core ver
        // arduino-esp32 LittleFS returns leaf name from openNextFile() on
        // recent cores; handle both by finding the last '/'.
        const char *leaf = strrchr(full, '/');
        leaf = (leaf != nullptr) ? (leaf + 1) : full;

        uint64_t ts_ms = 0;
        uint16_t seq = 0;
        bool is_tmp = false;
        bool ok = parse_filename(leaf, &ts_ms, &seq, &is_tmp);
        if (ok) {
            if (is_tmp) {
                // Premortem S2-P3 recovery: scrub orphan .tmp from power-loss.
                char path[FULL_PATH_BUF_LEN];
                if (build_full_path(leaf, path, sizeof(path))) {
                    entry.close();
                    if (LittleFS.remove(path) && out_tmp_scrubbed != nullptr) {
                        ++(*out_tmp_scrubbed);
                    }
                    entry = dir.openNextFile();
                    continue;
                }
            } else if (out_opus_count != nullptr) {
                ++(*out_opus_count);
            }
        }
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();
    return true;
}

// Find the OLDEST .opus chunk (lowest timestamp+seq). Writes filename to
// `out_name` and id to `*out_id`. Returns false if no chunks present.
bool find_oldest_opus(char *out_name, size_t out_max, uint64_t *out_id)
{
    File dir = LittleFS.open(CHUNKS_DIR);
    if (!dir || !dir.isDirectory()) {
        return false;
    }

    bool found = false;
    uint64_t best_ts = UINT64_MAX;
    uint16_t best_seq = UINT16_MAX;
    char best_leaf[FILENAME_BUF_LEN] = {0};

    File entry = dir.openNextFile();
    while (entry) {
        const char *full = entry.name();
        const char *leaf = strrchr(full, '/');
        leaf = (leaf != nullptr) ? (leaf + 1) : full;

        uint64_t ts_ms = 0;
        uint16_t seq = 0;
        bool is_tmp = false;
        if (parse_filename(leaf, &ts_ms, &seq, &is_tmp) && !is_tmp) {
            if (!found || ts_ms < best_ts || (ts_ms == best_ts && seq < best_seq)) {
                found = true;
                best_ts = ts_ms;
                best_seq = seq;
                strncpy(best_leaf, leaf, sizeof(best_leaf) - 1);
                best_leaf[sizeof(best_leaf) - 1] = '\0';
            }
        }
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();

    if (!found) {
        return false;
    }
    if (out_name != nullptr) {
        strncpy(out_name, best_leaf, out_max - 1);
        out_name[out_max - 1] = '\0';
    }
    if (out_id != nullptr) {
        *out_id = encode_id(best_ts, best_seq);
    }
    return true;
}

// Evict the oldest .opus chunk if present. Caller holds mutex. Returns true
// if a chunk was actually evicted.
bool evict_oldest_unlocked()
{
    char leaf[FILENAME_BUF_LEN] = {0};
    uint64_t id = 0;
    if (!find_oldest_opus(leaf, sizeof(leaf), &id)) {
        return false;
    }
    char path[FULL_PATH_BUF_LEN] = {0};
    if (!build_full_path(leaf, path, sizeof(path))) {
        return false;
    }
    if (!LittleFS.remove(path)) {
        return false;
    }
    if (s_pending_count > 0) {
        --s_pending_count;
    }
    ++s_evicted_lifetime;
    return true;
}

// Returns true if free space is below the configured floor.
bool below_free_floor()
{
    size_t total = LittleFS.totalBytes();
    size_t used = LittleFS.usedBytes();
    if (total == 0) {
        return false;
    }
    size_t free_bytes = (used < total) ? (total - used) : 0;
    // free < total * CAP_PCT / 100
    return free_bytes * 100 < total * (size_t) CHUNK_CAP_FREE_PCT;
}

// Ensure /littlefs/chunks/ exists. Returns false on hard failure.
bool ensure_chunks_dir()
{
    if (LittleFS.exists(CHUNKS_DIR)) {
        return true;
    }
    // mkdir returns true on success; arduino-esp32 maps to vfs_mkdir.
    return LittleFS.mkdir(CHUNKS_DIR);
}

} // namespace

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------
esp_err_t begin()
{
    if (s_mounted) {
        return ESP_OK;
    }

    if (s_mutex == nullptr) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == nullptr) {
            Serial.println("[STORAGE] mutex alloc failed");
            return ESP_ERR_NO_MEM;
        }
    }

    // First mount attempt without format-on-fail.
    bool ok = LittleFS.begin(/*formatOnFail=*/false, MOUNT_POINT, MAX_OPEN_FILES, PARTITION_LABEL);
    if (!ok) {
        // Premortem S2-P1 recovery: format and retry exactly once.
        Serial.println("[STORAGE] mount failed, formatting and retrying once");
        ok = LittleFS.begin(/*formatOnFail=*/true, MOUNT_POINT, MAX_OPEN_FILES, PARTITION_LABEL);
        if (!ok) {
            Serial.println("[STORAGE] mount failed after format retry");
            return ESP_FAIL;
        }
    }

    Lock lock;
    if (!lock.held()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!ensure_chunks_dir()) {
        Serial.println("[STORAGE] mkdir /chunks failed");
        return ESP_FAIL;
    }

    // Boot-time scan: count .opus and scrub orphan .tmp.
    size_t opus_count = 0;
    size_t tmp_scrubbed = 0;
    if (!scan_chunks_dir(&opus_count, &tmp_scrubbed)) {
        // Dir open failed despite mkdir success — treat as empty but log.
        Serial.println("[STORAGE] dir scan failed at boot");
    }
    s_pending_count = opus_count;

    if (tmp_scrubbed > 0) {
        Serial.printf("[STORAGE] scrubbed %u orphan .tmp files\n", (unsigned) tmp_scrubbed);
    }

    s_mounted = true;
    return ESP_OK;
}

esp_err_t chunk_write(const uint8_t *buf, size_t len, uint64_t *out_chunk_id)
{
    if (buf == nullptr || len == 0 || len > CHUNK_MAX_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    Lock lock;
    if (!lock.held()) {
        return ESP_ERR_INVALID_STATE;
    }

    // Eviction policy — premortem S2-P4. Drop oldest until under both caps.
    // We bound eviction iterations to avoid pathological loops on a corrupt
    // filesystem (defensive: should converge in at most a handful of evictions).
    constexpr size_t MAX_EVICT_PER_WRITE = 8;
    for (size_t i = 0; i < MAX_EVICT_PER_WRITE; ++i) {
        bool need_count = s_pending_count >= CHUNK_CAP_COUNT;
        bool need_free = below_free_floor();
        if (!need_count && !need_free) {
            break;
        }
        if (!evict_oldest_unlocked()) {
            break; // nothing to evict
        }
    }

    // Build filename + paths. Use millis-resolution wall clock; seq disambiguates
    // sub-millisecond writes and resets per boot.
    uint64_t ts_ms = now_ms();
    uint16_t seq = (s_boot_seq++) & 0x3FFF;

    char tmp_leaf[FILENAME_BUF_LEN] = {0};
    char opus_leaf[FILENAME_BUF_LEN] = {0};
    if (!build_filename(ts_ms, seq, TMP_EXT, tmp_leaf, sizeof(tmp_leaf)) ||
        !build_filename(ts_ms, seq, CHUNK_EXT, opus_leaf, sizeof(opus_leaf))) {
        return ESP_FAIL;
    }
    char tmp_path[FULL_PATH_BUF_LEN] = {0};
    char opus_path[FULL_PATH_BUF_LEN] = {0};
    if (!build_full_path(tmp_leaf, tmp_path, sizeof(tmp_path)) ||
        !build_full_path(opus_leaf, opus_path, sizeof(opus_path))) {
        return ESP_FAIL;
    }

    // Write .tmp
    File f = LittleFS.open(tmp_path, "w", /*create=*/true);
    if (!f) {
        return ESP_FAIL;
    }
    size_t written = f.write(buf, len);
    f.flush();
    f.close();
    if (written != len) {
        // Partial write — most likely filesystem full despite eviction (e.g.
        // metadata overhead spike). Scrub the .tmp we just created.
        LittleFS.remove(tmp_path);
        return ESP_ERR_NO_MEM;
    }

    // Atomic rename .tmp → .opus
    if (!LittleFS.rename(tmp_path, opus_path)) {
        LittleFS.remove(tmp_path);
        return ESP_FAIL;
    }

    ++s_pending_count;
    if (out_chunk_id != nullptr) {
        *out_chunk_id = encode_id(ts_ms, seq);
    }
    return ESP_OK;
}

esp_err_t chunk_read_next(uint8_t *buf, size_t max_len, size_t *out_len, uint64_t *out_id)
{
    if (buf == nullptr || out_len == nullptr || out_id == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    Lock lock;
    if (!lock.held()) {
        return ESP_ERR_INVALID_STATE;
    }

    char leaf[FILENAME_BUF_LEN] = {0};
    uint64_t id = 0;
    if (!find_oldest_opus(leaf, sizeof(leaf), &id)) {
        return ESP_ERR_NOT_FOUND;
    }

    char path[FULL_PATH_BUF_LEN] = {0};
    if (!build_full_path(leaf, path, sizeof(path))) {
        return ESP_FAIL;
    }

    File f = LittleFS.open(path, "r");
    if (!f) {
        return ESP_FAIL;
    }
    size_t file_size = f.size();
    if (file_size > max_len) {
        f.close();
        return ESP_ERR_INVALID_SIZE;
    }
    size_t read_n = f.read(buf, file_size);
    f.close();
    if (read_n != file_size) {
        return ESP_FAIL;
    }
    *out_len = file_size;
    *out_id = id;
    return ESP_OK;
}

esp_err_t chunk_delete(uint64_t chunk_id)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    Lock lock;
    if (!lock.held()) {
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t ts_ms = 0;
    uint16_t seq = 0;
    decode_id(chunk_id, &ts_ms, &seq);

    char leaf[FILENAME_BUF_LEN] = {0};
    if (!build_filename(ts_ms, seq, CHUNK_EXT, leaf, sizeof(leaf))) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[FULL_PATH_BUF_LEN] = {0};
    if (!build_full_path(leaf, path, sizeof(path))) {
        return ESP_FAIL;
    }

    if (!LittleFS.exists(path)) {
        // Idempotent — not an error.
        return ESP_OK;
    }
    if (!LittleFS.remove(path)) {
        return ESP_FAIL;
    }
    if (s_pending_count > 0) {
        --s_pending_count;
    }
    return ESP_OK;
}

size_t chunks_pending_count()
{
    // Single word read of a cached counter — no lock needed on Xtensa LX7
    // (aligned word load is atomic). The mutator paths inside chunk_write /
    // chunk_delete are mutex-protected against each other.
    return s_pending_count;
}

size_t chunks_evicted_lifetime()
{
    return s_evicted_lifetime;
}

} // namespace pai_storage
