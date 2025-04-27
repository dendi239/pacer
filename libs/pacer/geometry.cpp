#include "geometry.hpp"

#include "datatypes.hpp"

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

pacer::Point pacer::ToPoint(GPSSample s) { return {.x = s.lon, .y = s.lat}; }

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
