#include <nanobind/nanobind.h>

#include "geometry.hpp"

#include <pacer/datatypes/datatypes.hpp>
#include <pacer/geometry/geometry.hpp>

namespace nb = nanobind;

NB_MODULE(_pacer_geometry_impl, m) {
  nb::class_<pacer::Vec3f>(m, "Vec3f")
      .def(nb::init<>())
      .def(nb::init<double, double, double>())
      .def("__getitem__",
           [](const pacer::Vec3f &v, size_t i) {
             if (i >= 3) {
               throw std::out_of_range("Index out of range for Vec3f");
             }
             return v[i];
           })
      .def("__setitem__", [](pacer::Vec3f &v, size_t i, double value) {
        if (i >= 3) {
          throw std::out_of_range("Index out of range for Vec3f");
        }
        v[i] = value;
      });

  nb::class_<pacer::GPSSample>(m, "GPSSample")
      .def(nb::init<double, double, double, double, double>())
      .def_ro("latitude", &pacer::GPSSample::lat)
      .def_ro("longitude", &pacer::GPSSample::lon)
      .def_ro("altitude", &pacer::GPSSample::altitude)
      .def_ro("full_speed", &pacer::GPSSample::full_speed)
      .def_ro("ground_speed", &pacer::GPSSample::ground_speed)
      .def("__repr__", [](const pacer::GPSSample &s) {
        return nb::str("GPSSample(lat={}, lon={}, altitude={}, full_speed={}, "
                       "ground_speed={})")
            .format(s.lat, s.lon, s.altitude, s.full_speed, s.ground_speed);
      });

  nb::class_<pacer::CoordinateSystem>(m, "CoordinateSystem")
      .def(nb::init<pacer::GPSSample>())
      .def("Local", &pacer::CoordinateSystem::Local, nb::arg("point"))
      .def("Global", &pacer::CoordinateSystem::Global, nb::arg("point"));
}
