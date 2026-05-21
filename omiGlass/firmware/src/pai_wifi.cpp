// =============================================================================
// pai_wifi.cpp — Pendant Nova WiFi STA driver
// =============================================================================
// Anti-leak invariants (ISC-A1):
//   * Search this file for "ssid" / "psk" — they appear ONLY as struct field
//     assignments to wifi_config_t. Zero Serial.print* references touch
//     wcfg.sta.ssid or wcfg.sta.password.
//   * Connect-success log: "[WIFI] connected idx=N ip=x.x.x.x rssi=-NN"
//   * Disconnect log:      "[WIFI] disconnected reason=<bucket>"
// =============================================================================

#include "pai_wifi.h"

#include "pai_nvs.h"

#include <Arduino.h>
// Arduino's esp32-hal-gpio.h defines DISABLED as a macro. Undef it BEFORE any
// further headers re-pull it in so our State::PAUSED enum stays clean.
#ifdef DISABLED
#undef DISABLED
#endif
#include <algorithm>
#include <esp_event.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <string.h>

namespace pai_wifi
{

namespace
{

// -----------------------------------------------------------------------------
// Tunables
// -----------------------------------------------------------------------------
constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;
constexpr uint32_t BACKOFF_INITIAL_MS = 60UL * 1000UL;
constexpr uint32_t BACKOFF_CAP_MS = 300UL * 1000UL;
constexpr uint32_t SCAN_BLOCK_DEFAULT_MS = 6000;
constexpr uint32_t TASK_STACK_BYTES = 4096;
constexpr UBaseType_t TASK_PRIORITY = 3;
constexpr BaseType_t TASK_CORE = 0; // BLE+audio run on core 1

// FreeRTOS event group bits
constexpr EventBits_t BIT_CONNECTED = BIT0;
constexpr EventBits_t BIT_DISCONNECTED = BIT1;
constexpr EventBits_t BIT_GOT_IP = BIT2;
constexpr EventBits_t BIT_PAUSE_REQ = BIT3;
constexpr EventBits_t BIT_RESUME_REQ = BIT4;

// -----------------------------------------------------------------------------
// Shared state — protected by s_mux spinlock for getters
// -----------------------------------------------------------------------------
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
State s_state = State::INIT;
uint32_t s_ip_raw = 0;
int8_t s_rssi = 0;
uint8_t s_disconnect_reason = 0;
uint8_t s_active_profile = 0xFF;

EventGroupHandle_t s_events = nullptr;
TaskHandle_t s_task = nullptr;
SemaphoreHandle_t s_scan_mutex = nullptr;
bool s_paused = false;
bool s_initialized = false;

// -----------------------------------------------------------------------------
// State helpers
// -----------------------------------------------------------------------------
void set_state(State s)
{
    portENTER_CRITICAL(&s_mux);
    s_state = s;
    portEXIT_CRITICAL(&s_mux);
}

void set_ip(uint32_t raw)
{
    portENTER_CRITICAL(&s_mux);
    s_ip_raw = raw;
    portEXIT_CRITICAL(&s_mux);
}

void set_rssi(int8_t r)
{
    portENTER_CRITICAL(&s_mux);
    s_rssi = r;
    portEXIT_CRITICAL(&s_mux);
}

void set_active_profile(uint8_t idx)
{
    portENTER_CRITICAL(&s_mux);
    s_active_profile = idx;
    portEXIT_CRITICAL(&s_mux);
}

void set_disconnect_reason(uint8_t r)
{
    portENTER_CRITICAL(&s_mux);
    s_disconnect_reason = r;
    portEXIT_CRITICAL(&s_mux);
}

// Map ESP-IDF wifi disconnect reasons into 4 buckets so logs never reveal AP
// identity via fine-grained reasons (e.g. "ASSOC_FAIL: AP=MyHomeNet").
const char *reason_bucket(uint8_t code)
{
    switch (code) {
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
        return "AUTH_FAILED";
    case WIFI_REASON_NO_AP_FOUND:
        return "AP_NOT_FOUND";
    case WIFI_REASON_BEACON_TIMEOUT:
    case WIFI_REASON_ASSOC_EXPIRE:
    case WIFI_REASON_NOT_AUTHED:
    case WIFI_REASON_NOT_ASSOCED:
        return "TIMEOUT";
    default:
        return "OTHER";
    }
}

// -----------------------------------------------------------------------------
// Hostname: pendant-nova-XXXX (last 4 hex of MAC)
// -----------------------------------------------------------------------------
void apply_hostname(esp_netif_t *netif)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        return;
    }
    char host[24];
    snprintf(host, sizeof(host), "pendant-nova-%02X%02X", mac[4], mac[5]);
    esp_netif_set_hostname(netif, host);
    Serial.printf("[WIFI] hostname=%s\n", host);
}

