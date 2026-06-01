// =============================================================================
// pai_ble_relay.h — Pendant Nova BLE-relay transport (replaces WiFi upload)
// =============================================================================
// Drains the pai_storage chunk queue over a BLE GATT notify characteristic to
// a phone, which forwards each opaque, HMAC-signed blob to the ingest server
// over its own cellular link. The phone is a DUMB PIPE and holds no secret:
// the HMAC over the chunk body is computed HERE (key stays in NVS) and shipped
// alongside the body in the START frame; the phone merely replays the POST.
//
// Wire protocol (matches the Android relay app byte-for-byte; all ints LE):
//   START (0x01): magic(0x9A) type chunk_id[8] total_len[4] frag_count[2] hmac[32]   = 48 B
//   DATA  (0x02): magic(0x9A) type frag_idx[2] payload[..mtu-3-4]
//   END   (0x03): magic(0x9A) type chunk_id[8]                                        = 10 B
//   ACK (phone->pendant, written to the ACK char): status(0x01 ok / 0x02 nack) chunk_id[8] = 9 B
//
// Delete-on-ack: a chunk is removed from LittleFS ONLY after a matching ACK
// (the phone acks only after the server returns 2xx). On NACK/timeout/disconnect
// the chunk stays oldest in the queue and is retransmitted on the next pass.
//
// Reuses pai_storage's queue (oldest-first), pai_nvs upload token, and the same
// HMAC-SHA256 contract as the old pai_upload path — only the transport changes.
// =============================================================================

#ifndef PAI_BLE_RELAY_H
#define PAI_BLE_RELAY_H

#include <stddef.h>
#include <stdint.h>

class BLECharacteristic; // fwd-decl (Bluedroid)

namespace pai_ble_relay
{

// Wire-protocol constants (kept in the header so app.cpp/tests can reference).
static constexpr uint8_t FRAME_MAGIC = 0x9A;
static constexpr uint8_t FRAME_START = 0x01;
static constexpr uint8_t FRAME_DATA = 0x02;
static constexpr uint8_t FRAME_END = 0x03;
static constexpr uint8_t ACK_OK = 0x01;
static constexpr uint8_t ACK_NACK = 0x02;

// Wire to configure_ble(): hand over the created characteristics so the relay
// task can notify on CHUNK and the ACK write-callback can route here. (The
// STATUS characteristic is read-only diagnostics for the phone — the firmware
// never touches it after configure_ble sets its value, so it isn't passed in.)
void register_characteristics(BLECharacteristic *chunk_char, BLECharacteristic *ack_char);

// BLE lifecycle hooks (call from ServerHandler).
void on_connect();
void on_disconnect();
void on_mtu(uint16_t negotiated_mtu); // store usable payload = mtu - 3

// ACK characteristic write callback target (parses status + chunk_id).
void on_ack(const uint8_t *data, size_t len);

// Spawn the relay drain task (core 0). Runs hmac self-test + loads the NVS
// token first; self-deletes on failure (mirrors pai_upload).
void start_task();

// True while a chunk transfer is in flight (SENDING or AWAIT_ACK). The
// light-sleep gate reads this so we never sleep mid-chunk (8 s supervision
// timeout would drop the link).
bool is_busy();

} // namespace pai_ble_relay

#endif // PAI_BLE_RELAY_H
