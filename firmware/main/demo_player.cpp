#include "demo_player.hpp"

namespace {

// Longest stretch between two records played as recorded; anything longer
// plays as this. A dropped epoch or two is part of the recording, a minute
// in the pits isn't worth watching.
constexpr int64_t kMaxGapMs = 1000;

// Falling further behind than this — an SD stall, the main loop busy
// replaying a reference lap — moves the schedule forward instead of
// catching up in a burst that would flash past on the screen.
constexpr int64_t kMaxLagUs = 500 * 1000;

} // namespace

bool DemoPlayer::Start(const std::string &path, int speed, int64_t now_us) {
  Stop();
  if (!reader_.Open(path)) {
    return false;
  }
  if (!reader_.Next(&next_ms_, &next_pvt_)) {
    reader_.Close();
    return false;
  }
  path_ = path;
  speed_ = speed < 1 ? 1 : speed;
  next_restarts_ = false;
  base_ms_ = next_ms_;
  base_us_ = now_us;
  return true;
}

void DemoPlayer::Stop() {
  reader_.Close();
  path_.clear();
}

void DemoPlayer::SetSpeed(int speed, int64_t now_us) {
  speed = speed < 1 ? 1 : speed;
  if (speed == speed_) {
    return;
  }
  // Pin the position playing right now, then run on from it at the new rate.
  base_ms_ += (now_us - base_us_) * speed_ / 1000;
  base_us_ = now_us;
  speed_ = speed;
}

bool DemoPlayer::Poll(int64_t now_us, uGnssDecUbxNavPvt_t *pvt,
                      int64_t *due_us, bool *restarted) {
  if (!active()) {
    return false;
  }
  int64_t due = DueUs(next_ms_);
  if (due > now_us) {
    return false;
  }
  if (now_us - due > kMaxLagUs) {
    base_ms_ = next_ms_;
    base_us_ = now_us;
    due = now_us;
  }
  *pvt = next_pvt_;
  *due_us = due;
  *restarted = next_restarts_;
  if (!ReadNext()) {
    Stop();
  }
  return true;
}

int64_t DemoPlayer::NextDueUs() const {
  return active() ? DueUs(next_ms_) : std::numeric_limits<int64_t>::max();
}

// Reads the record after the one just handed out, looping back to the first
// at the end. False only if the file can't be read from the start either.
bool DemoPlayer::ReadNext() {
  int64_t prev_ms = next_ms_;
  next_restarts_ = false;
  if (!reader_.Next(&next_ms_, &next_pvt_)) {
    reader_.Rewind();
    if (!reader_.Next(&next_ms_, &next_pvt_)) {
      return false;
    }
    next_restarts_ = true;
    // The first record plays one capped gap after the last one did.
    int64_t last_due_us = DueUs(prev_ms);
    base_ms_ = next_ms_;
    base_us_ = last_due_us + kMaxGapMs * 1000 / speed_;
    return true;
  }
  int64_t gap_ms = next_ms_ - prev_ms;
  if (gap_ms > kMaxGapMs) {
    base_ms_ += gap_ms - kMaxGapMs;
  }
  return true;
}

int64_t DemoPlayer::DueUs(int64_t record_ms) const {
  return base_us_ + (record_ms - base_ms_) * 1000 / speed_;
}
