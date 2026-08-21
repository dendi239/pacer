#pragma once

#include <iomanip>
#include <ostream>

#include "ops.hpp"

namespace pacer {

struct GPSSample {
  double lat, lon, altitude = 0, full_speed = 0, ground_speed = 0;

  /// Velocity in the local NED frame, m/s. The receiver derives this from
  /// carrier Doppler rather than from differencing positions, so it is about
  /// an order of magnitude less noisy than (pos[i] - pos[i-1]) / dt: metre-
  /// level position noise spread over a 40 ms epoch is metres per second of
  /// apparent velocity error, while the Doppler estimate sits near 0.05 m/s.
  /// Prefer it wherever a heading or a sub-epoch position is wanted.
  ///
  /// Zero when the source doesn't carry it — GPMF gives speed but no vector,
  /// so check the source before treating {0,0,0} as "stationary".
  double vel_n = 0, vel_e = 0, vel_d = 0;

  /// The receiver's own 1-sigma estimates: horizontal position in m, speed
  /// in m/s. Zero means "not reported", not "perfect".
  double h_acc = 0, s_acc = 0;

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
