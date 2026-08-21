#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <vector>

#include <pacer/datatypes/datatypes.hpp>
#include <pacer/geometry/geometry.hpp>
#include <pacer/laps/laps.hpp>
#include <pacer/live-timing/live-timing.hpp>
#include <pacer/reference-track/reference-track.hpp>

using Catch::Approx;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSampleRateHz = 25.0;
constexpr double kTrackRadiusM = 100.0;

// A circular test track: 36 hand-annotated radial gates (as if drawn in
// track_annotator), gate 0 at angle 0 doubling as the start/finish line.
pacer::ReferenceTrack MakeCircularTrack(const pacer::CoordinateSystem &cs) {
  pacer::ReferenceTrack rt;
  rt.cs = cs;
  const int gate_count = 36;
  for (int i = 0; i < gate_count; ++i) {
    double theta = 2 * kPi * i / gate_count;
    double c = std::cos(theta), s = std::sin(theta);
    rt.segments.push_back(pacer::Segment{
        pacer::Point{(kTrackRadiusM - 10) * c, (kTrackRadiusM - 10) * s},
        pacer::Point{(kTrackRadiusM + 10) * c, (kTrackRadiusM + 10) * s},
    });
  }
  return rt;
}

// Drives around the circle at constant speed for `laps` full revolutions,
// advancing `theta` / `t` in place so consecutive stints join seamlessly.
// `radius_offset` displaces the line driven from the annotated one, in
// meters (positive outward), so a stint can run wide of the gates or leave
// the circuit entirely. Emits 25 Hz samples (with timestamp_ms set from `t`)
// through `sink`.
template <class F>
void DriveWide(const pacer::CoordinateSystem &cs, double lap_period_s,
               double laps, double radius_offset, double &theta, double &t,
               F sink) {
  double omega = 2 * kPi / lap_period_s;
  double speed = omega * kTrackRadiusM;
  double radius = kTrackRadiusM + radius_offset;
  int steps = static_cast<int>(laps * lap_period_s * kSampleRateHz);
  for (int i = 0; i < steps; ++i) {
    theta += omega / kSampleRateHz;
    t += 1.0 / kSampleRateHz;
    pacer::GPSSample s = cs.Global(
        pacer::Vec3f{radius * std::cos(theta), radius * std::sin(theta), 0});
    s.full_speed = speed;
    s.ground_speed = speed;
    s.timestamp_ms = static_cast<int64_t>(std::llround(t * 1000));
    sink(s);
  }
}

template <class F>
void Drive(const pacer::CoordinateSystem &cs, double lap_period_s, double laps,
           double &theta, double &t, F sink) {
  DriveWide(cs, lap_period_s, laps, 0.0, theta, t, sink);
}

pacer::CoordinateSystem TestCS() {
  return pacer::CoordinateSystem(
      pacer::GPSSample{.lat = 51.0, .lon = 0.5, .altitude = 0});
}

} // namespace

TEST_CASE("LiveTiming counts laps and times them", "[live-timing]") {
  auto cs = TestCS();
  auto rt = MakeCircularTrack(cs);

  pacer::LiveTiming lt;
  lt.SetReferenceTrack(rt, pacer::SessionConfig{.session_length_s = 600});

  // Also feed the offline pipeline the exact same stream to cross-check.
  pacer::Laps laps;

  double theta = -0.05, t = 100.0;
  const double lap_period = 40.0;
  Drive(cs, lap_period, 3.2, theta, t, [&](pacer::GPSSample s) {
    lt.OnSample(s);
    laps.AddPoint(s);
  });

  auto snap = lt.Snapshot();
  REQUIRE(snap.lap_number == 4); // 3 finished laps, currently on lap 4
  REQUIRE(snap.last_lap_s == Approx(lap_period).margin(0.1));
  REQUIRE(snap.best_lap_s == Approx(lap_period).margin(0.1));
  REQUIRE(snap.current_lap_s > 0);
  REQUIRE(snap.current_lap_s < lap_period);

  laps.SetCoordinateSystem(cs);
  laps.sectors = rt.BuildSectors(cs);
  laps.Update();
  REQUIRE(laps.LapsCount() >= 3);
  for (size_t i = 0; i + 1 < laps.LapsCount(); ++i) {
    REQUIRE(laps.LapTime(i) == Approx(lap_period).margin(0.05));
  }
}

