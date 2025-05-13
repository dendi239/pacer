#pragma once

#include <array>
#include <iomanip>
#include <ostream>
#include <type_traits>

#include "ops.hpp"

namespace pacer {

struct GPSSample {
  double lat, lon, altitude, full_speed, ground_speed;
};

inline std::ostream &operator<<(std::ostream &os, const GPSSample &s) {
  return os << "GPS(lat: " << std::setprecision(4) << std::fixed << s.lat
            << ", lon: " << s.lon << ", alt: " << s.altitude
            << ", full: " << s.full_speed << ", ground: " << s.ground_speed
            << ")";
}

namespace impl {

template <typename T, size_t N, typename... Args> struct is_array_constructible;

template <typename T, size_t N, typename Head, typename... Tail>
struct is_array_constructible<T, N, Head, Tail...> {
  static constexpr const bool value =
      std::is_constructible_v<T, Head> &&
      is_array_constructible<T, N - 1, Tail...>::value;
};

template <typename T, size_t N> struct is_array_constructible<T, N> {
  static constexpr const bool value = N == 0;
};
template <typename T, size_t N, typename... Args>
constexpr bool is_array_constructible_v =
    is_array_constructible<T, N, Args...>::value;

} // namespace impl

template <typename T, size_t N>
struct Vector : public VectorOperators<Vector<T, N>, T, N> {
  std::array<T, N> data;

  Vector() = default;
  template <typename... Args>
    requires std::is_constructible_v<std::array<T, N>, Args...> ||
             impl::is_array_constructible_v<T, N, Args...>
  Vector(Args... args) : data{static_cast<T>(args)...} {}

  T &operator[](size_t index) { return data[index]; }
  T operator[](size_t index) const { return data[index]; }

  friend std::ostream &operator<<(std::ostream &os, const Vector<T, N> &v) {
    os << "(";
    for (size_t i = 0; i < N; ++i) {
      os << v[i];
      if (i != N - 1)
        os << ", ";
    }
    return os << ")";
  }
};

using Vec2f = Vector<double, 2>;
using Vec3f = Vector<double, 3>;

} // namespace pacer
