#include "lap_table.hpp"

#include <algorithm>
#include <cstdio>

#include "lap_time_format.hpp"
#include "pacer_fonts.h"

namespace {

// Every row is in the 24 px face. Rows are pitched under its 27 px line
// height: the tags and digits have no descenders, so the cut only eats
// empty space below the baseline.
const lv_font_t &kFont = pacer_font_mono_24;
constexpr int32_t kRowH = 24;
constexpr int32_t kBestGap = 4;

// Tags and times for LAST/BEST, and the dimmed pair for older laps.
const lv_color_t kTagColor = lv_color_hex(0x808080);
const lv_color_t kTimeColor = lv_color_hex(0xFFFFFF);
const lv_color_t kDimTagColor = lv_color_hex(0x585858);
const lv_color_t kDimTimeColor = lv_color_hex(0xA0A0A0);

} // namespace

LapTable::LapTable(lv_obj_t *parent, size_t recent_rows, int32_t width)
    : width_(width) {
  recent_rows = std::clamp<size_t>(recent_rows, 1, kMaxRecent);
  height_ = static_cast<int32_t>(recent_rows + 1) * kRowH + kBestGap;

  container_ = lv_obj_create(parent);
  lv_obj_remove_style_all(container_);
  lv_obj_set_size(container_, width, height_);
  // Not clickable, so taps and long presses fall through to the screen.
  lv_obj_remove_flag(container_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(container_, LV_OBJ_FLAG_CLICKABLE);

  for (size_t i = 0; i < recent_rows; ++i) {
    bool last = i + 1 == recent_rows;
    recent_.push_back(MakeRow(static_cast<int32_t>(i) * kRowH, !last));
  }
  lv_label_set_text(recent_.back().tag, "LAST");

  best_ = MakeRow(height_ - kRowH, false);
  lv_label_set_text(best_.tag, "BEST");
}

LapTable::Row LapTable::MakeRow(int32_t y, bool dim) {
  Row row;
  row.tag = lv_label_create(container_);
  lv_obj_set_style_text_font(row.tag, &kFont, 0);
  lv_obj_set_style_text_color(row.tag, dim ? kDimTagColor : kTagColor, 0);
  lv_obj_set_pos(row.tag, 0, y);
  lv_label_set_text(row.tag, "");

  row.time = lv_label_create(container_);
  lv_obj_set_style_text_font(row.time, &kFont, 0);
  lv_obj_set_style_text_color(row.time, dim ? kDimTimeColor : kTimeColor, 0);
  lv_obj_set_width(row.time, width_);
  lv_obj_set_style_text_align(row.time, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(row.time, 0, y);
  lv_label_set_text(row.time, "--.---");
  return row;
}

void LapTable::Update(const pacer::LiveSnapshot &snap) {
  char buf[16];
  size_t n = recent_.size();
  for (size_t i = 0; i < n; ++i) {
    // Bottom row is the newest lap, recent_laps[0].
    const pacer::CompletedLap &lap = snap.recent_laps[n - 1 - i];
    bool have = !std::isnan(lap.time_s);
    if (i + 1 < n) {
      if (have) {
        snprintf(buf, sizeof(buf), "L%d", lap.number);
        lv_label_set_text(recent_[i].tag, buf);
      } else {
        lv_label_set_text(recent_[i].tag, "");
      }
    }
    FormatLapTime(buf, sizeof(buf), lap.time_s);
    lv_label_set_text(recent_[i].time, buf);
  }

  // The bottom row is the lap the delta reads against. That is the session
  // best unless a recorded reference lap is still faster — then it is that
  // lap, tagged REF so it isn't taken for one driven today.
  bool recorded = !std::isnan(snap.reference_lap_s) &&
                  (std::isnan(snap.best_lap_s) ||
                   snap.reference_lap_s < snap.best_lap_s);
  lv_label_set_text(best_.tag, recorded ? "REF" : "BEST");
  FormatLapTime(buf, sizeof(buf),
                recorded ? snap.reference_lap_s : snap.best_lap_s);
  lv_label_set_text(best_.time, buf);
}
