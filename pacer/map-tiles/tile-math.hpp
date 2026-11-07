#pragma once

#include <string>
#include <utility>

namespace pacer {

/// Deepest tile zoom fetched from any provider. ArcGIS World_Imagery stops
/// here; Google goes deeper in places, but 19 is already ~0.2 m/px.
inline constexpr int kMaxSatelliteZoom = 19;
inline constexpr int kMinSatelliteZoom = 3;
inline constexpr int kTilePixels = 256;

/// Web-Mercator: fractional slippy-map tile coordinates for a lat/lon.
std::pair<double, double> LatLonToTileXY(double latitude, double longitude,
                                         int zoom);

/// Inverse of LatLonToTileXY; accepts fractional tile coordinates.
std::pair<double, double> TileXYToLatLon(int zoom, double x, double y);

/// ArcGIS World_Imagery tile.
std::string SatelliteTileUrl(int zoom, int x, int y);

/// Google Map Tiles API: POST a GoogleSatelliteSessionJson() body here to
/// get a session token, which every tile request then carries.
std::string GoogleCreateSessionUrl(const std::string &api_key);
std::string GoogleSatelliteSessionJson();

/// Google Map Tiles API 2D tile for a session from GoogleCreateSessionUrl.
std::string GoogleTileUrl(int zoom, int x, int y, const std::string &session,
                          const std::string &api_key);

/// Ground meters covered by one pixel of a kTilePixels-wide tile at the
/// given latitude.
double MetersPerTilePixel(double latitude, int zoom);

} // namespace pacer
