#include "live-timing.hpp"

#include <cmath>
#include <limits>

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
} // namespace

void pacer::LiveTiming::SetReferenceTrack(const ReferenceTrack &rt,
                                          SessionConfig cfg) {
  cfg_ = cfg;

  gates_.clear();
  for (const Segment &local : rt.DensifiedGates()) {
    gates_.push_back(rt.ToGlobal(local));
  }

  has_prev_ = false;
  on_lap_ = false;
  next_gate_ = 0;
  lap_start_time_ = 0;
  last_recorded_gate_ = 0;
  current_gate_times_.assign(gates_.size(), kNaN);
  best_gate_times_.clear();

  snapshot_ = LiveSnapshot{};
  snapshot_.session_remaining_s = kNaN;
  snapshot_.current_lap_s = kNaN;
  snapshot_.last_lap_s = kNaN;
  snapshot_.best_lap_s = kNaN;
  snapshot_.gate_count = gates_.size();
  session_start_time_ = 0;
}

void pacer::LiveTiming::StartLap(double crossing_time) {
  on_lap_ = true;
  lap_start_time_ = crossing_time;
  next_gate_ = 1 % gates_.size();
  last_recorded_gate_ = 0;
  current_gate_times_.assign(gates_.size(), kNaN);
  current_gate_times_[0] = 0;

  snapshot_.lap_number += 1;
  snapshot_.gates_crossed = 1;
}

void pacer::LiveTiming::FinishLap(double crossing_time) {
  double lap_time = crossing_time - lap_start_time_;

  snapshot_.last_lap_s = lap_time;

  // A lap qualifies as the delta reference only with full gate coverage;
  // interpolation fills small skips, so a hole means we lost sync somewhere.
  bool complete = true;
  for (double t : current_gate_times_) {
    if (std::isnan(t)) {
      complete = false;
      break;
    }
  }

  // Delta at the line is against the best as it stood when the lap was
  // driven, so a new best flashes negative rather than 0.00.
  if (!best_gate_times_.empty()) {
    snapshot_.delta_s = lap_time - snapshot_.best_lap_s;
    snapshot_.delta_valid = true;
  }

  if (complete &&
      (std::isnan(snapshot_.best_lap_s) || lap_time < snapshot_.best_lap_s)) {
    best_gate_times_ = current_gate_times_;
    snapshot_.best_lap_s = lap_time;
  }
}

void pacer::LiveTiming::RecordGate(size_t gate, double crossing_time) {
  double rel = crossing_time - lap_start_time_;

  // Fill gates skipped since the last recorded one by linear interpolation,
  // so a glitchy sample can't leave holes in the reference lap.
  size_t prev = last_recorded_gate_;
  size_t skipped = (gate + gates_.size() - prev) % gates_.size();
  double prev_rel = current_gate_times_[prev];
  for (size_t k = 1; k < skipped; ++k) {
    size_t idx = (prev + k) % gates_.size();
    double ratio = static_cast<double>(k) / static_cast<double>(skipped);
    current_gate_times_[idx] = prev_rel + (rel - prev_rel) * ratio;
  }

  current_gate_times_[gate] = rel;
  last_recorded_gate_ = gate;

  snapshot_.gates_crossed = gate + 1;
  if (gate < best_gate_times_.size()) {
    snapshot_.delta_s = rel - best_gate_times_[gate];
    snapshot_.delta_valid = true;
  }
}

