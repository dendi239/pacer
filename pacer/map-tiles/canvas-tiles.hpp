#pragma once

#include <utility>

#include <imgui.h>

#include <pacer/map-tiles/tile-math.hpp>
#include <pacer/map-tiles/tile-store.hpp>

namespace pacer {

// Camera over an ImDrawList slippy map: a geographic center plus an integer
// tile zoom level and a continuous scale (screen pixels per tile pixel)
// around it. The center always sits in the middle of the canvas; panning and
// zooming move lat/lon rather than accumulating a screen offset, so the
// visible tile range is always computable from the view alone.
struct TileCanvasView {
  double lat = 0.0;
  double lon = 0.0;
  int zoom = kMaxSatelliteZoom;
  float scale = 1.0f;
};

ImVec2 CanvasFromLatLon(double lat, double lon, const TileCanvasView &view,
                        const ImVec2 &canvas_min, const ImVec2 &canvas_max);

std::pair<double, double> LatLonFromCanvas(const ImVec2 &screen,
                                           const TileCanvasView &view,
                                           const ImVec2 &canvas_min,
                                           const ImVec2 &canvas_max);

/// Shifts the view center by a screen-space delta (drag panning): dragging
/// right moves the map content right.
void PanCanvas(TileCanvasView &view, const ImVec2 &delta);

/// Multiplies the view scale by `factor`, keeping the point under `pivot`
/// fixed on screen. Rebases the tile zoom level so tiles are always fetched
/// near native resolution; total zoom is clamped to
/// [kMinSatelliteZoom, kMaxSatelliteZoom + 2] (the +2 allows magnifying max-
/// resolution tiles for fine editing).
void ZoomCanvasAt(TileCanvasView &view, float factor, const ImVec2 &pivot,
                  const ImVec2 &canvas_min, const ImVec2 &canvas_max);

/// Requests every tile visible in the canvas from `store`.
void RequestVisibleCanvasTiles(TileStore &store, const TileCanvasView &view,
                               const ImVec2 &canvas_min,
                               const ImVec2 &canvas_max);

/// Draws the visible tiles; tiles still loading or failed render as
/// placeholder boxes with their status text.
void RenderCanvasTiles(const TileStore &store, const TileCanvasView &view,
                       ImDrawList *draw_list, const ImVec2 &canvas_min,
                       const ImVec2 &canvas_max);

} // namespace pacer
