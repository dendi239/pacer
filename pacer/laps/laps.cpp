#include "laps.hpp"

#include <cmath>
#include <cstdio>

#include <pacer/datatypes/datatypes.hpp>
#include <pacer/geometry/geometry.hpp>

void pacer::Laps::Update() {
  if (sectors.start_line == dirty_start_line_ &&
      sectors.sector_lines == dirty_sector_lines_)
    return;

  dirty_start_line_ = sectors.start_line;
  dirty_sector_lines_ = sectors.sector_lines;

  laps_.clear();
  sectors_.clear();

  PointInTime<GPSSample> previous;

  int sector_index = -1;
  auto sector_line = [&] {
    return sector_index == -1 ? sectors.start_line
                              : sectors.sector_lines[sector_index];
  };

  auto to_global = [&](Segment x) -> Segment {
    auto gps_first = cs_.Global(Vec3f{x.first.x, x.first.y, 0});
    auto gps_second = cs_.Global(Vec3f{x.second.x, x.second.y, 0});
    return Segment{.first = {gps_first.lon, gps_first.lat},
                   .second = {gps_second.lon, gps_second.lat}};
  };

  for (size_t i = 0; i < points_.size(); ++i) {
    PointInTime<GPSSample> current = points_[i];

    auto lap_split = Split(to_global(sectors.start_line), previous, current);
    auto sector_split = Split(to_global(sector_line()), previous, current);

    previous = current;

    if (lap_split) {
      if (!laps_.empty()) {
        laps_.back().finish = *lap_split;
        laps_.back().finish_index = i;
      }

      laps_.push_back(LapChunk{.start = *lap_split,
                               .finish = *lap_split,
                               .start_index = i,
                               .finish_index = i});
    }

    if (sector_split) {
      if (!sectors_.empty()) {
        sectors_.back().finish = *sector_split;
        sectors_.back().finish_index = i;
      }

      sectors_.push_back(LapChunk{
          .start = *sector_split,
          .finish = *sector_split,
          .start_index = i,
          .finish_index = i,
      });

      sector_index += 1;
      if (sector_index == sectors.sector_lines.size())
        sector_index = -1;
    }
  }
}

pacer::Segment pacer::Laps::PickRandomStart() const {
  if (points_.empty())
    return Segment{{0, 0}, {0, 0}};

  // If we don't have enough samples to pick the +20 offset, clamp safely.
  size_t mid = points_.size() / 2;
  size_t snd_idx = std::min(points_.size() - 1, mid + (size_t)20);

  auto fst = points_[mid].point;
  auto snd = points_[snd_idx].point;

  auto s1 = cs_.Local(fst);
  auto s2 = cs_.Local(snd);

  auto p1 = Point{s1[0], s1[1]}, p2 = Point{s2[0], s2[1]};
  auto m = (p1 + p2) / 2, dir = (p2 - p1);

  double norm = std::sqrt(dir.Norm());
  if (norm == 0.0)
    return Segment{m, m};

  dir /= norm;
  dir = Point{-dir[1], dir[0]};

  // offset start midpoint by 5m
  return Segment{m - dir * 5, m + dir * 5};
}

auto pacer::Laps::MinMax() const -> std::pair<Point, Point> {
  if (points_.empty())
    return {Point{0, 0}, Point{0, 0}};

  Point min{points_[0].point.lon, points_[0].point.lat}, max = min;
  for (const auto &pit : points_) {
    const auto &p = pit.point;
    min.x = std::min(min.x, p.lon);
    max.x = std::max(max.x, p.lon);
    min.y = std::min(min.y, p.lat);
    max.y = std::max(max.y, p.lat);
  }
  return {min, max};
}

double pacer::Laps::LapChunk::Time() const { return finish.time - start.time; }

double pacer::Laps::GetLapDistance(size_t lap,
                                   const CoordinateSystem &cs) const {
  if (lap >= laps_.size())
    return 0.0;
  double distance = cs.Distance(laps_[lap].start.point,
                                points_[laps_[lap].start_index].point) +
                    cs.Distance(laps_[lap].finish.point,
                                points_[laps_[lap].finish_index].point) +
                    cum_point_dist_[laps_[lap].finish_index] -
                    cum_point_dist_[laps_[lap].start_index];

  return distance;
}

pacer::PointInTime<pacer::GPSSample> pacer::Laps::At(size_t lap,
                                                     size_t row) const {
  if (lap >= laps_.size())
    return {};
  if (row == 0) {
    return laps_[lap].start;
  }
  if (row - 1 >= laps_[lap].finish_index - laps_[lap].start_index) {
    return laps_[lap].finish;
  }
  return points_[laps_[lap].start_index + row - 1];
}

