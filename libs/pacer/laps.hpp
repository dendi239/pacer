#pragma once

#include <vector>

#include "datatypes.hpp"
#include "geometry.hpp"

namespace pacer {

struct LapChunk {
  PointInTime<GPSSample> start, finish;
  size_t start_index, finish_index;

  double Time() const;
  size_t Count() const { return finish_index - start_index; }
};

struct Laps {
  Segment start_line;
  std::vector<Segment> sector_lines;

  std::vector<std::pair<GPSSample, double>> points;
  std::vector<LapChunk> laps;
  std::vector<LapChunk> sectors;

  /// Updates all laps given updated start_line
  void Update();

  /// Picks a starting point for start_line.
  /// Default implementation builds segment perpendicular to median segment.
  Segment PickRandomStart() const;

  /// Gets bounding box for entire thing, might be cached
  /// as depends on points only.
  auto MinMax() const -> std::pair<Point, Point>;
};

} // namespace pacer
