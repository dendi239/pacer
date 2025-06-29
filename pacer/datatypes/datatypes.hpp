#pragma once

#include <iomanip>
#include <ostream>

#include "ops.hpp"

namespace pacer {

struct GPSSample {
  double lat, lon, altitude, full_speed, ground_speed;
};

template <class P> struct PointInTime {
  P point;
  double time;

  template <class F, class U> PointInTime<U> Map(F f) const {
    return PointInTime<U>{.point = f(point), .time = time};
  }
};

inline std::ostream &operator<<(std::ostream &os, const GPSSample &s) {
  return os << "GPS(lat: " << std::setprecision(4) << std::fixed << s.lat
            << ", lon: " << s.lon << ", alt: " << s.altitude
            << ", full: " << s.full_speed << ", ground: " << s.ground_speed
            << ")";
}

struct Vec3f : public VectorOperators<Vec3f, double, 3> {
  double x = 0, y = 0, z = 0;

  Vec3f() = default;
  Vec3f(double x, double y, double z) : x{x}, y{y}, z{z} {}

  double &operator[](size_t index) {
    return (index == 0) ? x : (index == 1) ? y : z;
  }
  double operator[](size_t index) const {
    return (index == 0) ? x : (index == 1) ? y : z;
  }
};

} // namespace pacer
