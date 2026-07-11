#include "canvas-tiles.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pacer {

static ImVec2 CanvasCenter(const ImVec2 &canvas_min, const ImVec2 &canvas_max) {
  return ImVec2((canvas_min.x + canvas_max.x) * 0.5f,
                (canvas_min.y + canvas_max.y) * 0.5f);
}

static void WrapLongitude(TileCanvasView &view) {
  if (view.lon < -180.0)
    view.lon += 360.0;
  if (view.lon > 180.0)
    view.lon -= 360.0;
}

ImVec2 CanvasFromLatLon(double lat, double lon, const TileCanvasView &view,
                        const ImVec2 &canvas_min, const ImVec2 &canvas_max) {
  auto [center_x, center_y] = LatLonToTileXY(view.lat, view.lon, view.zoom);
  auto [point_x, point_y] = LatLonToTileXY(lat, lon, view.zoom);
  double tile_px = kTilePixels * static_cast<double>(view.scale);
  ImVec2 center = CanvasCenter(canvas_min, canvas_max);
  return ImVec2(center.x + (point_x - center_x) * tile_px,
                center.y + (point_y - center_y) * tile_px);
}

std::pair<double, double> LatLonFromCanvas(const ImVec2 &screen,
                                           const TileCanvasView &view,
                                           const ImVec2 &canvas_min,
                                           const ImVec2 &canvas_max) {
  auto [center_x, center_y] = LatLonToTileXY(view.lat, view.lon, view.zoom);
  double tile_px = kTilePixels * static_cast<double>(view.scale);
  ImVec2 center = CanvasCenter(canvas_min, canvas_max);
  double point_x = center_x + (screen.x - center.x) / tile_px;
  double point_y = center_y + (screen.y - center.y) / tile_px;
  return TileXYToLatLon(view.zoom, point_x, point_y);
}

void PanCanvas(TileCanvasView &view, const ImVec2 &delta) {
  auto [center_x, center_y] = LatLonToTileXY(view.lat, view.lon, view.zoom);
  double tile_px = kTilePixels * static_cast<double>(view.scale);
  auto [lat, lon] = TileXYToLatLon(view.zoom, center_x - delta.x / tile_px,
                                   center_y - delta.y / tile_px);
  view.lat = lat;
  view.lon = lon;
  WrapLongitude(view);
}

// Applies a scale factor while keeping tiles near native resolution: the
// continuous zoom (zoom + log2(scale)) is redistributed so `scale` stays
// close to 1 and whole steps land in `zoom` instead.
static void ZoomBy(TileCanvasView &view, float factor) {
  double continuous =
      view.zoom + std::log2(static_cast<double>(view.scale) * factor);
  continuous =
      std::clamp(continuous, static_cast<double>(kMinSatelliteZoom),
                 static_cast<double>(kMaxSatelliteZoom) + 2.0);
  int zoom = std::clamp(static_cast<int>(std::lround(continuous)),
                        kMinSatelliteZoom, kMaxSatelliteZoom);
  view.zoom = zoom;
  view.scale = static_cast<float>(std::exp2(continuous - zoom));
}

void ZoomCanvasAt(TileCanvasView &view, float factor, const ImVec2 &pivot,
                  const ImVec2 &canvas_min, const ImVec2 &canvas_max) {
  auto [pivot_lat, pivot_lon] =
      LatLonFromCanvas(pivot, view, canvas_min, canvas_max);
  ZoomBy(view, factor);

  // Re-center so the geographic point under the cursor stays put.
  auto [pivot_x, pivot_y] = LatLonToTileXY(pivot_lat, pivot_lon, view.zoom);
  double tile_px = kTilePixels * static_cast<double>(view.scale);
  ImVec2 center = CanvasCenter(canvas_min, canvas_max);
  auto [lat, lon] = TileXYToLatLon(view.zoom, pivot_x - (pivot.x - center.x) / tile_px,
                                   pivot_y - (pivot.y - center.y) / tile_px);
  view.lat = lat;
  view.lon = lon;
  WrapLongitude(view);
}