double pacer::Laps::Speed(size_t lap, size_t row) const {
  if (lap >= laps_.size())
    return 0.0;
  return At(lap, row).point.full_speed;
}

double pacer::Laps::Distance(size_t lap, size_t row) const {
  double distance = 0;

  if (lap >= laps_.size() || row <= 0)
    return distance;

  distance += cs_.Distance(laps_[lap].start.point,
                           points_[laps_[lap].start_index].point);

  size_t point_index = row - 1 + laps_[lap].start_index;
  distance += cum_point_dist_[std::min(point_index, laps_[lap].finish_index)] -
              cum_point_dist_[laps_[lap].start_index];

  if (point_index <= laps_[lap].finish_index)
    return distance;

  distance += cs_.Distance(points_[laps_[lap].finish_index].point,
                           laps_[lap].finish.point);

  return distance;
}

double pacer::Laps::LapTime(size_t lap) const {
  if (lap >= laps_.size())
    return 0.0;
  return laps_[lap].Time();
}

size_t pacer::Laps::SampleCount(size_t lap) const {
  if (lap >= laps_.size()) {
    return 0;
  }
  return laps_[lap].finish_index - laps_[lap].start_index + 3;
}

double pacer::Laps::StartTimestamp(size_t lap) const {
  if (lap >= laps_.size())
    return 0.0;
  return laps_[lap].start.time;
}

pacer::Lap pacer::Laps::GetLap(size_t lap) const {
  if (lap >= laps_.size())
    return {};
  std::vector<PointInTime<GPSSample>> points{laps_[lap].start};
  points.insert(points.end(), points_.begin() + laps_[lap].start_index,
                points_.begin() + laps_[lap].finish_index);
  points.push_back(laps_[lap].finish);
  auto l = Lap{.points = points, .cum_distances = {}};
  l.FillDistances(cs_);
  return l;
}

double pacer::Laps::SectorStartTimestamp(size_t sector) const {
  return sectors_[sector].start.time;
}

double pacer::Laps::SectorEntrySpeed(size_t sector) const {
  return sectors_[sector].start.point.full_speed;
}

double pacer::Laps::SectorTime(size_t sector) {
  return sectors_[sector].finish.time - sectors_[sector].start.time;
}

size_t pacer::Laps::SectorCount() const { return sectors.sector_lines.size(); }

double pacer::Laps::LapEntrySpeed(size_t lap) const {
  if (lap >= laps_.size())
    return 0.0;
  return laps_[lap].start.point.full_speed;
}

size_t pacer::Laps::LapsCount() const { return laps_.size(); }

void pacer::Laps::ClearSectors() { sectors.sector_lines.clear(); }

void pacer::Laps::AddPoint(GPSSample s, double t) {
  if (!points_.empty()) {
    cum_point_dist_.push_back(cum_point_dist_.back() +
                              cs_.Distance(points_.back().point, s));
  }
  points_.emplace_back(s, t);
}

size_t pacer::Laps::PointCount() const { return points_.size(); }

pacer::PointInTime<pacer::GPSSample> pacer::Laps::GetPoint(size_t row) const {
  return points_[row];
}

void pacer::Laps::SetCoordinateSystem(CoordinateSystem coordinate_system) {
  cs_ = coordinate_system;
  // Ensure cum_point_dist_ has the same size as points_ and is initialized
  cum_point_dist_.assign(points_.size(), 0.0);
  for (size_t i = 0; i + 1 < points_.size(); ++i) {
    cum_point_dist_[i + 1] =
        cum_point_dist_[i] +
        cs_.Distance(points_[i].point, points_[i + 1].point);
  }
}
size_t pacer::Laps::RecordedSectors() const { return sectors_.size(); }

size_t pacer::Lap::Count() const { return points.size(); }

void pacer::Lap::FillDistances(const CoordinateSystem &cs) {
  cum_distances = std::vector<double>{0};
  for (size_t i = 1; i < points.size(); ++i) {
    cum_distances.push_back(cum_distances.back() +
                            cs.Distance(points[i - 1].point, points[i].point));
  }
}
double pacer::Lap::LapTime() const {
  return points.back().time - points.front().time;
}
void pacer::Laps::ClearPoints() {
  points_.clear();
  // keep a single zero entry so AddPoint and SetCoordinateSystem behave
  // consistently (cum_point_dist_[0] == 0)
  cum_point_dist_.assign(1, 0.0);
}
