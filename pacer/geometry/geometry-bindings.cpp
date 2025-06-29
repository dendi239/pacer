#include <nanobind/nanobind.h>
#include <nanobind/operators.h> // Required for operator overloading

#include "geometry.hpp"

#include <pacer/datatypes/datatypes.hpp>
#include <pacer/geometry/geometry.hpp>

namespace nb = nanobind;

NB_MODULE(_pacer_geometry_impl, m) {
  nb::class_<pacer::Vec3f>(m, "Vec3f")
      .def(nb::init<>())
      .def(nb::init<double, double, double>(), nb::arg("x"), nb::arg("y"),
           nb::arg("z"))
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

  nb::class_<pacer::PointInTime<pacer::GPSSample>>(m, "GPSPointInTime")
      .def(nb::init<pacer::GPSSample, double>())
      .def_ro("point", &pacer::PointInTime<pacer::GPSSample>::point)
      .def_ro("time", &pacer::PointInTime<pacer::GPSSample>::time)
      .def("__repr__", [](const pacer::PointInTime<pacer::GPSSample> &pit) {
        return nb::str("GPSPointInTime(point={}, time={})")
            .format(pit.point, pit.time);
      });

  nb::class_<pacer::Point>(m, "Point")
      .def(nb::init<>())
      .def(nb::init<double, double>(), nb::arg("x"), nb::arg("y"))
      .def_rw("x", &pacer::Point::x)
      .def_rw("y", &pacer::Point::y)
      .def("scalar", &pacer::Point::Scalar, nb::arg("other"))
      .def("__add__", std::plus<pacer::Point>{}, nb::arg("other"))
      .def("__sub__", std::minus<pacer::Point>{}, nb::arg("other"))
      .def(
          "__mul__",
          [](const pacer::Point &p, double scalar) { return p * scalar; },
          nb::arg("scalar"))
      // Enable reverse multiplication (float * Point)
      .def(
          "__rmul__",
          [](const pacer::Point &p, double scalar) { return p * scalar; },
          nb::arg("scalar"))
      .def("__repr__", [](const pacer::Point &point) {
        return nb::str("Point(x={}, y={})").format(point.x, point.y);
      });

  nb::class_<pacer::CoordinateSystem>(m, "CoordinateSystem")
      .def(nb::init<pacer::GPSSample>())
      .def("local", &pacer::CoordinateSystem::Local, nb::arg("point"))
      .def("global", &pacer::CoordinateSystem::Global, nb::arg("point"))
      .def("distance", &pacer::CoordinateSystem::Distance, nb::arg("from"),
           nb::arg("to"));
}
