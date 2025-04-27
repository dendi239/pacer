#include "laps.hpp"

#include <iostream>

#include "datatypes.hpp"
#include "geometry.hpp"

void pacer::Laps::Update() {
  laps.clear();
  sectors.clear();

  PointInTime<GPSSample> previous;

  int sector_index = -1;
  auto sector_line = [&] {
    if (sector_index == -1)
      return start_line;
    return sector_lines[sector_index];
  };

  for (size_t i = 0; i < points.size(); ++i) {
    PointInTime<GPSSample> current{.point = points[i].first,
                                   .time = points[i].second};
    auto lap_split = Split(start_line, previous, current);
    auto sector_split = Split(sector_line(), previous, current);

    previous = current;

    if (lap_split) {
      if (!laps.empty()) {
        laps.back().finish = *lap_split;
        laps.back().finish_index = i;
      }

      laps.push_back(LapChunk{.start = *lap_split,
                              .finish = *lap_split,
                              .start_index = i,
                              .finish_index = i});
    }

    if (sector_split) {
      if (!sectors.empty()) {
        sectors.back().finish = *sector_split;
        sectors.back().finish_index = i;
      }

      sectors.push_back(LapChunk{
          .start = *sector_split,
          .finish = *sector_split,
          .start_index = i,
          .finish_index = i,
      });

      sector_index += 1;
      if (sector_index == sector_lines.size())
        sector_index = -1;
    }
  }
}

pacer::Segment pacer::Laps::PickRandomStart() const {
  auto fst = points[points.size() / 2].first;
  auto snd = points[points.size() / 2 + 1].first;

  auto s1 = Point{.x = fst.lon, .y = fst.lat};
  auto s2 = Point{.x = snd.lon, .y = snd.lat};

  auto m = s1 + (s2 - s1) * 0.5;
  return {m + (s1 - m) * Point(0, 5), m + (s2 - m) * Point(0, 5)};
}

auto pacer::Laps::MinMax() const -> std::pair<Point, Point> {
  Point min{points[0].first.lon, points[0].first.lat}, max = min;
  for (auto [p, _] : points) {
    min.x = std::min(min.x, p.lon);
    max.x = std::max(max.x, p.lon);
    min.y = std::min(min.y, p.lat);
    max.y = std::max(max.y, p.lat);
  }
  return {min, max};
}

double pacer::LapChunk::Time() const { return finish.time - start.time; }