TEST_CASE("LiveTiming delta tracks time lost to the best lap",
          "[live-timing]") {
  auto cs = TestCS();
  auto rt = MakeCircularTrack(cs);

  pacer::LiveTiming lt;
  lt.SetReferenceTrack(rt);

  double theta = -0.05, t = 0.0;
  const double fast = 40.0, slow = 42.0;

  // Lap 1 (fast) becomes the session best.
  Drive(cs, fast, 1.02, theta, t,
        [&](pacer::GPSSample s) { lt.OnSample(s); });
  REQUIRE(lt.Snapshot().best_lap_s == Approx(fast).margin(0.1));

  // Half a lap at the slow pace: should be ~half the final gap down.
  Drive(cs, slow, 0.5, theta, t,
        [&](pacer::GPSSample s) { lt.OnSample(s); });
  {
    auto snap = lt.Snapshot();
    REQUIRE(snap.delta_valid);
    REQUIRE(snap.delta_s == Approx((slow - fast) / 2).margin(0.15));
  }

  // Complete the slow lap: delta at the line equals the lap-time difference.
  Drive(cs, slow, 0.52, theta, t,
        [&](pacer::GPSSample s) { lt.OnSample(s); });
  {
    auto snap = lt.Snapshot();
    REQUIRE(snap.delta_valid);
    REQUIRE(snap.last_lap_s == Approx(slow).margin(0.1));
    REQUIRE(snap.best_lap_s == Approx(fast).margin(0.1));
    REQUIRE(snap.delta_s > 0);
  }
}

TEST_CASE("LiveTiming survives dropped samples", "[live-timing]") {
  auto cs = TestCS();
  auto rt = MakeCircularTrack(cs);

  pacer::LiveTiming lt;
  lt.SetReferenceTrack(rt);

  // Drop two of every three samples: ~2.4 m gaps, several gates per gap.
  double theta = -0.05, t = 0.0;
  int i = 0;
  Drive(cs, 40.0, 2.1, theta, t, [&](pacer::GPSSample s) {
    if (i++ % 3 == 0) {
      lt.OnSample(s);
    }
  });

  auto snap = lt.Snapshot();
  REQUIRE(snap.lap_number == 3);
  REQUIRE(snap.last_lap_s == Approx(40.0).margin(0.2));
  // Full gate coverage despite the gaps (skips get interpolated), so the
  // lap still qualified as the delta reference.
  REQUIRE(snap.best_lap_s == Approx(40.0).margin(0.2));
}

// Gates reach 10 m either side of the driven line plus TimingLine()'s 2 m
// extension, so these offsets put the kart just past a gate's end (still
// within a couple of meters of the gate itself) and far enough out to be off
// the circuit altogether.
constexpr double kJustPastGateEnd = 16.0;
constexpr double kOffCircuit = 40.0;

