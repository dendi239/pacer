#include "gps-source.hpp"

#include <functional>

#include <nanobind/nanobind.h>
#include <nanobind/stl/function.h>
#include <nanobind/stl/pair.h>

namespace nb = nanobind;
using namespace nb::literals;

NB_MODULE(_pacer_gps_source_impl, m) {
  nb::class_<pacer::RawGPSSource>(m, "GPSSource")
      .def("seek", &pacer::RawGPSSource::Seek, nb::arg("target"))
      .def("next", &pacer::RawGPSSource::Next)
      .def("is_end", &pacer::RawGPSSource::IsEnd)
      .def("current_time_span", &pacer::RawGPSSource::CurrentTimeSpan)
      .def("get_total_duration", &pacer::RawGPSSource::GetTotalDuration);

  nb::class_<pacer::GPMFSource, pacer::RawGPSSource>(m, "GPMFSource")
      .def(nb::init<const char *>())
      .def(
          "samples",
          [](pacer::GPMFSource &self,
             std::function<void(pacer::GPSSample, size_t, size_t)> on_sample) {
            return self.Samples(
                &on_sample,
                [](void *data_tuple, pacer::GPSSample s, size_t current,
                   size_t total) -> void {
                  auto on_sample = *reinterpret_cast<
                      std::function<void(pacer::GPSSample, size_t, size_t)> *>(
                      data_tuple);
                  on_sample(s, current, total);
                });
          },
          nb::arg("on_sample"))
      .def("seek", &pacer::GPMFSource::Seek, nb::arg("target"))
      .def("next", &pacer::GPMFSource::Next)
      .def("is_end", &pacer::GPMFSource::IsEnd)
      .def("current_time_span", &pacer::GPMFSource::CurrentTimeSpan)
      .def("get_total_duration", &pacer::GPMFSource::GetTotalDuration);

  nb::class_<pacer::SequentialGPSSource, pacer::RawGPSSource>(
      m, "SequentialGPSSource")
      .def(nb::init<pacer::RawGPSSource *, pacer::RawGPSSource *>())
      .def("get_total_duration", &pacer::SequentialGPSSource::GetTotalDuration)
      .def("is_end", &pacer::SequentialGPSSource::IsEnd)
      .def("samples", &pacer::SequentialGPSSource::Samples, nb::arg("data"),
           nb::arg("on_sample"))
      .def("seek", &pacer::SequentialGPSSource::Seek, nb::arg("target"))
      .def("next", &pacer::SequentialGPSSource::Next)
      .def("current_time_span", &pacer::SequentialGPSSource::CurrentTimeSpan);

  nb::enum_<pacer::DatVersion>(m, "DatVersion")
      .value("JUST_DATA", pacer::DatVersion::JUST_DATA)
      .value("WITH_TIMESTAMP", pacer::DatVersion::WITH_TIMESTAMP)
      .export_values();

  m.def(
      "read_dat_file",
      [](const char *filename,
         std::function<void(pacer::GPSSample, double)> on_sample,
         pacer::DatVersion version = pacer::DatVersion::JUST_DATA) {
        pacer::ReadDatFile(filename, on_sample, version);
      },
      nb::arg("filename"), nb::arg("on_sample"),
      nb::arg("version") = pacer::DatVersion::JUST_DATA);
}
