#pragma once

// u-blox GPS over UART: configures the receiver for 25 Hz UBX-NAV-PVT
// output on GPS L1C/A (the M10 won't hold 25 Hz with a second constellation
// tracking), optionally with SBAS corrections on top, and runs a reader task
// that decodes each frame into the same uGnssDecUbxNavPvt_t struct the
// desktop .dat pipeline uses.

#include "esp_err.h"

#include <pacer/gps-source/ubx-nav-pvt.hpp>

// Called from the GPS reader task for every decoded NAV-PVT frame.
using ubx_pvt_callback_t = void (*)(const uGnssDecUbxNavPvt_t &pvt, void *ctx);

// Installs the UART driver (pins/baud from Kconfig), pushes the 25 Hz + UBX
// output configuration to the receiver (trying both the configured baud and
// the u-blox 9600 default, then switching the receiver to the configured
// baud), and starts the reader task.
esp_err_t ubx_gps_start(ubx_pvt_callback_t on_pvt, void *ctx);

// The configuration this build pushes to the receiver, as newline-separated
// lines for the debug menu. Static text: it is what the firmware *asked* for,
// not a read-back, so a receiver that NAKed a key still reads as configured
// here — compare it against the live figures on the same page.
const char *ubx_gps_config_summary();