// -----------------------------------------------------------------------------
// Event handler — runs in esp-event task, must be quick
// -----------------------------------------------------------------------------
void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            // Driver started — connection is initiated by the state machine task.
            break;
        case WIFI_EVENT_STA_CONNECTED: {
            wifi_event_sta_connected_t *ev = (wifi_event_sta_connected_t *) data;
            // NB: ev->ssid IS the SSID — DO NOT print it.
            set_rssi(0); // populated on STA_GOT_IP via esp_wifi_sta_get_ap_info
            (void) ev;
            xEventGroupSetBits(s_events, BIT_CONNECTED);
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *) data;
            set_disconnect_reason(ev->reason);
            set_ip(0);
            set_rssi(0);
            xEventGroupSetBits(s_events, BIT_DISCONNECTED);
            break;
        }
        default:
            break;
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *) data;
        set_ip(ev->ip_info.ip.addr);

        // Pull RSSI now that we have AP info.
        wifi_ap_record_t ap = {};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            set_rssi(ap.rssi);
        }
        xEventGroupSetBits(s_events, BIT_GOT_IP);
    }
}

// -----------------------------------------------------------------------------
// Try a single profile. Blocks until connected, timed out, or disconnected.
// Returns true on success. SSID/PSK live only on the stack here.
// -----------------------------------------------------------------------------
bool try_profile(uint8_t idx)
{
    char ssid[pai_nvs::NVS_MAX_SSID_LEN + 1] = {0};
    char psk[pai_nvs::NVS_MAX_PSK_LEN + 1] = {0};

    esp_err_t err = pai_nvs::get_profile(idx, ssid, sizeof(ssid), psk, sizeof(psk));
    if (err != ESP_OK) {
        Serial.printf("[WIFI] profile %u unavailable err=0x%x\n", (unsigned) idx, err);
        // Defensive scrub (get_profile already did this, but belt+suspenders).
        memset(ssid, 0, sizeof(ssid));
        memset(psk, 0, sizeof(psk));
        return false;
    }

    Serial.printf("[WIFI] attempting profile idx=%u\n", (unsigned) idx);
    set_active_profile(idx);
    set_state(State::CONNECTING);

    wifi_config_t wcfg = {};
    // Length-bounded copies — NVS schema caps at MAX_SSID/PSK_LEN, but the
    // wifi_config_t buffers (sizeof 32 / 64) are smaller so we still bound.
    strncpy((char *) wcfg.sta.ssid, ssid, sizeof(wcfg.sta.ssid) - 1);
    strncpy((char *) wcfg.sta.password, psk, sizeof(wcfg.sta.password) - 1);
    wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wcfg.sta.pmf_cfg.capable = true;
    wcfg.sta.pmf_cfg.required = false;

    // Wipe local copies BEFORE handing off to the driver. The driver keeps
    // its own copy internally (we can't help that), but at least our stack
    // frame doesn't linger with secrets after we return.
    memset(ssid, 0, sizeof(ssid));
    memset(psk, 0, sizeof(psk));

    xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_DISCONNECTED | BIT_GOT_IP);

    esp_err_t cfg_err = esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    // Wipe the config-on-stack as soon as the driver has consumed it.
    memset(&wcfg, 0, sizeof(wcfg));
    if (cfg_err != ESP_OK) {
        Serial.printf("[WIFI] set_config err=0x%x\n", cfg_err);
        return false;
    }

    esp_err_t conn_err = esp_wifi_connect();
    if (conn_err != ESP_OK && conn_err != ESP_ERR_WIFI_CONN) {
        Serial.printf("[WIFI] connect call err=0x%x\n", conn_err);
        return false;
    }

    // Wait for GOT_IP (success) or DISCONNECTED (fail) up to CONNECT_TIMEOUT_MS.
    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_GOT_IP | BIT_DISCONNECTED, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));

    if (bits & BIT_GOT_IP) {
        // SSID is intentionally absent from this log. idx allows operator to
        // correlate without exposing the AP name.
        IPAddress ip(s_ip_raw);
        Serial.printf("[WIFI] connected idx=%u ip=%s rssi=%d\n", (unsigned) idx, ip.toString().c_str(), (int) s_rssi);
        set_state(State::CONNECTED);
        return true;
    }

    if (bits & BIT_DISCONNECTED) {
        Serial.printf("[WIFI] disconnected reason=%s\n", reason_bucket(s_disconnect_reason));
    } else {
        Serial.println("[WIFI] connect attempt timed out");
    }
    // Make sure driver is fully torn down before next attempt.
    esp_wifi_disconnect();
    return false;
}

