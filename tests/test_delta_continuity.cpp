#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

#include <pacer/datatypes/datatypes.hpp>
#include <pacer/geometry/geometry.hpp>
#include <pacer/gps-source/gps-source.hpp>
#include <pacer/laps/laps.hpp>
#include <pacer/reference-track/reference-track.hpp>

TEST_CASE("Delta continuity for Daytona Sandown Park GX010316",
          "[lap][continuity]") {
  const std::string filename =
      "/Users/denys/Library/CloudStorage/GoogleDrive-dendi239@gmail.com/My "
      "Drive/karting/videos/Daytona Sandown Park/GX010316.MP4";

  REQUIRE(std::filesystem::exists(filename));

  pacer::GPMFSource source(filename.c_str());
  source.Seek(0);

  pacer::Laps laps;

  while (!source.IsEnd()) {
    auto [start, end] = source.CurrentTimeSpan();

    source.RawGPSSource::Samples([&](pacer::GPSSample sample, size_t current,
                                     size_t total) {
      if (total == 0)
        return;
      if (sample.timestamp_ms == 0) {
        // File predates timestamped samples: synthesize a clock from the
        // MP4 chunk spans.
        double t = start + (end - start) * static_cast<double>(current) /
                               static_cast<double>(total);
        sample.timestamp_ms = static_cast<int64_t>(t * 1000);
      }
      laps.AddPoint(sample);
    });
    source.Next();
  }

  REQUIRE(laps.PointCount() > 0);

  laps.SetCoordinateSystem(pacer::CoordinateSystem(laps.GetPoint(0)));
  laps.sectors.start_line = laps.PickRandomStart();
  laps.Update();

  REQUIRE(laps.LapsCount() > 1);

  std::vector<size_t> lap_ids(laps.LapsCount());
  std::iota(lap_ids.begin(), lap_ids.end(), 0);

  std::vector<double> lap_times;
  lap_times.reserve(laps.LapsCount());
  for (size_t i = 0; i < laps.LapsCount(); ++i) {
    lap_times.push_back(laps.LapTime(i));
  }

  double best_time = std::numeric_limits<double>::infinity();
  for (double lap_time : lap_times) {
    if (lap_time > 0.0 && lap_time < best_time) {
      best_time = lap_time;
    }
  }
  REQUIRE(best_time < std::numeric_limits<double>::infinity());
  REQUIRE(best_time > 10.0);
  REQUIRE(best_time < 120.0);

  std::vector<size_t> close_laps;
  for (size_t i = 0; i < lap_times.size(); ++i) {
    if (lap_times[i] > 0.0 &&
        std::abs(lap_times[i] - best_time) <= 0.01 * best_time) {
      close_laps.push_back(i);
    }
  }

  REQUIRE(close_laps.size() >= 2);

  const size_t reference_lap_id = *std::min_element(
      close_laps.begin(), close_laps.end(),
      [&](size_t a, size_t b) { return lap_times[a] < lap_times[b]; });
  pacer::Lap reference_lap = laps.GetLap(reference_lap_id);

  pacer::CoordinateSystem cs(reference_lap.points.front());
  reference_lap.FillDistances(cs);

  pacer::ReferenceTrack reference_track =
      pacer::ReferenceTrack::FromLap(reference_lap, 7.0f, cs);

  const double continuity_threshold = 0.5;
  for (size_t lap_id : close_laps) {
    if (lap_id == reference_lap_id)
      continue;

    if (lap_id == 2) {
      auto raw_lap = laps.GetLap(lap_id);
      std::cout << "raw lap " << lap_id << " first points:\n";
      for (size_t i = 0; i < std::min<size_t>(12, raw_lap.points.size()); ++i) {
        auto &p = raw_lap.points[i];
        auto t = (raw_lap.points[i].timestamp_ms -
                  raw_lap.points[0].timestamp_ms) /
                 1000.0;
        std::cout << "  idx=" << i << " t=" << t << " lat=" << p.lat
                  << " lon=" << p.lon << "\n";
      }
      std::cout << "\n";
    }

    pacer::Lap comparison_lap = reference_track.Resample(laps.GetLap(lap_id));
    comparison_lap.FillDistances(cs);

    const size_t point_count =
        std::min(reference_lap.points.size(), comparison_lap.points.size());
    INFO("reference_id=" << reference_lap_id << " lap_id=" << lap_id
                         << " reference_points=" << reference_lap.points.size()
                         << " comparison_points="
                         << comparison_lap.points.size());
    REQUIRE(point_count > 2);

    double previous_delta = 0.0;
    for (size_t index = 0; index < point_count; ++index) {
      const double reference_time = (reference_lap.points[index].timestamp_ms -
                                     reference_lap.points[0].timestamp_ms) /
                                    1000.0;
      const double comparison_time =
          (comparison_lap.points[index].timestamp_ms -
           comparison_lap.points[0].timestamp_ms) /
          1000.0;
      const double delta = comparison_time - reference_time;
      const auto &ref_point = reference_lap.points[index];
      const auto &cmp_point = comparison_lap.points[index];
      if (lap_id == 2 && index < 10) {
        std::cout << "lap_id=" << lap_id << " idx=" << index
                  << " ref_t=" << reference_time << " cmp_t=" << comparison_time
                  << " delta=" << delta << " ref_lat=" << ref_point.lat
                  << " ref_lon=" << ref_point.lon
                  << " cmp_lat=" << cmp_point.lat
                  << " cmp_lon=" << cmp_point.lon << "\n";
      }
      INFO("idx=" << index << " ref=(" << ref_point.lat << "," << ref_point.lon
                  << ") t=" << reference_time << " cmp=(" << cmp_point.lat
                  << "," << cmp_point.lon << ") t=" << comparison_time
                  << " delta=" << delta);

      if (index > 0) {
        INFO("reference_id="
             << reference_lap_id << " (" << reference_lap.LapTime()
             << "s) lap_id=" << lap_id << " (" << comparison_lap.LapTime()
             << "s)"
             << " index=" << index << " reference_time=" << reference_time
             << " comparison_time=" << comparison_time << " delta=" << delta
             << " previous_delta=" << previous_delta);
        REQUIRE(std::abs(delta - previous_delta) <= continuity_threshold);
      }
      previous_delta = delta;
    }
  }
}
