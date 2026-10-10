#include "geometry.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

#include <pacer/datatypes/datatypes.hpp>

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

    *ratio = d2 / (d1 + d2);
  }

  return true;
}

pacer::Point pacer::ToPoint(Vec3f v) { return {v.x, v.y}; }

pacer::Point pacer::ToPoint(Point x) { return x; }

pacer::Point pacer::ToPoint(GPSSample s) { return {s.lon, s.lat}; }

pacer::Point pacer::Interpolate(Point from, Point to, double ratio) {
  return from * (1 - ratio) + to * ratio;
}

pacer::GPSSample pacer::Interpolate(GPSSample from, GPSSample to,
                                    double ratio) {
  assert(ratio != ratio || (ratio >= 0 && ratio <= 1));
  return {
      .lat = from.lat * (1 - ratio) + to.lat * ratio,
      .lon = from.lon * (1 - ratio) + to.lon * ratio,
      .altitude = from.altitude * (1 - ratio) + to.altitude * ratio,
      .full_speed = from.full_speed * (1 - ratio) + to.full_speed * ratio,
      .ground_speed = from.ground_speed * (1 - ratio) + to.ground_speed * ratio,
      .vel_n = from.vel_n * (1 - ratio) + to.vel_n * ratio,
      .vel_e = from.vel_e * (1 - ratio) + to.vel_e * ratio,
      .vel_d = from.vel_d * (1 - ratio) + to.vel_d * ratio,
      // Accuracy blends toward the worse of the two: an interpolated point
      // is no better known than the endpoint it is least sure of.
      .h_acc = std::max(from.h_acc, to.h_acc),
      .s_acc = std::max(from.s_acc, to.s_acc),
      .timestamp_ms = from.timestamp_ms +
                      static_cast<int64_t>(std::llround(
                          (to.timestamp_ms - from.timestamp_ms) * ratio)),
  };
}

/*
Note: The Earth is almost, but not quite, a perfect sphere.
Its equatorial radius is 6378 km, but its polar radius is 6357 km - in other
words, the Earth is slightly flattened. 22 Oct 2020
*/

auto pacer::CoordinateSystem::Global(Vec3f point) const -> GPSSample {
  point = local_origin + dx * point[0] + dy * point[1] + dz * point[2];
  // Inverse of CanonicalLocal (WGS84 ECEF -> geodetic). Latitude has no
  // closed form, but the fixed-point iteration converges to sub-millimetre
  // precision in a handful of steps away from the poles.
  double p = std::sqrt(point[0] * point[0] + point[1] * point[1]);
  double lon = std::atan2(point[1], point[0]);
  double lat = std::atan2(point[2], p * (1 - E_squared));
  double altitude = 0;
  for (int i = 0; i < 5; ++i) {
    double N = R_semi_major /
               std::sqrt(1 - E_squared * std::sin(lat) * std::sin(lat));
    altitude = p / std::cos(lat) - N;
    lat = std::atan2(point[2], p * (1 - E_squared * N / (N + altitude)));
  }

  return GPSSample{
      .lat = lat * 180 / M_PI,
      .lon = lon * 180 / M_PI,
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
  double N = R_semi_major /
             std::sqrt(1 - E_squared * std::sin(origin.lat * M_PI / 180.) *
                               std::sin(origin.lat * M_PI / 180.));
  return Vec3f{
      (N + origin.altitude) * std::cos(origin.lat * M_PI / 180.) *
          std::cos(origin.lon * M_PI / 180.),
      (N + origin.altitude) * std::cos(origin.lat * M_PI / 180.) *
          std::sin(origin.lon * M_PI / 180.),
      (N * (1 - E_squared) + origin.altitude) *
          std::sin(origin.lat * M_PI / 180.),
  };
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
