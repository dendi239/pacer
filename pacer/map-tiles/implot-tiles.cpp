#include "implot-tiles.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <implot.h>

#include "tile-math.hpp"

void pacer::PlotSatelliteTiles(TileStore &store, const CoordinateSystem &cs) {
  store.ApplyResults();

  ImPlotRect limits = ImPlot::GetPlotLimits();
  ImVec2 plot_px = ImPlot::GetPlotSize();
  if (plot_px.x <= 0.0f || plot_px.y <= 0.0f || limits.X.Size() <= 0.0)
    return;

  // Geographic bounding box of the visible plot rect. The local frame is
  // axis-aligned with north/east only approximately, so take min/max over
  // all four corners.
  double min_lat = 90.0, max_lat = -90.0, min_lon = 180.0, max_lon = -180.0;
  for (double x : {limits.X.Min, limits.X.Max}) {
    for (double y : {limits.Y.Min, limits.Y.Max}) {
      GPSSample corner = cs.Global(Vec3f{x, y, 0.0});
      min_lat = std::min(min_lat, corner.lat);
      max_lat = std::max(max_lat, corner.lat);
      min_lon = std::min(min_lon, corner.lon);
      max_lon = std::max(max_lon, corner.lon);
    }
  }
  if (std::isnan(min_lat) || std::isnan(min_lon) || min_lat > max_lat)
    return;

  // Tile zoom whose native resolution best matches the plot's meters/pixel.
  double center_lat = (min_lat + max_lat) / 2.0;
  double meters_per_px = limits.X.Size() / plot_px.x;
  double zf = std::log2(MetersPerTilePixel(center_lat, 0) / meters_per_px);
  int zoom = std::clamp(static_cast<int>(std::lround(zf)), kMinSatelliteZoom,
                        kMaxSatelliteZoom);

  auto [x_west, y_north] = LatLonToTileXY(max_lat, min_lon, zoom);
  auto [x_east, y_south] = LatLonToTileXY(min_lat, max_lon, zoom);
  int n = 1 << zoom;
  int min_tx = std::clamp(static_cast<int>(std::floor(x_west)), 0, n - 1);
  int max_tx = std::clamp(static_cast<int>(std::floor(x_east)), 0, n - 1);
  int min_ty = std::clamp(static_cast<int>(std::floor(y_north)), 0, n - 1);
  int max_ty = std::clamp(static_cast<int>(std::floor(y_south)), 0, n - 1);

  for (int ty = min_ty; ty <= max_ty; ++ty) {
    for (int tx = min_tx; tx <= max_tx; ++tx) {
      store.RequestTile(zoom, tx, ty);
      const MapTileImage *tile = store.Find(zoom, tx, ty);
      if (!tile || !tile->valid)
        continue;

      auto [nw_lat, nw_lon] = TileXYToLatLon(zoom, tx, ty);
      auto [se_lat, se_lon] = TileXYToLatLon(zoom, tx + 1, ty + 1);
      Vec3f south_west = cs.Local(GPSSample{.lat = se_lat, .lon = nw_lon});
      Vec3f north_east = cs.Local(GPSSample{.lat = nw_lat, .lon = se_lon});
      ImPlot::PlotImage("##satellite",
                        (ImTextureID)(intptr_t)tile->texture,
                        ImPlotPoint(south_west[0], south_west[1]),
                        ImPlotPoint(north_east[0], north_east[1]));
    }
  }
}
