#include "live-timing.hpp"

#include <cmath>
#include <limits>

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/// Longest gap between fixes the "distance since the last gate" integrator
/// will credit at the newer fix's speed. Past this the two fixes say nothing
/// about what happened in between, so the tracker checks where the kart is
/// rather than assuming.
constexpr double kMaxIntegrableGapS = 5.0;
} // namespace

void pacer::LiveTiming::SetReferenceTrack(const ReferenceTrack &rt,
                                          SessionConfig cfg) {
  cfg_ = cfg;

  // Release the previous track's gates before building the new one, then
  // convert the densified gates in place and adopt the vector rather than
  // copying it across. At ~1 gate/m these vectors are tens of KB each
  // (a 1.1 km circuit is ~35 KB), so holding the old set, the densified set
  // and a growing copy at the same time is what made loading a second track
  // throw std::bad_alloc on the ESP32.
  gates_ = std::vector<Segment>{};
  current_gate_times_ = std::vector<double>{};
  best_gate_times_ = std::vector<double>{};

  std::vector<Segment> gates = rt.DensifiedGates();
  for (Segment &gate : gates) {
    gate = rt.ToGlobal(gate);
  }
  gates_ = std::move(gates);

  ResetSession();
}

void pacer::LiveTiming::ResetSession() {
  has_prev_ = false;
  on_lap_ = false;
  next_gate_ = 0;
  lap_start_time_ = 0;
  last_recorded_gate_ = 0;
  distance_since_gate_m_ = 0;
  distance_since_scan_m_ = 0;
  current_gate_times_.assign(gates_.size(), kNaN);
  best_gate_times_.clear();

  snapshot_ = LiveSnapshot{};
  snapshot_.session_remaining_s = kNaN;
  snapshot_.session_elapsed_s = kNaN;
  snapshot_.current_lap_s = kNaN;
  snapshot_.last_lap_s = kNaN;
  snapshot_.best_lap_s = kNaN;
  snapshot_.gate_count = gates_.size();
  session_start_time_ = 0;
}

void pacer::LiveTiming::StartLap(double crossing_time) {
  // The session is timed from the start of lap 1, so an out lap of any
  // length — or a long crawl out of the pits — costs nothing.
  if (!snapshot_.session_started) {
    snapshot_.session_started = true;
    session_start_time_ = crossing_time;
  }

  on_lap_ = true;
  lap_start_time_ = crossing_time;
  next_gate_ = 1 % gates_.size();
  last_recorded_gate_ = 0;
  distance_since_gate_m_ = 0;
  distance_since_scan_m_ = 0;
  current_gate_times_.assign(gates_.size(), kNaN);
  current_gate_times_[0] = 0;

  snapshot_.lap_number += 1;
  snapshot_.gates_crossed = 1;
  snapshot_.lost = false;
}

void pacer::LiveTiming::AbandonLap() {
  // No FinishLap(): there is no honest time to report. The lap keeps its
  // number (it was driven, just not timed), and last/best lap stand.
  on_lap_ = false;
  next_gate_ = 0;
  last_recorded_gate_ = 0;
  distance_since_gate_m_ = 0;
  distance_since_scan_m_ = 0;
  current_gate_times_.assign(gates_.size(), kNaN);

  snapshot_.lost = false;
  snapshot_.delta_valid = false;
  snapshot_.current_lap_s = kNaN;
  snapshot_.gates_crossed = 0;
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
  // so a glitchy sample can't leave holes in the reference lap. Wide holes
  // are left as they are — see max_interpolated_gates.
  size_t prev = last_recorded_gate_;
  size_t skipped = (gate + gates_.size() - prev) % gates_.size();
  double prev_rel = current_gate_times_[prev];
  if (skipped <= cfg_.max_interpolated_gates && !std::isnan(prev_rel)) {
    for (size_t k = 1; k < skipped; ++k) {
      size_t idx = (prev + k) % gates_.size();
      double ratio = static_cast<double>(k) / static_cast<double>(skipped);
      current_gate_times_[idx] = prev_rel + (rel - prev_rel) * ratio;
    }
  }

  current_gate_times_[gate] = rel;
  last_recorded_gate_ = gate;
  distance_since_gate_m_ = 0;
  distance_since_scan_m_ = 0;
  snapshot_.lost = false;

  snapshot_.gates_crossed = gate + 1;
  if (gate < best_gate_times_.size()) {
    snapshot_.delta_s = rel - best_gate_times_[gate];
    snapshot_.delta_valid = true;
  }
}

