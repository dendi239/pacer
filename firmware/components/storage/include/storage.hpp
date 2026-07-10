#pragma once

// SD card (SPI mode) storage:
//  - /sdcard/tracks/*.json        track_annotator reference tracks
//  - /sdcard/pacer/config.json    {"session_minutes": 15}
//  - /sdcard/pacer/SESS_NNN.dat   raw session log, one int64 timestamp_ms +
//                                 uGnssDecUbxNavPvt_t per record — the same
//                                 DatVersion::WITH_TIMESTAMP format the
//                                 desktop tools already read.

#include <string>

#include "esp_err.h"

#include <pacer/gps-source/ubx-nav-pvt.hpp>

esp_err_t storage_mount();

/// Session length from config.json, or `fallback_minutes` if absent/invalid.
double storage_session_minutes(double fallback_minutes);

/// Creates the next free /sdcard/pacer/SESS_NNN.dat for logging.
esp_err_t storage_log_open(std::string *path_out = nullptr);

/// Appends one record; flushes to card roughly once a second (25 records).
void storage_log_append(int64_t timestamp_ms, const uGnssDecUbxNavPvt_t &pvt);

/// Records handed to storage_log_append so far this session.
size_t storage_log_appended();

/// Records durably on the card (covered by the last fsync).
size_t storage_log_flushed();

/// Flushes + fsyncs whatever is pending now (e.g. when logging is paused
/// from the debug menu).
void storage_log_flush();

/// Scans /sdcard/tracks/*.json and returns the path whose start line is
/// nearest to (lat, lon), or an empty string if none parse. Distance in
/// meters of the winner via distance_m_out. debug_out (if given) gets a
/// one-line human-readable scan report: every entry seen and why it was
/// skipped, for showing on screen.
std::string storage_find_track(double lat, double lon,
                               double *distance_m_out = nullptr,
                               std::string *debug_out = nullptr);
