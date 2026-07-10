#pragma once

#include <iomanip>
#include <ostream>

#include "ops.hpp"

namespace pacer {

struct GPSSample {
  double lat, lon, altitude = 0, full_speed = 0, ground_speed = 0;
  int64_t timestamp_ms = 0;
};

inline std::ostream &operator<<(std::ostream &os, const GPSSample &s) {
  return os << "GPS(t: " << s.timestamp_ms << ", lat: " << std::setprecision(4)
            << std::fixed << s.lat << ", lon: " << s.lon
            << ", alt: " << s.altitude << ", full: " << s.full_speed
            << ", ground: " << s.ground_speed << ")";
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