void pacer::LiveTiming::TryReacquire(const GPSSample &s) {
  auto off = OffsetFromTrack(s);
  if (!off || off->distance_m > cfg_.resync_max_offset_m) {
    // Nowhere near the gate sequence: in the pit lane, in the run-off, or a
    // fix bad enough to be off the circuit entirely. Keep looking — and say
    // so, because a delta that stopped moving is indistinguishable on the
    // screen from one that is merely holding steady.
    snapshot_.lost = true;
    snapshot_.delta_valid = false;
    return;
  }

  size_t gate = off->gate;

  if (snapshot_.lost) {
    // The kart was off the gate sequence entirely and is only now back on
    // it. Nothing measured across that hole is worth anything, and it may
    // well have passed the start line while out of sight — which is exactly
    // what a pit stop does, every kart circuit running its pit lane
    // alongside the line. Drop the lap and wait for a clean crossing.
    AbandonLap();
    return;
  }

  if (gate == 0) {
    // The start line is the crossing logic's business: proximity cannot
    // tell "about to cross it" from "just crossed it", and getting that
    // wrong either invents a lap or loses one. It is also the one gate
    // approached across an un-densified gap (the annotated track's last
    // gate back round to its first), so this case is routine, not an error.
    return;
  }

  size_t ahead = (gate + gates_.size() - last_recorded_gate_) % gates_.size();
  if (ahead == 0 || ahead > gates_.size() / 2) {
    // At, or behind, the last gate recorded: the tracker has run ahead of
    // the kart rather than behind it (a fix that jumped forward, say).
    // Driving into the gate it is waiting for fixes that by itself, and
    // costs at most the overshoot — no need to rewrite any gate times.
    return;
  }

  if (gate < last_recorded_gate_) {
    // Forward, but only by going round past gate 0 — so the kart crossed
    // the start line unseen (wide of it, or during a fix outage). The lap
    // clock is measuring from the wrong line crossing now, so the lap has
    // to go, same as after a pit stop.
    AbandonLap();
    return;
  }

  // Back on the sequence, further round the same lap. Resume from here; the
  // gates in the hole stay NaN, so this lap can't become the delta
  // reference — but the delta is a live reading again immediately, and an
  // exact one from the next gate actually crossed. (This gate is stamped at
  // the fix that found it, so it can be up to a scan interval late.)
  RecordGate(gate, s.timestamp_ms / 1000.0);
  next_gate_ = (gate + 1) % gates_.size();
}

void pacer::LiveTiming::OnSample(GPSSample s) {
  double t = s.timestamp_ms / 1000.0;
  snapshot_.speed_mps = s.full_speed;

  TrackCrossings(s);

  // Both clocks tick on every sample once armed, including the ones
  // TrackCrossings() bails out of (parked on track, no previous fix).
  if (snapshot_.session_started) {
    snapshot_.session_elapsed_s = t - session_start_time_;
    snapshot_.session_remaining_s =
        cfg_.session_length_s - snapshot_.session_elapsed_s;
  }
  if (on_lap_) {
    snapshot_.current_lap_s = t - lap_start_time_;
  }
}

void pacer::LiveTiming::TrackCrossings(const GPSSample &s) {
  double t = s.timestamp_ms / 1000.0;

  GPSSample cur = s;
  if (!has_prev_ || gates_.empty()) {
    has_prev_ = !gates_.empty();
    prev_ = cur;
    return;
  }

  // A kart parked on/near a gate wiggles across it through fix noise alone;
  // below min_crossing_speed_mps no crossing is trustworthy. (Kept low
  // enough that walking a track for a test still produces laps.)
  if (s.full_speed < cfg_.min_crossing_speed_mps) {
    prev_ = cur;
    return;
  }

  // Ground covered since the previous fix, integrated from the receiver's
  // own speed: differencing 25 Hz positions is mostly noise, while gSpeed
  // comes off carrier Doppler.
  double dt = (s.timestamp_ms - prev_.timestamp_ms) / 1000.0;
  if (on_lap_ && dt > 0) {
    if (dt < kMaxIntegrableGapS) {
      distance_since_gate_m_ += s.full_speed * dt;
      distance_since_scan_m_ += s.full_speed * dt;
    } else {
      // Too long a silence to integrate through — this fix's speed says
      // nothing about the seconds before it. The kart could be anywhere, so
      // arrange for a position check below, which the gate crossings found
      // on this (very long) step will cancel if they land.
      distance_since_gate_m_ = cfg_.resync_distance_m + 1;
      distance_since_scan_m_ = cfg_.resync_scan_interval_m;
    }
  }

  if (!on_lap_) {
    // Out lap: nothing to time until the start line is crossed.
    if (auto split = Split(gates_[0], prev_, cur)) {
      StartLap(split->timestamp_ms / 1000.0);
    }
  } else {
    // A kart covers a couple of meters per sample, so one interval
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

    // Gates sit about a metre apart, so more than the forward window's
    // worth of meters without crossing the one we are waiting for means the
    // kart is no longer on the sequence — it ran wide of the annotated
    // edge, took the pit lane, or the receiver dropped a burst of fixes.
    // Searching only forward from next_gate_, the tracker would otherwise
    // sit there until the kart came all the way round to it: on a 68 s
    // circuit that is up to a full lap of the delta holding a stale number.
    if (on_lap_ && distance_since_gate_m_ > cfg_.resync_distance_m &&
        distance_since_scan_m_ >= cfg_.resync_scan_interval_m) {
      distance_since_scan_m_ = 0;
      TryReacquire(cur);
    }
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
