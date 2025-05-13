#include "implot.h"

#include <pacer/laps/laps.hpp>

namespace pacer {

struct LapsDisplay {
  Laps &laps;
  int selected_lap = -1;

  CoordinateSystem cs;

  ImPlotPoint ToImPlotPoint(GPSSample s) const;

  std::pair<Point, Point> bounds = {{1, 1}, {0, 0}};

  void DragTimingLine(Segment *s, const char *name, int drag_id);

  void DisplayMap();

  void DisplayLapTelemetry() const;

  bool DisplayTable();
};

} // namespace pacer
