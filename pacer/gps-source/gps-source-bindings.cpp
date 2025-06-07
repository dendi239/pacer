#include "gps-source.hpp"

#include <functional>

#include <nanobind/nanobind.h>
#include <nanobind/stl/function.h>
#include <nanobind/stl/pair.h>

namespace nb = nanobind;
using namespace nb::literals;

NB_MODULE(_pacer_gps_source_impl, m) {
  nb::class_<pacer::GPSSource>(m, "GPSSource")
      .def("Seek", &pacer::GPSSource::Seek, nb::arg("target"))
      .def("Next", &pacer::GPSSource::Next)
      .def("IsEnd", &pacer::GPSSource::IsEnd)
      .def("CurrentTimeSpan", &pacer::GPSSource::CurrentTimeSpan)
      .def("GetTotalDuration", &pacer::GPSSource::GetTotalDuration);

  nb::class_<pacer::GPMFSource, pacer::GPSSource>(m, "GPMFSource")
      .def(nb::init<const char *>())
      .def(
          "Samples",
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
      .def("Seek", &pacer::GPMFSource::Seek, nb::arg("target"))
      .def("Next", &pacer::GPMFSource::Next)
      .def("IsEnd", &pacer::GPMFSource::IsEnd)
      .def("CurrentTimeSpan", &pacer::GPMFSource::CurrentTimeSpan)
      .def("GetTotalDuration", &pacer::GPMFSource::GetTotalDuration);

  nb::class_<pacer::SequentialGPSSource, pacer::GPSSource>(
      m, "SequentialGPSSource")
      .def(nb::init<pacer::GPSSource *, pacer::GPSSource *>())
      .def("GetTotalDuration", &pacer::SequentialGPSSource::GetTotalDuration)
      .def("IsEnd", &pacer::SequentialGPSSource::IsEnd)
      .def("Samples", &pacer::SequentialGPSSource::Samples, nb::arg("data"),
           nb::arg("on_sample"))
      .def("Seek", &pacer::SequentialGPSSource::Seek, nb::arg("target"))
      .def("Next", &pacer::SequentialGPSSource::Next)
      .def("CurrentTimeSpan", &pacer::SequentialGPSSource::CurrentTimeSpan);
}
