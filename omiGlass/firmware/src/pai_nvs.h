// =============================================================================
// pai_nvs.h — Pendant Nova NVS storage layer
// =============================================================================
// Read-only-at-runtime access to the "pn_wifi" namespace. Provisioning is
// performed out-of-band by scripts/provision_nvs.py before flashing the app.
//
// Schema (namespace = "pn_wifi"):
//   ssid_0           string  max 64 bytes
//   psk_0            string  max 64 bytes
//   ssid_1           string  max 64 bytes
//   psk_1            string  max 64 bytes
//   upload_token     blob    32 bytes (HMAC key)
//   upload_endpoint  string  max 128 bytes (null-terminated URL)
//
// Security invariants:
//   * Firmware NEVER writes to this namespace at runtime (ISC-A7).
//   * SSIDs/PSKs/tokens NEVER printed to Serial (ISC-A1).
//   * get_upload_token() zeros the destination on failure to avoid stale leaks.
//
// All functions return ESP_OK on success or a standard esp_err_t (typically
// ESP_ERR_NVS_NOT_FOUND for missing keys, ESP_ERR_NVS_INVALID_LENGTH if the
// caller-supplied buffer is too small).
// =============================================================================

#ifndef PAI_NVS_H
#define PAI_NVS_H

#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>

namespace pai_nvs
{

// Schema constants (mirror provision_nvs.py).
// NB: prefixed to avoid clashing with esp_wifi_types.h's MAX_SSID_LEN macro
// (which defines a hard 32-byte cap for the driver's wifi_config_t buffers).
static constexpr size_t NVS_MAX_SSID_LEN = 64;
static constexpr size_t NVS_MAX_PSK_LEN = 64;
static constexpr size_t UPLOAD_TOKEN_LEN = 32;
static constexpr size_t NVS_MAX_ENDPOINT_LEN = 128;
static constexpr uint8_t PROFILE_COUNT = 2;

// Initialize NVS and open the "pn_wifi" namespace read-only.
// Safe to call multiple times; subsequent calls are no-ops.
// Renamed from init() to begin() because Arduino.h declares a global
// void init(void) that would create an ambiguating overload.
// Returns ESP_OK if the namespace exists, ESP_ERR_NVS_NOT_FOUND if the device
// has never been provisioned.
esp_err_t begin();

// Read profile `idx` (0 or 1) into caller-supplied buffers.
// Buffers are zeroed before reading. On any error both buffers are zeroed
// before return.
//   ssid_max / psk_max  : capacity of caller buffer (incl. null terminator)
// Returns ESP_OK if both ssid_N and psk_N keys exist and fit; otherwise
// ESP_ERR_NVS_NOT_FOUND or ESP_ERR_NVS_INVALID_LENGTH.
esp_err_t get_profile(uint8_t idx, char *ssid, size_t ssid_max, char *psk, size_t psk_max);

// Cheap "does this profile exist?" probe — only checks ssid_N key.
// Does NOT read PSK. Safe to call before get_profile().
bool has_profile(uint8_t idx);

// Read 32-byte upload token. ALWAYS zeros `buf` first; if the key is missing
// or any other error occurs, buf stays zeroed.
esp_err_t get_upload_token(uint8_t buf[UPLOAD_TOKEN_LEN]);

// Read upload endpoint URL (null-terminated). `buf` is zeroed before read.
esp_err_t get_upload_endpoint(char *buf, size_t max);

} // namespace pai_nvs

#endif // PAI_NVS_H