TEST_CASE("LiveTiming keeps the delta live through a stint run wide of the "
          "gates",
          "[live-timing]") {
  auto cs = TestCS();
  auto rt = MakeCircularTrack(cs);

  pacer::LiveTiming lt;
  lt.SetReferenceTrack(rt);

  double theta = -0.05, t = 0.0;
  const double lap = 40.0;

  // Lap 1 sets the reference.
  Drive(cs, lap, 1.02, theta, t, [&](pacer::GPSSample s) { lt.OnSample(s); });
  REQUIRE(lt.Snapshot().best_lap_s == Approx(lap).margin(0.1));

  // A third of the way round lap 2, run wide of the annotated edge for ~60 m
  // — five times the forward gate window, which used to leave the tracker
  // waiting at a gate the kart had long gone past.
  Drive(cs, lap, 0.3, theta, t, [&](pacer::GPSSample s) { lt.OnSample(s); });
  size_t gate_before = lt.Snapshot().gates_crossed;
  DriveWide(cs, lap, 0.1, kJustPastGateEnd, theta, t,
            [&](pacer::GPSSample s) { lt.OnSample(s); });

  // Back on the racing line: the delta has to come back with it, not wait
  // for the start line.
  double delta_at_return = lt.Snapshot().delta_s;
  double t_return = t;
  double t_delta_moved = 0;
  Drive(cs, lap, 0.2, theta, t, [&](pacer::GPSSample s) {
    lt.OnSample(s);
    if (t_delta_moved == 0 && lt.Snapshot().delta_s != delta_at_return) {
      t_delta_moved = s.timestamp_ms / 1000.0;
    }
  });
  REQUIRE(t_delta_moved > 0);
  REQUIRE(t_delta_moved - t_return < 2.0);
  REQUIRE(lt.Snapshot().gates_crossed > gate_before);
  REQUIRE(lt.Snapshot().delta_valid);
  REQUIRE_FALSE(lt.Snapshot().lost);

  // The lap itself still times off the start line, and lap 3 follows.
  Drive(cs, lap, 0.45, theta, t, [&](pacer::GPSSample s) { lt.OnSample(s); });
  auto snap = lt.Snapshot();
  REQUIRE(snap.lap_number == 3);
  REQUIRE(snap.last_lap_s == Approx(lap).margin(0.2));
}

TEST_CASE("LiveTiming drops the lap the kart spends off the circuit",
          "[live-timing]") {
  auto cs = TestCS();
  auto rt = MakeCircularTrack(cs);

  pacer::LiveTiming lt;
  lt.SetReferenceTrack(rt);

  double theta = -0.05, t = 0.0;
  const double lap = 40.0;

  Drive(cs, lap, 2.02, theta, t, [&](pacer::GPSSample s) { lt.OnSample(s); });
  REQUIRE(lt.Snapshot().best_lap_s == Approx(lap).margin(0.1));
  REQUIRE(lt.Snapshot().lap_number == 3);
  double best_before = lt.Snapshot().best_lap_s;

  // Most of lap 3, then off the circuit — the shape of a pit stop, which on
  // a kart track runs alongside the start line and so crosses neither it nor
  // any gate.
  Drive(cs, lap, 0.85, theta, t, [&](pacer::GPSSample s) { lt.OnSample(s); });
  DriveWide(cs, lap, 0.3, kOffCircuit, theta, t,
            [&](pacer::GPSSample s) { lt.OnSample(s); });
  {
    // Out of sight: the delta says so rather than holding its last reading.
    auto snap = lt.Snapshot();
    REQUIRE(snap.lost);
    REQUIRE_FALSE(snap.delta_valid);
  }

  // Rejoin past the start line: the lap in progress was never timed off it,
  // so it is dropped rather than reported as a several-minute lap.
  Drive(cs, lap, 0.3, theta, t, [&](pacer::GPSSample s) { lt.OnSample(s); });
  {
    auto snap = lt.Snapshot();
    REQUIRE_FALSE(snap.delta_valid);
    REQUIRE(std::isnan(snap.current_lap_s));
    REQUIRE(snap.best_lap_s == Approx(best_before).margin(0.001));
    REQUIRE(snap.last_lap_s == Approx(lap).margin(0.2));
  }

  // The next start-line crossing picks everything back up.
  Drive(cs, lap, 1.1, theta, t, [&](pacer::GPSSample s) { lt.OnSample(s); });
  auto snap = lt.Snapshot();
  REQUIRE(snap.delta_valid);
  REQUIRE_FALSE(snap.lost);
  REQUIRE(snap.last_lap_s == Approx(lap).margin(0.3));
  REQUIRE(snap.current_lap_s > 0);
}

