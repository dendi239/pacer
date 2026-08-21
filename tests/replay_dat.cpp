// Diagnostic replay: feeds a firmware .dat session log through LiveTiming and
// reports where the live delta stops updating while the kart is moving.
//
//   replay_dat <session.dat> <track.json> [--drop-per-lap <seconds>]
//                                         [--jump-per-lap <meters>]
//
// The two fault-injection options reproduce what the .dat cannot show: the
// log holds only the samples the firmware chose to write, while LiveTiming
// on the device also sees the ones dropped from a full queue (a gap) and the
// ones the receiver flagged as poor (a position outlier). Both are injected
// once per lap, 20 s in, so every lap of a real session becomes a test case.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include <pacer/gps-source/gps-source.hpp>
#include <pacer/live-timing/live-timing.hpp>
#include <pacer/reference-track/reference-track.hpp>

namespace {

// What the driver actually complains about is a delta that is displayed as a
// reading but has stopped being one. So the metric is: time spent moving with
// delta_valid set and delta_s not changing. Time spent moving with the delta
// blanked is counted separately — it is honest, and the driver can see it.
constexpr double kMovingMps = 5.0;
constexpr double kStallReportS = 1.0;

// Where in the lap injected faults land.
constexpr double kFaultAtLapS = 20.0;

// --no-resync restores the old unbounded interpolation as well.
constexpr size_t gates_never_capped = static_cast<size_t>(-1);

} // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: %s <session.dat> <track.json> "
                 "[--drop-per-lap <s>] [--jump-per-lap <m>] "
                 "[--no-resync] [--trace <from_s> <to_s>]\n",
                 argv[0]);
    return 2;
  }
  double drop_per_lap = 0, jump_per_lap = 0;
  double trace_from = 0, trace_to = -1;
  bool no_resync = false;
  for (int i = 3; i < argc; ++i) {
    if (std::strcmp(argv[i], "--no-resync") == 0) {
      no_resync = true;
    } else if (std::strcmp(argv[i], "--drop-per-lap") == 0 && i + 1 < argc) {
      drop_per_lap = std::atof(argv[++i]);
    } else if (std::strcmp(argv[i], "--jump-per-lap") == 0 && i + 1 < argc) {
      jump_per_lap = std::atof(argv[++i]);
    } else if (std::strcmp(argv[i], "--trace") == 0 && i + 2 < argc) {
      trace_from = std::atof(argv[++i]);
      trace_to = std::atof(argv[++i]);
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
      return 2;
    }
  }

  auto rt = pacer::ReferenceTrack::FromFile(argv[2]);
  pacer::SessionConfig cfg{.session_length_s = 6 * 3600};
  if (no_resync) {
    // Pre-fix behaviour, for A/B runs: never declare the tracker lost.
    cfg.resync_distance_m = 1e12;
    cfg.max_interpolated_gates = gates_never_capped;
  }
  pacer::LiveTiming lt;
  lt.SetReferenceTrack(rt, cfg);

  size_t n = 0;
  double t0 = 0;

  size_t last_gate = 0;
  double stall_since = 0;
  // Time inside the stall spent above kMovingMps, and the last sample time,
  // so a pit stop at the start of a stall doesn't excuse the driving that
  // follows it: what matters is how long the delta stood still while the
  // kart was actually going somewhere.
  double stall_moving_s = 0;
  double prev_t = 0;
  double stall_delta = 0;
  pacer::GPSSample stall_start_sample{};

  int last_lap = 0;
  double lap_start_t = 0;
  bool fault_done = true;
  int stalls = 0;
  double stalled_total = 0, worst = 0, worst_at = 0;
  int laps_with_stall = 0, last_stall_lap = -1;
  double blank_moving_s = 0;
  bool was_lost = false;
  double lost_since = 0;
  bool was_valid = false;
  double blank_since = 0, blank_moving_at_start = 0;

  pacer::ReadDatFile(
      argv[1],
      [&](pacer::GPSSample s, double t) {
        if (n++ == 0) {
          t0 = t;
          stall_since = t;
          stall_start_sample = s;
        }

        // Inject the lap's fault, once, kFaultAtLapS into it.
        if (!fault_done && t - lap_start_t >= kFaultAtLapS) {
          if (drop_per_lap > 0 && t - lap_start_t < kFaultAtLapS + drop_per_lap) {
            return; // swallow the sample: a queue-drop gap
          }
          fault_done = true;
          if (jump_per_lap > 0) {
            // One outlier fix, displaced north — the shape of a bad epoch
            // the receiver still reports as a 3D fix.
            s.lat += jump_per_lap / 111320.0;
          }
        }

        lt.OnSample(s);
        auto snap = lt.Snapshot();

        // Tracker-state transitions: when it loses the kart, when it finds
        // it again, and when a lap is dropped as untimeable.
        if (snap.lost && !was_lost) {
          lost_since = t;
          std::printf("  LOST   lap %3d  t=%8.1f  delta blanked (was %+.3f)\n",
                      snap.lap_number, t - t0, snap.delta_s);
        }
        if (!snap.lost && was_lost) {
          std::printf("  FOUND  lap %3d  t=%8.1f  after %5.1fs%s\n",
                      snap.lap_number, t - t0, t - lost_since,
                      std::isnan(snap.current_lap_s) ? "  (lap dropped)" : "");
        }
        was_lost = snap.lost;

        if (!snap.delta_valid && was_valid) {
          blank_since = t;
          blank_moving_at_start = blank_moving_s;
        }
        if (snap.delta_valid && !was_valid && blank_since > 0) {
          std::printf("  DELTA BACK  lap %3d  t=%8.1f  blank for %6.1fs "
                      "(%5.1fs of it moving)\n",
                      snap.lap_number, t - t0, t - blank_since,
                      blank_moving_s - blank_moving_at_start);
        }
        was_valid = snap.delta_valid;

        double dt = prev_t > 0 ? t - prev_t : 0;
        prev_t = t;

        // Second-by-second view of one window: where the kart actually is
        // (nearest gate) versus the gate the tracker is still waiting for.
        if (t - t0 >= trace_from && t - t0 <= trace_to) {
          static double next_trace = 0;
          // Short windows print every sample; long ones once a second.
          double step = trace_to - trace_from <= 30 ? 0.0 : 1.0;
          if (t >= next_trace) {
            next_trace = t + step;
            auto off = lt.OffsetFromTrack(s);
            std::printf("    trace t=%8.1f  lap %3d  %5.1f km/h  "
                        "tracker gate %4zu  actual gate %4zu  lateral %+6.1f m "
                        "(half-width %.1f m)  delta %+8.3f%s\n",
                        t - t0, snap.lap_number, s.full_speed * 3.6,
                        snap.gates_crossed, off ? off->gate : 0,
                        off ? off->lateral_m : 0, off ? off->half_width_m : 0,
                        snap.delta_s, snap.delta_valid ? "" : " (invalid)");
          }
        }
        if (snap.lap_number != last_lap) {
          std::printf(
              "lap %3d start  t=%8.1f  last=%7.3f best=%7.3f delta=%+7.3f%s\n",
              snap.lap_number, t - t0, snap.last_lap_s, snap.best_lap_s,
              snap.delta_s, snap.delta_valid ? "" : " (invalid)");
          last_lap = snap.lap_number;
          lap_start_t = t;
          fault_done = false;
        }
        if (snap.lap_number == 0) {
          return;
        }

        bool moving = s.full_speed >= kMovingMps;
        if (moving && !snap.delta_valid) {
          blank_moving_s += dt < 1.0 ? dt : 0;
        }

        // A displayed delta that hasn't moved: the fault under investigation.
        if (snap.delta_valid && snap.delta_s == stall_delta) {
          if (moving && dt < 1.0) {
            stall_moving_s += dt;
          }
          return;
        }

        if (stall_moving_s >= kStallReportS) {
          ++stalls;
          stalled_total += stall_moving_s;
          if (snap.lap_number != last_stall_lap) {
            ++laps_with_stall;
            last_stall_lap = snap.lap_number;
          }
          if (stall_moving_s > worst) {
            worst = stall_moving_s;
            worst_at = stall_since - t0;
          }
          auto off = lt.OffsetFromTrack(stall_start_sample);
          std::printf(
              "  STALE lap %3d  t=%8.1f  %5.1fs moving (%5.1fs wall)  "
              "gate %4zu -> %4zu / %zu  delta %+7.3f -> %+7.3f  "
              "at stall start: nearest gate %4zu lateral %+5.1f m "
              "(half-width %.1f m) dist %.1f m\n",
              snap.lap_number, stall_since - t0, stall_moving_s, t - stall_since,
              last_gate, snap.gates_crossed, snap.gate_count, stall_delta,
              snap.delta_s, off ? off->gate : 0, off ? off->lateral_m : 0,
              off ? off->half_width_m : 0, off ? off->distance_m : 0);
        }

        last_gate = snap.gates_crossed;
        stall_since = t;
        stall_moving_s = 0;
        stall_delta = snap.delta_s;
        stall_start_sample = s;
      },
      pacer::DatVersion::WITH_TIMESTAMP);

  auto snap = lt.Snapshot();
  std::printf("\n%zu samples, %.1f min, laps=%d best=%.3f gates=%zu\n", n,
              snap.session_elapsed_s / 60.0, snap.lap_number, snap.best_lap_s,
              snap.gate_count);
  std::printf("injected: drop %.1f s/lap, jump %.1f m/lap\n", drop_per_lap,
              jump_per_lap);
  std::printf("stale delta (shown, not updating) >%.1fs: %d times over %d "
              "laps, %.1f s of moving, worst %.1f s at t=%.1f\n",
              kStallReportS, stalls, laps_with_stall, stalled_total, worst,
              worst_at);
  std::printf("delta blanked while moving: %.1f s\n", blank_moving_s);
  return 0;
}
