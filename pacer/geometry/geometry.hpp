#pragma once

#include <cstdlib>
#include <optional>
#include <utility>

#include "implot.h"

#include <pacer/datatypes/datatypes.hpp>
#include <pacer/datatypes/ops.hpp>

namespace pacer {

ImPlotPoint ToImPlotPoint(int index, void *data);

struct Point : VectorOperators<Point, double, 2> {
  double x = 0, y = 0;

  Point() = default;
  Point(double x, double y) : x(x), y(y) {}

  double operator[](size_t index) const { return index ? y : x; }
  double &operator[](size_t index) { return index ? y : x; }

  operator ImPlotPoint() { return {(*this)[0], (*this)[1]}; }

  Point Rot() const { return Point{-(*this)[1], (*this)[0]}; }

  friend std::ostream &operator<<(std::ostream &os, const Point &p) {
    return os << "(" << p.x << ", " << p.y << ")";
  }
};

Point ToPoint(Point x);
Point ToPoint(GPSSample s);
Point ToPoint(Vec3f v);

// template <typename Concrete, typename T, size_t N>
// Point ToPoint(const LinearOperators<Concrete, T, N> &x) {
//   return Point{static_cast<const Concrete &>(x)[0],
//                static_cast<const Concrete &>(x)[1]};
// }

struct Segment {
  Point first, second;

  // Returns true if segments intersects, if ratio is non-null, it will satisfy:
  //   fst * (1 - ratio) + snd  lies  on present segment.
  bool Intersects(Point fst, Point snd, double *ratio) const;

  bool operator==(const Segment &other) const;
};

struct CoordinateSystem {
  // Coordinate system maps GPS coordinates to local coordinates.
  //  N.B. All local coordinates measured in meters.
  //
  // I employ following formulas:
  //   x = h_c * R_equator * cos(lat) * cos(lon)
  //   y = h_c * R_equator * cos(lat) * sin(lon)
  //   z = h_c * R_pole * sin(lat)
  //
  // Where h_c is the height compenstaion factor:
  //   h_c = 1 + altitude / R_equator
  //
  // Basis for resulting coordinate system is almost normalised gradients along
  // lon/lat/altiude coordinates: only altitude slightly differs to have
  // ortogonal system:
  //  dx = (-R_equator cos(lat) sin(lon), R_equator cos(lat) cos(lon), 0)
  //  dy = (-R_equator sin(lat) cos(lon), -R_equator sin(lat) sin(lon),
  //        R_pole cos(lat))
  //  dz = (R_pole cos(lat) cos(lon), R_pole cos(lat) sin(lon),
  //        R_equator sin(lat))
  //
  // This is most likely not the best way to do this, but it works for now.

  CoordinateSystem() = default;
  explicit CoordinateSystem(GPSSample origin);

  /// Converts point to local coordinate system.
  auto Local(GPSSample point) const -> Vec3f;

  /// Maps local-coordinate point back to gps sample.
  /// N.B. Speed is not preserved.
  auto Global(Vec3f point) const -> GPSSample;

  double Distance(const GPSSample &from, const GPSSample &to) const;

private:
  constexpr static double R_equator = 6'378'000;
  constexpr static double R_pole = 6'357'000;
  static Vec3f CanonicalLocal(GPSSample point);

  GPSSample origin;
  Vec3f local_origin, dx, dy, dz;
};

Point Interpolate(Point from, Point to, double ratio);
GPSSample Interpolate(GPSSample from, GPSSample to, double ratio);

// template <typename Concrete, typename T, size_t N>
// Concrete Interpolate(const LinearOperators<Concrete, T, N> &from,
//                      const LinearOperators<Concrete, T, N> &to, double ratio)
//                      {
//   return static_cast<Concrete>(static_cast<const Concrete &>(from) *
//                                    (1 - ratio) +
//                                static_cast<const Concrete &>(to) * ratio);
// }

template <class P>
std::optional<PointInTime<P>> Split(Segment start_line, PointInTime<P> first,
                                    PointInTime<P> second) {

  double ratio = 0.;
  if (!start_line.Intersects(ToPoint(first.point), ToPoint(second.point),
                             &ratio)) {
    return std::nullopt;
  }

  return PointInTime{
      .point = Interpolate(first.point, second.point, ratio),
      .time = first.time * (1 - ratio) + ratio * second.time,
  };
}
} // namespace pacer