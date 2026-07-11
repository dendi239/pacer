#pragma once

#include <string>
#include <utility>

namespace pacer {

/// ArcGIS World_Imagery serves tiles up to this zoom level.
inline constexpr int kMaxSatelliteZoom = 19;
inline constexpr int kMinSatelliteZoom = 3;
inline constexpr int kTilePixels = 256;

/// Web-Mercator: fractional slippy-map tile coordinates for a lat/lon.
std::pair<double, double> LatLonToTileXY(double latitude, double longitude,
                                         int zoom);

/// Inverse of LatLonToTileXY; accepts fractional tile coordinates.
std::pair<double, double> TileXYToLatLon(int zoom, double x, double y);

std::string SatelliteTileUrl(int zoom, int x, int y);

/// Ground meters covered by one pixel of a kTilePixels-wide tile at the
/// given latitude.
double MetersPerTilePixel(double latitude, int zoom);

} // namespace pacer