TEST_CASE("LiveTiming re-acquires after a burst of dropped fixes",
          "[live-timing]") {
  auto cs = TestCS();
  auto rt = MakeCircularTrack(cs);

  pacer::LiveTiming lt;
  lt.SetReferenceTrack(rt);

  double theta = -0.05, t = 0.0;
  const double lap = 40.0;
  Drive(cs, lap, 1.3, theta, t, [&](pacer::GPSSample s) { lt.OnSample(s); });

  // Seconds with nothing delivered — a full receive queue, or the fix going
  // away under a bridge. 3 s is short enough that the gap can still be
  // integrated from the fix that ends it; 8 s is not, and has to be caught
  // by checking where the kart actually is.
  const double gap_s = GENERATE(3.0, 8.0);
  INFO("gap_s=" << gap_s);
  Drive(cs, lap, gap_s / lap, theta, t, [&](pacer::GPSSample) {});

  double t_resume = t;
  double t_delta_moved = 0;
  double delta_before = lt.Snapshot().delta_s;
  Drive(cs, lap, 0.2, theta, t, [&](pacer::GPSSample s) {
    lt.OnSample(s);
    if (t_delta_moved == 0 && lt.Snapshot().delta_s != delta_before) {
      t_delta_moved = s.timestamp_ms / 1000.0;
    }
  });
  REQUIRE(t_delta_moved > 0);
  REQUIRE(t_delta_moved - t_resume < 2.0);
  REQUIRE(lt.Snapshot().delta_valid);

  // The gap doesn't cost the lap: it still closes on the start line.
  Drive(cs, lap, 0.6, theta, t, [&](pacer::GPSSample s) { lt.OnSample(s); });
  REQUIRE(lt.Snapshot().lap_number == 3);
  REQUIRE(lt.Snapshot().last_lap_s == Approx(lap).margin(0.2));
}

TEST_CASE("LiveTiming reports lateral offset from the track", "[live-timing]") {
  auto cs = TestCS();
  auto rt = MakeCircularTrack(cs);

  pacer::LiveTiming lt;
  REQUIRE_FALSE(lt.OffsetFromTrack(pacer::GPSSample{}).has_value());

  lt.SetReferenceTrack(rt);

  // Gates run inner->outer edge, so "toward second endpoint" == radially
  // outward: at radius R + d the lateral offset must read +d. TimingLine()
  // extends each gate 2 m past both edges, so half-width is 10 + 2.
  // Densified gates are chords between the annotated ones (sagitta ~0.4 m
  // at this radius/spacing), hence the loose margins.
  for (double d : {0.0, 4.0, -7.0, 15.0}) {
    double theta = 0.6; // between annotated gates, on a densified one
    auto s = cs.Global(pacer::Vec3f{(kTrackRadiusM + d) * std::cos(theta),
                                    (kTrackRadiusM + d) * std::sin(theta), 0});
    auto off = lt.OffsetFromTrack(s);
    REQUIRE(off.has_value());
    REQUIRE(off->lateral_m == Approx(d).margin(0.6));
    REQUIRE(off->half_width_m == Approx(12.0).margin(1.0));
    // Inside the gate's extent the fix sits on the segment itself; 15 m out
    // it is past the gate end by roughly 15 - half_width.
    if (std::fabs(d) < off->half_width_m) {
      REQUIRE(off->distance_m < 0.8); // within half a gate spacing-ish
    } else {
      REQUIRE(off->distance_m ==
              Approx(std::fabs(d) - off->half_width_m).margin(0.5));
    }
  }
}

