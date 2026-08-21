#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include <pacer/geometry/geometry.hpp>
#include <pacer/laps/laps.hpp>

namespace {

// Adds a straight run along the local x axis from `from_m` to `to_m`
// (meters), one sample per meter, advancing `timestamp_ms` as it goes.
void AddStraightRun(pacer::Laps &laps, const pacer::CoordinateSystem &cs,
                    double from_m, double to_m, int64_t &timestamp_ms) {
  double step = from_m < to_m ? 1.0 : -1.0;
  for (double x = from_m; step > 0 ? x <= to_m : x >= to_m; x += step) {
    auto sample = cs.Global(pacer::Vec3f{x, 0, 0});
    sample.timestamp_ms = timestamp_ms;
    timestamp_ms += 40; // 25 Hz
    laps.AddPoint(sample);
  }
}

} // namespace

TEST_CASE("ClearPoints forces re-split when identical sectors are re-applied",
          "[laps][reload]") {
  pacer::CoordinateSystem cs(pacer::GPSSample{.lat = 51.0, .lon = 0.0});

  pacer::Laps laps;
  laps.SetCoordinateSystem(cs);

  // A gate across the x axis at x = 0, in the same local frame as cs.
  pacer::Sectors sectors;
  sectors.start_line = pacer::Segment{{0, -10}, {0, 10}};

  // Drive back and forth over the gate: 3 crossings. The half-meter offset
  // keeps samples off the gate itself (Intersects is strict about that).
  int64_t timestamp_ms = 1'000;
  AddStraightRun(laps, cs, -20.5, 20.5, timestamp_ms);
  AddStraightRun(laps, cs, 20.5, -20.5, timestamp_ms);
  AddStraightRun(laps, cs, -20.5, 20.5, timestamp_ms);

  laps.sectors = sectors;
  laps.Update();
  REQUIRE(laps.LapsCount() == 3);

  // Reload: new data with a single crossing, but the timing lines are
  // re-applied unchanged (their frame outlives the data). Update() must
  // still re-split; stale chunks would index into the cleared points.
  laps.ClearPoints();
  AddStraightRun(laps, cs, -20.5, 20.5, timestamp_ms);

  laps.sectors = sectors;
  laps.Update();
  REQUIRE(laps.LapsCount() == 1);
}
