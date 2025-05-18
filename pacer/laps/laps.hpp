#pragma once

#include <vector>

#include <pacer/datatypes/datatypes.hpp>
#include <pacer/geometry/geometry.hpp>

namespace pacer {

struct Lap {
  float width;

  std::vector<PointInTime<GPSSample>> points;
  std::vector<double> cum_distances;

  void FillDistances(const CoordinateSystem &cs);

  double LapTime() const;

  size_t Count() const;
  Lap Resample(const Lap &lap, const CoordinateSystem &cs) const;

  size_t TimingLinesCount() const;
  Segment TimingLine(size_t index, const CoordinateSystem &cs) const;
};

struct Sectors {
  Segment start_line;
  std::vector<Segment> sector_lines;
};

struct Laps {
  /// Updates all laps given updated start_line and sector_lines
  void Update();

  /// Picks a starting point for start_line.
  /// Default implementation builds segment perpendicular to median segment.
  Segment PickRandomStart() const;

  //---------------------------- PRESENTATION -------------------------------//

  void SetCoordinateSystem(CoordinateSystem coordinate_system);

  /// Gets bounding box for entire thing, might be cached
  /// as depends on points only.
  auto MinMax() const -> std::pair<Point, Point>;

  //-------------------------------- LAPS -----------------------------------//

  Sectors sectors;

  size_t LapsCount() const;
  double LapEntrySpeed(size_t lap) const;
  double LapTime(size_t lap) const;
  size_t SampleCount(size_t lap) const;
  double StartTimestamp(size_t lap) const;
  PointInTime<GPSSample> At(size_t lap, size_t row) const;
  double Speed(size_t lap, size_t row) const;
  double Distance(size_t lap, size_t row) const;
  double GetLapDistance(size_t index, const CoordinateSystem &cs) const;

  Lap GetLap(size_t lap) const;

  //------------------------------- SECTORS ---------------------------------//

  size_t SectorCount() const;
  size_t RecordedSectors() const;
  void ClearSectors();
  double SectorTime(size_t sector);
  double SectorStartTimestamp(size_t sector) const;
  double SectorEntrySpeed(size_t sector) const;

  //------------------------------ RAW POINTS -------------------------------//

  void AddPoint(GPSSample s, double t);
  size_t PointCount() const;
  PointInTime<GPSSample> GetPoint(size_t row) const;

private:
  struct LapChunk {
    PointInTime<GPSSample> start, finish;
    size_t start_index, finish_index;

    double Time() const;
    size_t Count() const { return finish_index - start_index; }
  };

  CoordinateSystem cs_;

  std::vector<PointInTime<GPSSample>> points_;
  std::vector<double> cum_point_dist_{0};

  std::vector<LapChunk> laps_;
  std::vector<LapChunk> sectors_;

  Segment dirty_start_line_ = {};
  std::vector<Segment> dirty_sector_lines_ = {};
};

} // namespace pacer