TEST_CASE("LiveTiming session clock starts with lap 1 and expires",
          "[live-timing]") {
  auto cs = TestCS();
  auto rt = MakeCircularTrack(cs);

  pacer::LiveTiming lt;
  lt.SetReferenceTrack(rt, pacer::SessionConfig{.session_length_s = 60});

  // Parked in the pits, half a lap before the start line.
  double theta = -kPi, t = 2.0;
  auto still = cs.Global(pacer::Vec3f{kTrackRadiusM * std::cos(theta),
                                      kTrackRadiusM * std::sin(theta), 0});
  still.full_speed = 0;
  for (int i = 0; i < 50; ++i) {
    still.timestamp_ms = static_cast<int64_t>(std::llround(i / kSampleRateHz * 1000));
    lt.OnSample(still);
  }
  REQUIRE_FALSE(lt.Snapshot().session_started);
  REQUIRE(std::isnan(lt.Snapshot().session_remaining_s));

  // Rolling out: 16 s of out lap at racing speed still doesn't arm the
  // clock, because the start line hasn't been crossed yet.
  Drive(cs, 40.0, 0.4, theta, t, [&](pacer::GPSSample s) { lt.OnSample(s); });
  REQUIRE(lt.Snapshot().lap_number == 0);
  REQUIRE_FALSE(lt.Snapshot().session_started);
  REQUIRE(std::isnan(lt.Snapshot().session_remaining_s));
  REQUIRE(std::isnan(lt.Snapshot().session_elapsed_s));

  // Cross the line (0.1 lap in) and keep going: 80 s of timed running on a
  // 60 s session, so the clock arms and the countdown goes negative.
  Drive(cs, 40.0, 2.1, theta, t, [&](pacer::GPSSample s) { lt.OnSample(s); });

  auto snap = lt.Snapshot();
  REQUIRE(snap.session_started);
  // Timed from the lap-1 crossing, not from roll-out: 80 s, not the 96 s
  // the kart has been moving.
  REQUIRE(snap.session_elapsed_s == Approx(80.0).margin(0.5));
  REQUIRE(snap.session_remaining_s < 0);
  REQUIRE(snap.session_remaining_s == Approx(60.0 - 80.0).margin(0.5));
}

TEST_CASE("LiveTiming::ResetSession starts a fresh session on the same track",
          "[live-timing]") {
  auto cs = TestCS();
  auto rt = MakeCircularTrack(cs);

  pacer::LiveTiming lt;
  lt.SetReferenceTrack(rt, pacer::SessionConfig{.session_length_s = 600});

  double theta = -0.05, t = 100.0;
  Drive(cs, 40.0, 2.2, theta, t, [&](pacer::GPSSample s) { lt.OnSample(s); });
  REQUIRE(lt.Snapshot().lap_number == 3);
  REQUIRE(lt.Snapshot().session_started);

  lt.ResetSession();
  {
    auto snap = lt.Snapshot();
    REQUIRE(snap.lap_number == 0);
    REQUIRE_FALSE(snap.session_started);
    REQUIRE(std::isnan(snap.session_remaining_s));
    REQUIRE(std::isnan(snap.current_lap_s));
    REQUIRE(std::isnan(snap.last_lap_s));
    REQUIRE(std::isnan(snap.best_lap_s));
    REQUIRE_FALSE(snap.delta_valid);
    // The track itself survives, so timing picks up without a reload.
    REQUIRE(snap.gate_count > 0);
  }

  // More laps at a different pace: numbering and the delta reference both
  // restart from this session's laps, not the discarded ones.
  const double slower = 44.0;
  Drive(cs, slower, 3.0, theta, t, [&](pacer::GPSSample s) { lt.OnSample(s); });
  auto snap = lt.Snapshot();
  REQUIRE(snap.lap_number == 3);
  REQUIRE(snap.last_lap_s == Approx(slower).margin(0.1));
  REQUIRE(snap.best_lap_s == Approx(slower).margin(0.1));
  REQUIRE(snap.session_started);
}
