#pragma once

#include <cstdlib>
#include <optional>

#include "datatypes.hpp"
#include "implot.h"

namespace pacer {

ImPlotPoint ToImPlotPoint(int index, void *data);

struct Point {
  double x, y;

  operator ImPlotPoint() { return {x, y}; }

  friend bool operator==(Point lhs, Point rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y;
  }

  friend bool operator!=(Point lhs, Point rhs) {
    return lhs.x != rhs.x || lhs.y != rhs.y;
  }

  friend Point operator*(const Point &p, double d) {
    return {p.x * d, p.y * d};
  }
  friend Point operator*(double d, const Point &p) {
    return {p.x * d, p.y * d};
  }

  friend Point operator+(const Point &x, const Point &y) {
    return {x.x + y.x, x.y + y.y};
  }
  friend Point operator-(const Point &x, const Point &y) {
    return {x.x - y.x, x.y - y.y};
  }

  friend Point operator*(const Point &p1, const Point &p2) {
    return {p1.x * p2.x - p1.y * p2.y, p1.x * p2.y + p1.y * p2.x};
  }

  double Scalar(const Point &other) const { return x * other.x + y * other.y; }

  Point Rot() const { return *this * Point{0, 1}; }
};

Point ToPoint(Point x);
Point ToPoint(GPSSample s);

template <class P = Point> struct PointInTime {
  P point;
  double time;
};

struct Segment {
  Point first, second;

  // Returns true if segments intersects, if ratio is non-null, it will satisfy:
  //   fst * (1 - ratio) + snd  lies  on present segment.
  bool Intersects(Point fst, Point snd, double *ratio) const;
};

Point Interpolate(Point from, Point to, double ratio);
GPSSample Interpolate(GPSSample from, GPSSample to, double ratio);

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