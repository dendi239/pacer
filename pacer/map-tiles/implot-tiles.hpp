#pragma once

#include <pacer/geometry/geometry.hpp>
#include <pacer/map-tiles/tile-store.hpp>

namespace pacer {

/// Draws satellite tiles as the background of the current ImPlot plot whose
/// axes are local-frame meters of `cs` (the frame LapsDisplay plots in).
/// Picks the tile zoom that best matches the plot's current resolution,
/// requests the visible tiles, and uploads finished downloads (i.e. calls
/// store.ApplyResults()). Call between ImPlot::BeginPlot and the first
/// plotted item so the tiles stay underneath the traces.
void PlotSatelliteTiles(TileStore &store, const CoordinateSystem &cs);

} // namespace pacer
