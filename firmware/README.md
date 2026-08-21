# speedor firmware

In-kart live timing on an ESP32-S3: 25 Hz u-blox GPS in, NV3041A QSPI TFT out.
Shows live delta to the session-best lap (computed on-device with the same
`pacer` C++ core the desktop tools use), current/last/best lap times, lap
number and a session clock. Every fix is also logged to SD in the
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
| GPS UART1 | TX→GPS RX 17, RX→GPS TX 18 | 460800 baud |
| LCD SPI2 (QSPI) | PCLK 47, D0 21, D1 48, D2 40, D3 39, CS 45, BL 1 | |
| Touch I2C | SDA 8, SCL 4, RST 38, INT 3 | |
| SD SPI3 | SCLK 12, MOSI 11, MISO 13, CS 10 | |

The GPS receiver is configured at boot over UBX CFG-VALSET (RAM+BBR): UART
switched to the configured baud, NMEA off, UBX NAV-PVT at 25 Hz. If the
receiver sits silent at its 9600 default, the firmware reconfigures it
through 9600 automatically.

NAV-PVT is the only message enabled — 100 bytes on the wire per epoch, so
25 Hz costs 25 kbit/s. 115200 is the floor; 460800 is the default for the
headroom to add a second message later and to keep a frame down to 2.2 ms
of the 40 ms epoch. Note that the serial link is nowhere near the binding
constraint on rate: whether the receiver can *produce* 25 Hz is the limit
that bites, and the debug menu's GPS page reports the interval it actually
sustains.

The navigation filter is configured too, under `menuconfig → Pacer Dashboard
→ GPS`. These matter more than they look — left at the receiver's defaults,
the trace visibly drifts off the racing line:

| Setting | Default here | Why |
| --- | --- | --- |
| SBAS corrections | On | Every other constellation is off, because the M10 clamps well below 25 Hz once it tracks more than one. SBAS is not one: it corrects the GPS L1 pseudoranges already in use, for one geostationary channel. Without it the ionospheric and ephemeris error walks a few metres over a few minutes — slowly enough to stay constant across a lap, so it displaces whole laps rather than roughening the trace. Turn it off if the fix interval stops being a steady 40 ms. |
| Dynamic platform model | Automotive | The receiver assumes "portable" otherwise, whose acceleration limits a kart exceeds in every corner. The filter then trusts its own prediction over the measurements, lagging the real line and rounding off apexes. Switch to airborne <2g if that still shows. |
| Fix mode | 3D only | A 2D fallback solves against an *assumed* altitude and puts the error into the horizontal fix. A dropped epoch beats a confidently wrong one. |
| Elevation mask | 10° | Low satellites arrive through more atmosphere — and more often via a grandstand than directly. The receiver default is 5°. |
| Position accuracy mask | 25 m | Fixes worse than this get `gnssFixOK` cleared, keeping them out of the log. The receiver default is 100 m. |

Every fix carries the receiver's Doppler-derived NED velocity (`velN/velE/velD`)
and its own accuracy estimates (`hAcc`, `sAcc`) through to `GPSSample`. The
Doppler velocity is roughly an order of magnitude less noisy than differencing
consecutive positions, so prefer it for headings and sub-epoch positions. The
`.dat` logs have always carried these fields, so older recordings expose them
too once re-read.

## SD card layout

```text
/tracks/<name>.json      track_annotator annotations (segments[0] = start line)
/pacer/SESS_NNN.dat      session logs, created automatically
```

Copy the `track_annotation.json` produced by the desktop `track_annotator`
into `/tracks/` — on the first fix the firmware picks the track whose start
line is nearest. Multiple tracks can coexist; the right one is chosen by
location.

## Build & flash

Everything runs from the repo's pixi env — `idf.py`'s Python deps are pixi
pypi-dependencies and `scripts/idf-env.sh` (a pixi activation script) points at
the ESP-IDF checkout, so there is no `. $IDF_PATH/export.sh` step.

```bash
# once per machine: IDF checkout + xtensa toolchain
git clone -b v5.5.1 --recursive https://github.com/espressif/esp-idf ~/esp/esp-idf
pixi run fw-setup

pixi run fw-build
pixi run fw-flash
pixi run fw-monitor
pixi run idf menuconfig     # passthrough for any other idf.py command
```

The firmware is also an optional target of the root CMake project
(same `firmware/build` tree idf.py uses):

```bash
pixi run cmake -B build -DPACER_BUILD_FIRMWARE=ON
pixi run cmake --build build --target firmware
```

LVGL and esp_lvgl_port come from the IDF component registry on first build.
The `pacer` core sources are compiled directly out of the repo tree by the
`pacer_core` component — no separate library build step.

## Fonts

The UI is a table of numbers that changes several times a second, so every
label renders in JetBrains Mono (`components/dashboard_ui/fonts/`) rather than
LVGL's proportional Montserrat — with a fixed advance the digits stay in their
columns instead of shuffling as a "1" becomes an "8". The `.c` tables are
checked in; regenerate them only when a size is added or the face changes:

```bash
firmware/components/dashboard_ui/fonts/gen_fonts.sh   # needs node for npx
```

## Behavior

1. Boot → screen up → SD mount → GPS config → "waiting for gps fix".
2. First fix → nearest track annotation loaded → timing armed.
3. Crossing the start line starts lap 1 and the session clock together, so
   the out lap is off the clock; until then the clock reads `--:--`.
4. Every ~1 m gate updates the delta against the session-best lap. Ghost
   crossings while parked are ignored (crossings below walking pace don't
   count), and laps shorter than 15 s can't double-trigger off the extended
   start gate.
