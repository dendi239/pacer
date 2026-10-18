#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "lvgl.h"

#include <pacer/live-timing/live-timing.hpp>

// The lap column: N rows of recent laps over the session best.
//
//   L21  48.640   <- rows 1..N-1: older laps by number, oldest on top,
//   L22  49.104      dimmed
//   LAST 48.912   <- row N: the lap just finished
//   BEST 48.299   <- row N+1, a few px further down: the lap the delta
//                    reads against — "REF" while that is a recorded lap
//                    the session hasn't beaten yet
//
// Tags sit at the left edge and times are right-aligned, so the digits line
// up across rows (and past 100 s). Rows with no lap yet read blank / "--.---".
// LVGL objects: create and update under the LVGL lock.
class LapTable {
public:
  /// Most rows of recent laps the snapshot can fill.
  static constexpr size_t kMaxRecent =
      std::tuple_size_v<decltype(pacer::LiveSnapshot::recent_laps)>;

  /// Narrowest width that fits "L123 " plus a 100 s+ time ("102.345"):
  /// 12 characters of the 24 px face, 14.4 px each.
  static constexpr int32_t kFitWidth = 12 * 15;

  /// Builds the widget as a transparent `width`-wide container on `parent`;
  /// position it through obj(). `recent_rows` is N above, clamped to
  /// 1..kMaxRecent.
  LapTable(lv_obj_t *parent, size_t recent_rows, int32_t width);

  lv_obj_t *obj() const { return container_; }
  int32_t height() const { return height_; }

  void Update(const pacer::LiveSnapshot &snap);

private:
  struct Row {
    lv_obj_t *tag = nullptr;
    lv_obj_t *time = nullptr;
  };

  Row MakeRow(int32_t y, bool dim);

  lv_obj_t *container_ = nullptr;
  int32_t width_ = 0;
  int32_t height_ = 0;
  /// Top to bottom: recent_[0] is the oldest lap, recent_.back() is LAST.
  std::vector<Row> recent_;
  Row best_;
};
