#pragma once

// u-blox GPS over UART: configures the receiver for 25 Hz UBX-NAV-PVT
// output and runs a reader task that decodes each frame into the same
// uGnssDecUbxNavPvt_t struct the desktop .dat pipeline uses.

#include "esp_err.h"

#include <pacer/gps-source/ubx-nav-pvt.hpp>

// Called from the GPS reader task for every decoded NAV-PVT frame.
using ubx_pvt_callback_t = void (*)(const uGnssDecUbxNavPvt_t &pvt, void *ctx);

// Installs the UART driver (pins/baud from Kconfig), pushes the 25 Hz + UBX
// output configuration to the receiver (trying both the configured baud and
// the u-blox 9600 default, then switching the receiver to the configured
// baud), and starts the reader task.
esp_err_t ubx_gps_start(ubx_pvt_callback_t on_pvt, void *ctx);
