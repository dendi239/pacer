#pragma once

// SD card (SPI mode) storage:
//  - /sdcard/tracks/*.json        track_annotator reference tracks
//  - /sdcard/pacer/SESS_NNN.dat   raw session log, one int64 timestamp_ms +
//                                 uGnssDecUbxNavPvt_t per record — the same
//                                 DatVersion::WITH_TIMESTAMP format the
//                                 desktop tools already read.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "esp_err.h"

#include <pacer/gps-source/ubx-nav-pvt.hpp>

esp_err_t storage_mount();

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

/// Paths of every /sdcard/tracks/*.json, sorted by file name. Nothing is
/// parsed — the picker only needs names, and a broken file reports itself
/// when it's actually loaded.
std::vector<std::string> storage_list_tracks();

/// One session log, as storage_list_recordings finds it.
struct StorageRecording {
  std::string path;
  size_t records = 0;
  /// Last record's timestamp minus the first's: device time, so pauses in
  /// logging count too.
  double duration_s = 0;
  /// Fix from the middle record, in degrees — mid-session the kart is far
  /// likelier on track than at either end (pits, the drive there). NaN if
  /// it couldn't be read.
  double lat = std::numeric_limits<double>::quiet_NaN();
  double lon = std::numeric_limits<double>::quiet_NaN();
};

/// The newest `max_count` /sdcard/pacer/SESS_NNN.dat logs, newest (highest
/// NNN) first, skipping any too short to hold a lap — every boot opens a
/// log, so most of the card is empty files from bench sessions. Includes
/// the log being written right now, as far as it has been flushed.
std::vector<StorageRecording> storage_list_recordings(size_t max_count = 24);

/// Streams every whole record of a session log through `on_record`, in
/// order; a torn record at the end (power cut mid-write) is left out.
/// Returns the number of records read, or -1 if the file won't open.
long storage_read_recording(
    const std::string &path,
    const std::function<void(int64_t timestamp_ms,
                             const uGnssDecUbxNavPvt_t &pvt)> &on_record);

/// Reads a session log one record at a time, for replaying it at the pace it
/// was recorded rather than all at once like storage_read_recording.
class StorageRecordingReader {
public:
  StorageRecordingReader() = default;
  ~StorageRecordingReader() { Close(); }
  StorageRecordingReader(const StorageRecordingReader &) = delete;
  StorageRecordingReader &operator=(const StorageRecordingReader &) = delete;

  /// Closes whatever was open first. False if the file won't open.
  bool Open(const std::string &path);
  void Close();
  bool IsOpen() const { return file_ != nullptr; }

  /// The next whole record; false at the end, including a torn last record.
  bool Next(int64_t *timestamp_ms, uGnssDecUbxNavPvt_t *pvt);

  /// Back to the first record.
  void Rewind();

private:
  FILE *file_ = nullptr;
};
