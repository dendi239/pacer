# speedor firmware

In-kart live timing on an ESP32-S3: 25 Hz u-blox GPS in, NV3041A QSPI TFT out.
Shows live delta to the session-best lap (computed on-device with the same
`pacer` C++ core the desktop tools use), current/last/best lap times, lap
number and a timed-session countdown. Every fix is also logged to SD in the
`.dat` format (`int64 timestamp_ms` + raw `UBX-NAV-PVT` struct) that the
desktop analysis pipeline already reads.

## Hardware

- ESP32-S3 (~40 KB heap for timing state, no PSRAM needed)
- u-blox M9/M10 GPS on a UART (25 Hz capable, e.g. NEO-M10)
- NV3041A QSPI TFT, 480x272 landscape, with GT911 I2C touch
- micro-SD card on a second SPI bus

All pins, the panel orientation, UART/baud and SPI hosts live under
`menuconfig → Pacer Dashboard`. Defaults (the 4.3" ESP32-S3 NV3041A
board):

| Peripheral | Signal | GPIO |
| --- | --- | --- |
| GPS UART1 | TX→GPS RX 17, RX→GPS TX 18 | 115200 baud |
| LCD SPI2 (QSPI) | PCLK 47, D0 21, D1 48, D2 40, D3 39, CS 45, BL 1 | |
| Touch I2C | SDA 8, SCL 4, RST 38, INT 3 | |
| SD SPI3 | SCLK 12, MOSI 11, MISO 13, CS 10 | |

The GPS receiver is configured at boot over UBX CFG-VALSET (RAM+BBR): UART
switched to the configured baud, NMEA off, UBX NAV-PVT at 25 Hz. If the
receiver sits silent at its 9600 default, the firmware reconfigures it
through 9600 automatically.

## SD card layout

```text
/tracks/<name>.json      track_annotator annotations (segments[0] = start line)
/pacer/config.json       {"session_minutes": 15}   (optional)
/pacer/SESS_NNN.dat      session logs, created automatically
```

Copy the `track_annotation.json` produced by the desktop `track_annotator`
into `/tracks/` — on the first fix the firmware picks the track whose start
line is nearest. Multiple tracks can coexist; the right one is chosen by
location.

## Build & flash

```bash
. $IDF_PATH/export.sh   # ESP-IDF >= 5.3
cd firmware
idf.py set-target esp32s3
idf.py build flash monitor
```

LVGL and esp_lvgl_port come from the IDF component registry on first build.
The `pacer` core sources are compiled directly out of the repo tree by the
`pacer_core` component — no separate library build step.

## Behavior

1. Boot → screen up → SD mount → GPS config → "waiting for gps fix".
2. First fix → nearest track annotation loaded → timing armed.
3. Session countdown starts the first time speed exceeds ~2 m/s.
4. Crossing the start line starts lap 1; every ~1 m gate updates the delta
   against the session-best lap. Ghost crossings while parked are ignored
   (crossings below walking pace don't count), and laps shorter than 15 s
   can't double-trigger off the extended start gate.