struct TileRange {
  int min_tx, max_tx, min_ty, max_ty;
  int center_tx, center_ty;
  double frac_x, frac_y;
};

static TileRange VisibleTileRange(const TileCanvasView &view,
                                  const ImVec2 &canvas_min,
                                  const ImVec2 &canvas_max, int margin) {
  auto [tile_xf, tile_yf] = LatLonToTileXY(view.lat, view.lon, view.zoom);
  int center_tx = static_cast<int>(std::floor(tile_xf));
  int center_ty = static_cast<int>(std::floor(tile_yf));
  float tile_px = kTilePixels * view.scale;
  ImVec2 canvas_size =
      ImVec2(canvas_max.x - canvas_min.x, canvas_max.y - canvas_min.y);
  int extra_x =
      static_cast<int>(std::ceil((canvas_size.x * 0.5f) / tile_px)) + margin;
  int extra_y =
      static_cast<int>(std::ceil((canvas_size.y * 0.5f) / tile_px)) + margin;
  int n = 1 << view.zoom;
  return TileRange{
      .min_tx = std::max(0, center_tx - extra_x),
      .max_tx = std::min(n - 1, center_tx + extra_x),
      .min_ty = std::max(0, center_ty - extra_y),
      .max_ty = std::min(n - 1, center_ty + extra_y),
      .center_tx = center_tx,
      .center_ty = center_ty,
      .frac_x = tile_xf - center_tx,
      .frac_y = tile_yf - center_ty,
  };
}

void RequestVisibleCanvasTiles(TileStore &store, const TileCanvasView &view,
                               const ImVec2 &canvas_min,
                               const ImVec2 &canvas_max) {
  TileRange range = VisibleTileRange(view, canvas_min, canvas_max, 1);
  for (int ty = range.min_ty; ty <= range.max_ty; ++ty) {
    for (int tx = range.min_tx; tx <= range.max_tx; ++tx) {
      store.RequestTile(view.zoom, tx, ty);
    }
  }
}

void RenderCanvasTiles(const TileStore &store, const TileCanvasView &view,
                       ImDrawList *draw_list, const ImVec2 &canvas_min,
                       const ImVec2 &canvas_max) {
  TileRange range = VisibleTileRange(view, canvas_min, canvas_max, 2);
  float tile_px = kTilePixels * view.scale;
  ImVec2 center = CanvasCenter(canvas_min, canvas_max);
  ImVec2 origin = ImVec2(center.x - range.frac_x * tile_px,
                         center.y - range.frac_y * tile_px);

  for (int ty = range.min_ty; ty <= range.max_ty; ++ty) {
    for (int tx = range.min_tx; tx <= range.max_tx; ++tx) {
      ImVec2 tile_min = ImVec2(origin.x + (tx - range.center_tx) * tile_px,
                               origin.y + (ty - range.center_ty) * tile_px);
      ImVec2 tile_max = ImVec2(tile_min.x + tile_px, tile_min.y + tile_px);
      const MapTileImage *tile = store.Find(view.zoom, tx, ty);

      if (tile && tile->valid) {
        draw_list->AddImage((ImTextureID)(intptr_t)tile->texture, tile_min,
                            tile_max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
      } else {
        draw_list->AddRectFilled(tile_min, tile_max, IM_COL32(18, 22, 35, 255));
        draw_list->AddRect(tile_min, tile_max, IM_COL32(80, 92, 120, 150), 0.0f,
                           0, 1.0f);
        if (tile) {
          draw_list->AddText(ImVec2(tile_min.x + 4.0f, tile_min.y + 4.0f),
                             IM_COL32(200, 200, 220, 180),
                             tile->status.c_str());
        }
      }
    }
  }
}

} // namespace pacer