void pacer::LiveTiming::OnSample(GPSSample s) {
  double t = s.timestamp_ms / 1000.0;
  snapshot_.speed_mps = s.full_speed;

  if (!snapshot_.session_started && s.full_speed > cfg_.start_speed_mps) {
    snapshot_.session_started = true;
    session_start_time_ = t;
  }
  if (snapshot_.session_started) {
    snapshot_.session_remaining_s =
        cfg_.session_length_s - (t - session_start_time_);
  }

  GPSSample cur = s;
  if (!has_prev_ || gates_.empty()) {
    has_prev_ = !gates_.empty();
    prev_ = cur;
    return;
  }

  // A kart parked on/near a gate wiggles across it through fix noise alone;
  // below min_crossing_speed_mps no crossing is trustworthy. (Deliberately
  // lower than start_speed_mps: walking a track for a test must still lap.)
  if (s.full_speed < cfg_.min_crossing_speed_mps) {
    prev_ = cur;
    return;
  }

  if (!on_lap_) {
    // Out lap: nothing to time until the start line is crossed.
    if (auto split = Split(gates_[0], prev_, cur)) {
      StartLap(split->timestamp_ms / 1000.0);
    }
  } else {
    // At 25 Hz a kart covers a couple of meters per sample, so one interval
    // can cross several ~1 m gates; keep consuming crossings until none of
    // the upcoming gates intersects this segment. Gates are ordered along
    // the track, so the first hit in the window is the next one crossed.
    bool found = true;
    while (found) {
      found = false;
      size_t window = std::min(cfg_.gate_lookahead, gates_.size());
      for (size_t k = 0; k < window; ++k) {
        size_t idx = (next_gate_ + k) % gates_.size();
        auto split = Split(gates_[idx], prev_, cur);
        if (!split) {
          continue;
        }
        double crossing_time = split->timestamp_ms / 1000.0;
        if (idx == 0) {
          FinishLap(crossing_time);
          StartLap(crossing_time);
        } else {
          RecordGate(idx, crossing_time);
          next_gate_ = (idx + 1) % gates_.size();
        }
        found = true;
        break;
      }
    }

    // Resync guard: whatever the gate tracker thinks, a start-line crossing
    // after a plausible lap time always closes the lap.
    if (t - lap_start_time_ > cfg_.min_lap_s &&
        next_gate_ != 1 % gates_.size()) {
      size_t window = std::min(cfg_.gate_lookahead, gates_.size());
      bool zero_in_window =
          next_gate_ + window > gates_.size(); // window wraps past gate 0
      if (!zero_in_window) {
        if (auto split = Split(gates_[0], prev_, cur)) {
          FinishLap(split->timestamp_ms / 1000.0);
          StartLap(split->timestamp_ms / 1000.0);
        }
      }
    }
  }

  if (on_lap_) {
    snapshot_.current_lap_s = t - lap_start_time_;
  }

  prev_ = cur;
}

double pacer::LiveTiming::DistanceToNextLine(const GPSSample &s) const {
  if (gates_.empty()) {
    return kNaN;
  }
  const Segment &gate = gates_[on_lap_ ? next_gate_ % gates_.size() : 0];

  // Project both gate endpoints into a local metric frame centered on `s`
  // (so `s` itself is the origin), then take the 2D point-segment distance.
  CoordinateSystem cs(s);
  auto local = [&](Point p) {
    return cs.Local(GPSSample{.lat = p.y, .lon = p.x, .altitude = s.altitude});
  };
  Vec3f a = local(gate.first), b = local(gate.second);

  double dx = b[0] - a[0], dy = b[1] - a[1];
  double len2 = dx * dx + dy * dy;
  double t = len2 > 0 ? -(a[0] * dx + a[1] * dy) / len2 : 0;
  t = std::fmin(1.0, std::fmax(0.0, t));
  double px = a[0] + t * dx, py = a[1] + t * dy;
  return std::sqrt(px * px + py * py);
}

std::optional<pacer::TrackOffset>
pacer::LiveTiming::OffsetFromTrack(const GPSSample &s) const {
  if (gates_.empty()) {
    return std::nullopt;
  }

  // Same local-metric-frame trick as DistanceToNextLine: `s` is the origin,
  // so each gate reduces to an origin-to-segment problem in meters.
  CoordinateSystem cs(s);
  auto local = [&](Point p) {
    return cs.Local(GPSSample{.lat = p.y, .lon = p.x, .altitude = s.altitude});
  };

  TrackOffset best;
  double best_dist = std::numeric_limits<double>::infinity();
  for (size_t i = 0; i < gates_.size(); ++i) {
    Vec3f a = local(gates_[i].first), b = local(gates_[i].second);
    double dx = b[0] - a[0], dy = b[1] - a[1];
    double len2 = dx * dx + dy * dy;
    // Unclamped projection parameter: keeps its meaning (0.5 == midpoint)
    // even when the fix sits outside the gate's extent.
    double u = len2 > 0 ? -(a[0] * dx + a[1] * dy) / len2 : 0;
    double t = std::fmin(1.0, std::fmax(0.0, u));
    double px = a[0] + t * dx, py = a[1] + t * dy;
    double dist = std::sqrt(px * px + py * py);
    if (dist < best_dist) {
      best_dist = dist;
      double len = std::sqrt(len2);
      best.gate = i;
      best.lateral_m = (u - 0.5) * len;
      best.half_width_m = len / 2;
      best.distance_m = dist;
    }
  }
  return best;
}
