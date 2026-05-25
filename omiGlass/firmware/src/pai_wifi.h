// =============================================================================
// pai_wifi.h — Pendant Nova WiFi STA driver
// =============================================================================
// State-machine driven station mode. Reads up to 2 profiles from pai_nvs and
// iterates them with 15 s connect timeout per profile. On full failure the
// task sleeps with exponential backoff (60s → 120s → 240s → cap 300s) and
// retries.
//
// Threading model:
//   * One FreeRTOS task pinned to core 0 owns all esp_wifi_* calls.
//   * BLE / audio remain on core 1 (Arduino loop default).
//   * Public getters use a portMUX spinlock; safe to call from any core.
//
// Security invariants:
//   * NEVER prints SSID or PSK to Serial (ISC-A1).
//   * NEVER writes to NVS at runtime (ISC-A7).
//   * On WIFI_EVENT_STA_DISCONNECTED, logs reason code only — never the AP.
//
// Conditional compilation:
//   * Entire driver is gated by PAI_UPLOAD_MODE in app.cpp. pai_wifi.{h,cpp}
//     still compile when the flag is 0; they just stay idle until init().
// =============================================================================

#ifndef PAI_WIFI_H
#define PAI_WIFI_H

#include <IPAddress.h>
#include <esp_err.h>
#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

namespace pai_wifi
{

// NB: Arduino.h defines DISABLED as a macro (0x00) inside esp32-hal-gpio.h.
// We must `#undef` it before declaring the enumerator. The undef is also
// applied at the top of pai_wifi.cpp to keep both translation units consistent.
#ifdef DISABLED
#undef DISABLED
#endif

enum class State : uint8_t {
    INIT = 0,
    SCANNING = 1,
    CONNECTING = 2,
    CONNECTED = 3,
    BACKOFF = 4,
    PAUSED = 5, // renamed from DISABLED to avoid the Arduino GPIO macro
};

// Initialize STA mode, register event handlers, spawn the state-machine task.
// Idempotent. Returns ESP_OK on success.
// NB: named begin() because Arduino.h declares a global void init(void).
esp_err_t begin();

// Halt the state machine: stop reconnects and disconnect if connected.
// Renamed from pause() because <unistd.h> declares int pause(void).
void suspend();

// Resume the state machine after suspend(). Resets backoff so it retries fast.
void resume();

// Snapshot of current state (lock-protected read).
State get_state();

// Quick connectivity check. True only while CONNECTED.
bool is_connected();

// True once a valid IPv4 address has been assigned (IP_EVENT_STA_GOT_IP).
// Stronger than is_connected() for callers that must wait for DHCP/DNS to be
// usable before issuing sockets — guards against the "DNS Failed" first POST.
bool is_got_ip();

// IP address (0.0.0.0 if not connected).
IPAddress get_ip();

// Last known RSSI (0 if not connected).
int8_t get_rssi();

// Blocking scan. Returns (SSID, RSSI) pairs sorted by RSSI descending.
// MUST NOT be called from the wifi task itself (will deadlock).
///
/// @param timeout_ms Maximum time to wait for the scan MUTEX (not the radio scan itself,
///                   which runs ~4s fixed: 300ms × 13 channels). If 0, return immediately
///                   if mutex is held.
std::vector<std::pair<std::string, int8_t>> scan_blocking(uint32_t timeout_ms);

} // namespace pai_wifi

#endif // PAI_WIFI_H
