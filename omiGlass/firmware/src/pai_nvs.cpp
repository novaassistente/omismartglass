// =============================================================================
// pai_nvs.cpp — Pendant Nova NVS storage layer (read-only at runtime)
// =============================================================================
// IMPORTANT: This translation unit MUST NOT log SSID / PSK / token values.
// Only key names, error codes, and lengths are permitted in Serial output.
// =============================================================================

#include "pai_nvs.h"

#include <Arduino.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <string.h>

namespace pai_nvs
{

namespace
{
constexpr const char *NAMESPACE = "pn_wifi";
constexpr const char *KEY_TOKEN = "upload_token";
constexpr const char *KEY_ENDPOINT = "upload_endpoint";

bool s_initialized = false;

// Build a key name for profile `idx` into `out`. Returns false if idx >= PROFILE_COUNT.
bool build_profile_key(const char *prefix, uint8_t idx, char *out, size_t out_max)
{
    if (idx >= PROFILE_COUNT) {
        return false;
    }
    // Result format: "ssid_0", "psk_1", etc. Always short — bounded by literal prefix + digit.
    int n = snprintf(out, out_max, "%s_%u", prefix, (unsigned) idx);
    return n > 0 && (size_t) n < out_max;
}

esp_err_t ensure_flash_init()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // Partition truncated/upgraded — erase & retry. This is the only
        // write-class operation in this module and it only fires on flash
        // corruption, never on normal boot.
        Serial.println("[NVS] flash incompatible, erasing");
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            return err;
        }
        err = nvs_flash_init();
    }
    return err;
}

// Read a string key into caller buf. On any failure zeros buf and returns
// the underlying error. `max` includes space for null terminator.
esp_err_t read_string(nvs_handle_t h, const char *key, char *buf, size_t max)
{
    if (buf == nullptr || max == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(buf, 0, max);

    size_t required = max;
    esp_err_t err = nvs_get_str(h, key, buf, &required);
    if (err != ESP_OK) {
        memset(buf, 0, max);
    }
    return err;
}

} // namespace

esp_err_t begin()
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = ensure_flash_init();
    if (err != ESP_OK) {
        Serial.printf("[NVS] flash init failed err=0x%x\n", err);
        return err;
    }

    // Quickly verify the namespace exists by opening read-only.
    nvs_handle_t h;
    err = nvs_open(NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        nvs_close(h);
        s_initialized = true;
        Serial.println("[NVS] pn_wifi namespace ready");
        return ESP_OK;
    }

    // Common case on a fresh device: namespace not yet provisioned.
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        Serial.println("[NVS] pn_wifi namespace missing (device not provisioned)");
    } else {
        Serial.printf("[NVS] open failed err=0x%x\n", err);
    }
    // We still mark as initialized — subsequent reads will surface NOT_FOUND
    // cleanly without re-running nvs_flash_init.
    s_initialized = true;
    return err;
}

esp_err_t get_profile(uint8_t idx, char *ssid, size_t ssid_max, char *psk, size_t psk_max)
{
    if (ssid == nullptr || psk == nullptr || ssid_max == 0 || psk_max == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(ssid, 0, ssid_max);
    memset(psk, 0, psk_max);

    if (idx >= PROFILE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        esp_err_t err = begin();
        if (err != ESP_OK) {
            return err;
        }
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }

    char key[16];
    if (!build_profile_key("ssid", idx, key, sizeof(key))) {
        nvs_close(h);
        return ESP_ERR_INVALID_ARG;
    }
    err = read_string(h, key, ssid, ssid_max);
    if (err != ESP_OK) {
        nvs_close(h);
        memset(psk, 0, psk_max);
        return err;
    }

    if (!build_profile_key("psk", idx, key, sizeof(key))) {
        nvs_close(h);
        memset(ssid, 0, ssid_max);
        memset(psk, 0, psk_max);
        return ESP_ERR_INVALID_ARG;
    }
    err = read_string(h, key, psk, psk_max);
    nvs_close(h);
    if (err != ESP_OK) {
        // Wipe both buffers — half-loaded profile is worse than nothing.
        memset(ssid, 0, ssid_max);
        memset(psk, 0, psk_max);
    }
    return err;
}

bool has_profile(uint8_t idx)
{
    if (idx >= PROFILE_COUNT) {
        return false;
    }
    if (!s_initialized) {
        if (begin() != ESP_OK) {
            return false;
        }
    }

    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }

    char key[16];
    if (!build_profile_key("ssid", idx, key, sizeof(key))) {
        nvs_close(h);
        return false;
    }

    size_t required = 0;
    esp_err_t err = nvs_get_str(h, key, nullptr, &required);
    nvs_close(h);
    return (err == ESP_OK) && (required > 1); // > 1 == at least one char + NUL
}

esp_err_t get_upload_token(uint8_t buf[UPLOAD_TOKEN_LEN])
{
    if (buf == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(buf, 0, UPLOAD_TOKEN_LEN);

    if (!s_initialized) {
        esp_err_t err = begin();
        if (err != ESP_OK) {
            return err;
        }
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }

    size_t required = UPLOAD_TOKEN_LEN;
    err = nvs_get_blob(h, KEY_TOKEN, buf, &required);
    nvs_close(h);

    if (err != ESP_OK || required != UPLOAD_TOKEN_LEN) {
        memset(buf, 0, UPLOAD_TOKEN_LEN);
        return (err != ESP_OK) ? err : ESP_ERR_NVS_INVALID_LENGTH;
    }
    return ESP_OK;
}

esp_err_t get_upload_endpoint(char *buf, size_t max)
{
    if (buf == nullptr || max == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(buf, 0, max);

    if (!s_initialized) {
        esp_err_t err = begin();
        if (err != ESP_OK) {
            return err;
        }
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = read_string(h, KEY_ENDPOINT, buf, max);
    nvs_close(h);
    return err;
}

} // namespace pai_nvs
