#include "geometry.hpp"

#include <cmath>

#include <pacer/datatypes/datatypes.hpp>

ImPlotPoint pacer::ToImPlotPoint(int index, void *data) {
  GPSSample *data_ = reinterpret_cast<GPSSample *>(data);
  return ImPlotPoint(data_[index].lon, data_[index].lat);
}

bool pacer::Segment::Intersects(Point fst, Point snd, double *ratio) const {
  Point n = (snd - fst).Rot();
  if (n.Scalar(second - fst) * n.Scalar(first - fst) >= 0) {
    return false;
  }

  Point norm = (second - first).Rot();
  double d1 = norm.Scalar(snd - first), d2 = norm.Scalar(fst - first);
  if (d1 * d2 >= 0) {
    return false;
  }

  if (ratio != nullptr) {
    d1 = std::abs(d1);
    d2 = std::abs(d2);

    *ratio = d1 / (d1 + d2);
  }

  return true;
}

pacer::Point pacer::ToPoint(Point x) { return x; }

pacer::Point pacer::ToPoint(GPSSample s) { return {s.lon, s.lat}; }

pacer::Point pacer::Interpolate(Point from, Point to, double ratio) {
  return from * (1 - ratio) + to * ratio;
}

pacer::GPSSample pacer::Interpolate(GPSSample from, GPSSample to,
                                    double ratio) {
  return {
      .lat = from.lat * (1 - ratio) + from.lat * ratio,
      .lon = from.lon * (1 - ratio) + from.lon * ratio,
      .altitude = from.altitude * (1 - ratio) + from.altitude * ratio,
      .ground_speed =
          from.ground_speed * (1 - ratio) + from.ground_speed * ratio,
      .full_speed = from.full_speed * (1 - ratio) + from.full_speed * ratio,
  };
}

/*
Note: The Earth is almost, but not quite, a perfect sphere.
Its equatorial radius is 6378 km, but its polar radius is 6357 km - in other
words, the Earth is slightly flattened. 22 Oct 2020
*/

auto pacer::CoordinateSystem::Global(Vec3f point) const -> GPSSample {
  point = local_origin + dx * point[0] + dy * point[1] + dz * point[2];
  auto lon = 180 * atan2(point[1], point[0]) / M_PI;
  auto altitude =
      (std::sqrt((point / Vec3f{R_equator, R_equator, R_pole}).Norm()) - 1) *
      R_equator;

  auto lat =
      180 *
      atan2(point[2] / R_pole,
            std::sqrt(point[0] * point[0] + point[1] * point[1]) / R_equator) /
      M_PI;

  return GPSSample{
      .lat = lat,
      .lon = lon,
      .altitude = altitude,
      .full_speed = 0,
      .ground_speed = 0,
  };
}

auto pacer::CoordinateSystem::Local(GPSSample point) const -> Vec3f {
  auto p = CanonicalLocal(point);

  p -= local_origin;

  return Vec3f{
      Scalar(p, dx),
      Scalar(p, dy),
      Scalar(p, dz),
  };
}

pacer::Vec3f pacer::CoordinateSystem::CanonicalLocal(GPSSample origin) {
  return Vec3f{R_equator * std::cos(origin.lat * M_PI / 180.) *
                   std::cos(origin.lon * M_PI / 180.),
               R_equator * std::cos(origin.lat * M_PI / 180.) *
                   std::sin(origin.lon * M_PI / 180.),
               R_pole * std::sin(origin.lat * M_PI / 180.)} *
         (1 + origin.altitude / R_equator);
}

pacer::CoordinateSystem::CoordinateSystem(GPSSample origin)
    : origin(origin), local_origin(CanonicalLocal(origin)),
      dx(Vec3f{
          -R_equator * std::cos(origin.lat * M_PI / 180.) *
              std::sin(origin.lon * M_PI / 180.),
          R_equator * std::cos(origin.lat * M_PI / 180.) *
              std::cos(origin.lon * M_PI / 180.),
          0,
      }),
      dy(Vec3f{
          -R_equator * std::sin(origin.lat * M_PI / 180.) *
              std::cos(origin.lon * M_PI / 180.),
          -R_equator * std::sin(origin.lat * M_PI / 180.) *
              std::sin(origin.lon * M_PI / 180.),
          R_pole * std::cos(origin.lat * M_PI / 180.),
      }),
      dz(Vec3f{
          R_pole * std::cos(origin.lat * M_PI / 180.) *
              std::cos(origin.lon * M_PI / 180.),
          R_pole * std::cos(origin.lat * M_PI / 180.) *
              std::sin(origin.lon * M_PI / 180.),
          R_equator * std::sin(origin.lat * M_PI / 180.),
      }) {
  dx /= std::sqrt(dx.Norm());
  dy /= std::sqrt(dy.Norm());
  dz /= std::sqrt(dz.Norm());

  assert(std::abs(Scalar(dx, dy)) < 1e-6);
  assert(std::abs(Scalar(dx, dz)) < 1e-6);
  assert(std::abs(Scalar(dy, dz)) < 1e-6);
}
double pacer::CoordinateSystem::Distance(const GPSSample &from,
                                         const GPSSample &to) const {
  return std::sqrt((Local(from) - Local(to)).Norm());
}
bool pacer::Segment::operator==(const Segment &other) const {
  return (std::abs((first - other.first).x) < 1e-6) &&
         (std::abs((second - other.second).x) < 1e-6) &&
         (std::abs((first - other.first).y) < 1e-6) &&
         (std::abs((second - other.second).y) < 1e-6);
}
