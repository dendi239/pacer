#pragma once

// Demo mode: plays a recorded session log back in place of the receiver, at
// the pace it was recorded times a speed multiplier, looping at the end.
//
// Pacing runs on the log's own timestamps (device time when each fix came
// in), so the replay keeps the recording's rhythm — dropped epochs, rate
// changes and all. Long gaps (logging paused, a stop in the pits) are cut
// short: watching a demo sit still for minutes shows nothing.

#include <cstdint>
#include <limits>
#include <string>

#include <pacer/gps-source/ubx-nav-pvt.hpp>

#include "storage.hpp"

class DemoPlayer {
public:
  /// Starts `path` from its first record, due right away. False (and
  /// stopped) if the file won't open or holds no records.
  bool Start(const std::string &path, int speed, int64_t now_us);
  void Stop();

  bool active() const { return reader_.IsOpen(); }
  /// The recording playing; empty while stopped.
  const std::string &path() const { return path_; }
  int speed() const { return speed_; }

  /// Changes the multiplier from here on, without a jump in the playback
  /// position.
  void SetSpeed(int speed, int64_t now_us);

  /// The next record, if it is due by `now_us`. `due_us` gets the device
  /// time it was due at — the time it would have arrived from a receiver
  /// running `speed` times faster. `restarted` is set on the first record
  /// after looping back to the start, where the session should start over.
  bool Poll(int64_t now_us, uGnssDecUbxNavPvt_t *pvt, int64_t *due_us,
            bool *restarted);

  /// Device time the next record is due at, for sleeping until then;
  /// "never" while stopped.
  int64_t NextDueUs() const;

private:
  bool ReadNext();
  int64_t DueUs(int64_t record_ms) const;

  StorageRecordingReader reader_;
  std::string path_;
  int speed_ = 1;

  // The record Poll hands out next, read ahead so its due time is known.
  int64_t next_ms_ = 0;
  uGnssDecUbxNavPvt_t next_pvt_{};
  bool next_restarts_ = false;

  // Recording time `base_ms_` plays at device time `base_us_`; later records
  // play `speed_` times faster than they were recorded. Rebased on speed
  // changes, skipped gaps, loops and stalls.
  int64_t base_ms_ = 0;
  int64_t base_us_ = 0;
};
