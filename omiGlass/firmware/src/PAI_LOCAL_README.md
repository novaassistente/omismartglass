# PAI Local Mode — omiGlass ESP32-S3 Firmware

When `PAI_LOCAL_MODE = 1` (default in `config.h`), the firmware boots BLE-only:
no Wi-Fi auto-init, no OTA HTTP fetch. The Linux host pairing service is the
only consumer; it pairs over BLE and handles transcription / inference itself.

Set `PAI_LOCAL_MODE` to `0` in `config.h` to restore upstream omi.me cloud-app
compatibility (Wi-Fi credentials over BLE + HTTP firmware fetch).

## BLE GATT Surface

Advertised name: `OMI Glass` (see `BLE_DEVICE_NAME` in `config.h`).

### Main service — `19B10000-E8F2-537E-4F6C-D104768A1214`

| Name | UUID | Direction | Purpose |
|------|------|-----------|---------|
| Audio Data | `19B10001-E8F2-537E-4F6C-D104768A1214` | notify (read) | Streams encoded Opus frames. Packet = 2-byte LE index + 1-byte sub-index + Opus payload (`AUDIO_PACKET_HEADER_SIZE = 3`, max payload `OPUS_OUTPUT_MAX_BYTES = 160`). Subscribe via CCCD (BLE2902) to start streaming. |
| Audio Codec | `19B10002-E8F2-537E-4F6C-D104768A1214` | read | Single byte codec ID. Currently `21` (Opus, `AUDIO_CODEC_ID`). Read once on connect to confirm codec. |
| Photo Data | `19B10005-E8F2-537E-4F6C-D104768A1214` | notify (read) | Streams JPEG photo chunks. First chunk: `[idx_lo, idx_hi, orientation, ...up to 199 JPEG bytes]`. Subsequent chunks: `[idx_lo, idx_hi, ...up to 200 JPEG bytes]`. End-of-photo marker: `[0xFF, 0xFF]`. Frame size `FRAMESIZE_VGA` 640x480 JPEG, quality 25. |
| Photo Control | `19B10006-E8F2-537E-4F6C-D104768A1214` | write | Single signed byte. `-1` = single shot, `0` = stop, `5..300` = start interval capture (firmware uses fixed `PHOTO_CAPTURE_INTERVAL_MS = 30000`). |

### Battery service — `0x180F` (standard)

| Name | UUID | Direction | Purpose |
|------|------|-----------|---------|
| Battery Level | `0x2A19` | notify (read) | uint8 percentage 0–100. Updated every `BATTERY_TASK_INTERVAL_MS = 20000` ms. |

### Device Information service — `0x180A` (standard)

| Name | UUID | Direction | Purpose |
|------|------|-----------|---------|
| Manufacturer Name | `0x2A29` | read | `Based Hardware` |
| Model Number | `0x2A24` | read | `OMI Glass` |
| Firmware Revision | `0x2A26` | read | `2.3.2` (from `FIRMWARE_VERSION_STRING`) |
| Hardware Revision | `0x2A27` | read | `ESP32-S3-v1.0` |
| Serial Number | `0x2A25` | read | Derived from ESP32 eFuse MAC, hex string. |

### OTA service — `19B10010-E8F2-537E-4F6C-D104768A1214`

In PAI_LOCAL_MODE the BLE command parser still accepts writes (so a future host
can ship firmware over BLE), but the Wi-Fi connect and HTTP fetch are
compiled out — `START_OTA` will respond `OTA_STATUS_WIFI_FAILED` /
`OTA_STATUS_DOWNLOAD_FAILED`.

| Name | UUID | Direction | Purpose |
|------|------|-----------|---------|
| OTA Control | `19B10011-E8F2-537E-4F6C-D104768A1214` | write / read | Command channel. Read returns `[status, progress]`. Writes use OTA_CMD_* opcodes (see `config.h`). |
| OTA Data | `19B10012-E8F2-537E-4F6C-D104768A1214` | notify (read) | Progress notifications, `[status, progress]` byte pairs. |

## Host pairing flow

1. Scan for `OMI Glass`, connect.
2. Discover service `19B10000-E8F2-537E-4F6C-D104768A1214`.
3. Read `19B10002-...` to confirm codec id `21` (Opus 16 kHz, 32 kbps, 20 ms frames).
4. Subscribe (write CCCD `0x0001`) to `19B10001-...` for audio frames.
5. Subscribe to `19B10005-...` for photo chunks; reassemble until you see `[0xFF, 0xFF]`.
6. Write `-1` to `19B10006-...` for an on-demand photo, or `30` to start the 30 s interval loop.
7. Subscribe to battery `0x2A19` if you want power telemetry.

## Audio payload format expected by host

Each notification on `19B10001-...` is:

```
byte 0     : packet_index_low
byte 1     : packet_index_high
byte 2     : sub_index (0; reserved for fragmentation, currently always 0)
bytes 3..N : Opus frame (variable length, ≤ 160 bytes, 20 ms @ 16 kHz)
```

Decode with libopus at 16 000 Hz mono, 320 samples/frame. Drop / interpolate on
gaps in the 16-bit packet index.
