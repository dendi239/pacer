#include "tile-math.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

std::pair<double, double> pacer::LatLonToTileXY(double latitude,
                                                double longitude, int zoom) {
  latitude = std::clamp(latitude, -85.05112878, 85.05112878);
  double lat_rad = latitude * M_PI / 180.0;
  double n = static_cast<double>(1 << zoom);
  double x = (longitude + 180.0) / 360.0 * n;
  double y =
      (1.0 - std::log(std::tan(lat_rad) + 1.0 / std::cos(lat_rad)) / M_PI) /
      2.0 * n;
  return {x, y};
}

std::pair<double, double> pacer::TileXYToLatLon(int zoom, double x, double y) {
  double n = static_cast<double>(1 << zoom);
  double lon = x / n * 360.0 - 180.0;
  double lat_rad = std::atan(std::sinh(M_PI * (1.0 - 2.0 * y / n)));
  double lat = lat_rad * 180.0 / M_PI;
  return {lat, lon};
}

std::string pacer::SatelliteTileUrl(int zoom, int x, int y) {
  char url_buffer[512];
  std::snprintf(url_buffer, sizeof(url_buffer),
                "https://server.arcgisonline.com/ArcGIS/rest/services/"
                "World_Imagery/MapServer/tile/%d/%d/%d",
                zoom, y, x);
  return std::string(url_buffer);
}

double pacer::MetersPerTilePixel(double latitude, int zoom) {
  constexpr double kEarthCircumference = 2.0 * M_PI * 6378137.0;
  double lat_rad = std::clamp(latitude, -85.05112878, 85.05112878) * M_PI / 180.0;
  return kEarthCircumference * std::cos(lat_rad) /
         (static_cast<double>(kTilePixels) * static_cast<double>(1 << zoom));
}
