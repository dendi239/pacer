#include <pacer/laps/laps.hpp>

#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/vector.h>

namespace nb = nanobind;

NB_MODULE(_pacer_laps, m) {
  using namespace pacer;
  using namespace nanobind::literals;

  // Bind Sectors struct
  nb::class_<Sectors>(m, "Sectors")
      .def_rw("start_line", &Sectors::start_line)
      .def_rw("sector_lines", &Sectors::sector_lines);

  // Bind Lap struct
  nb::class_<Lap>(m, "Lap")
      .def_rw("width", &Lap::width)
      .def_rw("points", &Lap::points)
      .def_rw("cum_distances", &Lap::cum_distances)
      .def("fill_distances", &Lap::FillDistances, "cs"_a)
      .def("lap_time", &Lap::LapTime)
      .def("count", &Lap::Count)
      .def("resample", &Lap::Resample, "lap"_a, "cs"_a)
      .def("timing_lines_count", &Lap::TimingLinesCount)
      .def("timing_line", &Lap::TimingLine, "index"_a, "cs"_a);

  nb::class_<Segment>(m, "Segment")
      .def(nb::init<>())
      .def(nb::init<Point, Point>())
      .def("__eq__", &Segment::operator==)
      .def("__repr__",
           [](const Segment &s) {
             return nb::str("Segment(first={}, second={})")
                 .format(s.first, s.second);
           })
      .def_rw("first", &Segment::first)
      .def_rw("second", &Segment::second)
      .def(
          "intersects",
          [](const Segment &s, pacer::Point fst,
             pacer::Point snd) -> std::optional<double> {
            double ratio;
            if (!s.Intersects(fst, snd, &ratio)) {
              return std::nullopt;
            }
            return ratio;
          },
          "fst"_a, "snd"_a)
      .def("__repr__", [](const Segment &s) {
        return nb::str("Segment(first={}, second={})")
            .format(s.first, s.second);
      });

  // Bind Laps class
  nb::class_<Laps>(m, "Laps")
      .def(nb::init<>())
      .def("update", &Laps::Update)
      .def("pick_random_start", &Laps::PickRandomStart)
      .def("set_coordinate_system", &Laps::SetCoordinateSystem,
           "coordinate_system"_a)
      .def("min_max", &Laps::MinMax)
      .def_rw("sectors", &Laps::sectors)
      .def("laps_count", &Laps::LapsCount)
      .def("lap_entry_speed", &Laps::LapEntrySpeed, "lap"_a)
      .def("lap_time", &Laps::LapTime, "lap"_a)
      .def("sample_count", &Laps::SampleCount, "lap"_a)
      .def("start_timestamp", &Laps::StartTimestamp, "lap"_a)
      .def("at", &Laps::At, "lap"_a, "row"_a)
      .def("speed", &Laps::Speed, "lap"_a, "row"_a)
      .def("distance", &Laps::Distance, "lap"_a, "row"_a)
      .def("get_lap_distance", &Laps::GetLapDistance, "index"_a, "cs"_a)
      .def("get_lap", &Laps::GetLap, "lap"_a)
      .def("sector_count", &Laps::SectorCount)
      .def("recorded_sectors", &Laps::RecordedSectors)
      .def("clear_sectors", &Laps::ClearSectors)
      .def("sector_time", &Laps::SectorTime, "sector"_a)
      .def("sector_start_timestamp", &Laps::SectorStartTimestamp, "sector"_a)
      .def("sector_entry_speed", &Laps::SectorEntrySpeed, "sector"_a)
      .def("add_point", &Laps::AddPoint, "s"_a, "t"_a)
      .def("get_point", &Laps::GetPoint, "i"_a)
      .def("point_count", &Laps::PointCount)
      .def("get_point", &Laps::GetPoint, "row"_a)
      .def("clear_point", &Laps::ClearPoints);
}