// -----------------------------------------------------------------------------
// Main state machine task
// -----------------------------------------------------------------------------
void task_main(void *arg)
{
    (void) arg;
    uint32_t backoff_ms = BACKOFF_INITIAL_MS;
    bool was_connected = false;

    for (;;) {
        // Pause/resume check
        if (s_paused) {
            set_state(State::PAUSED);
            EventBits_t b = xEventGroupWaitBits(s_events, BIT_RESUME_REQ, pdTRUE, pdFALSE, portMAX_DELAY);
            if (b & BIT_RESUME_REQ) {
                s_paused = false;
                backoff_ms = BACKOFF_INITIAL_MS;
                set_state(State::INIT);
            }
            continue;
        }

        // If we were connected and lost the link, wait for the disconnect
        // signal then attempt immediate reconnect of the active profile.
        if (was_connected) {
            EventBits_t b = xEventGroupWaitBits(s_events, BIT_DISCONNECTED | BIT_PAUSE_REQ, pdTRUE, pdFALSE,
                                                portMAX_DELAY);
            if (b & BIT_PAUSE_REQ) {
                s_paused = true;
                continue;
            }
            was_connected = false;
            set_state(State::INIT);
            // Fall through to retry loop.
        }

        bool any_success = false;
        for (uint8_t idx = 0; idx < pai_nvs::PROFILE_COUNT; ++idx) {
            if (!pai_nvs::has_profile(idx)) {
                continue;
            }
            if (try_profile(idx)) {
                any_success = true;
                was_connected = true;
                backoff_ms = BACKOFF_INITIAL_MS; // reset on success
                break;
            }
            // Short pause between profile attempts so we don't hammer.
            vTaskDelay(pdMS_TO_TICKS(500));
            if (s_paused) {
                break;
            }
        }

        if (any_success) {
            // Stay parked until disconnect signals us back.
            continue;
        }

        // Backoff
        set_state(State::BACKOFF);
        Serial.printf("[WIFI] all profiles failed, backoff %lus\n", (unsigned long) (backoff_ms / 1000));
        // Wait with early exit on pause request.
        EventBits_t b = xEventGroupWaitBits(s_events, BIT_PAUSE_REQ, pdTRUE, pdFALSE, pdMS_TO_TICKS(backoff_ms));
        if (b & BIT_PAUSE_REQ) {
            s_paused = true;
        }
        // Exponential backoff with cap.
        backoff_ms = (backoff_ms < BACKOFF_CAP_MS) ? (backoff_ms * 2) : BACKOFF_CAP_MS;
        if (backoff_ms > BACKOFF_CAP_MS) {
            backoff_ms = BACKOFF_CAP_MS;
        }
    }
}

} // namespace

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------
esp_err_t begin()
{
    if (s_initialized) {
        return ESP_OK;
    }

    // Make sure NVS is up first (pai_nvs::begin is idempotent and called by
    // app.cpp before us; this is a safety re-entry).
    pai_nvs::begin();

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Serial.printf("[WIFI] netif_init err=0x%x\n", err);
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Serial.printf("[WIFI] event_loop err=0x%x\n", err);
        return err;
    }

    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    if (netif == nullptr) {
        // Already created — fetch existing.
        netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    }
    if (netif != nullptr) {
        apply_hostname(netif);
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        Serial.printf("[WIFI] esp_wifi_init err=0x%x\n", err);
        return err;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, nullptr);
    if (err != ESP_OK) {
        Serial.printf("[WIFI] handler reg wifi err=0x%x\n", err);
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr, nullptr);
    if (err != ESP_OK) {
        Serial.printf("[WIFI] handler reg ip err=0x%x\n", err);
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        Serial.printf("[WIFI] set_mode err=0x%x\n", err);
        return err;
    }
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM); // never persist creds to NVS at runtime
    if (err != ESP_OK) {
        Serial.printf("[WIFI] set_storage err=0x%x\n", err);
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        Serial.printf("[WIFI] start err=0x%x\n", err);
        return err;
    }

    // Modem sleep for power budget.
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    s_events = xEventGroupCreate();
    s_scan_mutex = xSemaphoreCreateMutex();
    if (s_events == nullptr || s_scan_mutex == nullptr) {
        Serial.println("[WIFI] event/mutex alloc failed");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(task_main, "pai_wifi", TASK_STACK_BYTES, nullptr, TASK_PRIORITY, &s_task,
                                            TASK_CORE);
    if (ok != pdPASS) {
        Serial.println("[WIFI] task create failed");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    Serial.println("[WIFI] driver up, state machine running on core 0");
    return ESP_OK;
}

void suspend()
{
    if (!s_initialized) {
        return;
    }
    s_paused = true;
    xEventGroupSetBits(s_events, BIT_PAUSE_REQ);
    esp_wifi_disconnect();
}

void resume()
{
    if (!s_initialized) {
        return;
    }
    xEventGroupSetBits(s_events, BIT_RESUME_REQ);
}

State get_state()
{
    State s;
    portENTER_CRITICAL(&s_mux);
    s = s_state;
    portEXIT_CRITICAL(&s_mux);
    return s;
}

bool is_connected()
{
    return get_state() == State::CONNECTED;
}

IPAddress get_ip()
{
    uint32_t raw;
    portENTER_CRITICAL(&s_mux);
    raw = s_ip_raw;
    portEXIT_CRITICAL(&s_mux);
    return IPAddress(raw);
}

int8_t get_rssi()
{
    int8_t r;
    portENTER_CRITICAL(&s_mux);
    r = s_rssi;
    portEXIT_CRITICAL(&s_mux);
    return r;
}

std::vector<std::pair<std::string, int8_t>> scan_blocking(uint32_t timeout_ms)
{
    std::vector<std::pair<std::string, int8_t>> out;
    if (!s_initialized || s_scan_mutex == nullptr) {
        return out;
    }
    if (xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return out;
    }

    wifi_scan_config_t cfg = {};
    cfg.show_hidden = false;
    cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    cfg.scan_time.active.min = 100;
    cfg.scan_time.active.max = 300;

    // Blocking scan.
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err != ESP_OK) {
        xSemaphoreGive(s_scan_mutex);
        Serial.printf("[WIFI] scan err=0x%x\n", err);
        return out;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        xSemaphoreGive(s_scan_mutex);
        return out;
    }

    // Cap memory; 32 results is plenty.
    if (ap_count > 32) {
        ap_count = 32;
    }
    wifi_ap_record_t *records = (wifi_ap_record_t *) calloc(ap_count, sizeof(wifi_ap_record_t));
    if (records == nullptr) {
        xSemaphoreGive(s_scan_mutex);
        return out;
    }

    uint16_t fetched = ap_count;
    if (esp_wifi_scan_get_ap_records(&fetched, records) == ESP_OK) {
        out.reserve(fetched);
        for (uint16_t i = 0; i < fetched; ++i) {
            // SSID copy is intentional: caller asked for scan list to do
            // their own provisioning UI. Never log it from this module.
            out.emplace_back(std::string((const char *) records[i].ssid), records[i].rssi);
        }
        // Sort by RSSI descending.
        std::sort(out.begin(), out.end(),
                  [](const std::pair<std::string, int8_t> &a, const std::pair<std::string, int8_t> &b) {
                      return a.second > b.second;
                  });
    }

    free(records);
    xSemaphoreGive(s_scan_mutex);
    return out;
}

} // namespace pai_wifi
