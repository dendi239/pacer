#include "dashboard_ui.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <vector>

#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "esp_lcd_nv3041a.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "sdkconfig.h"

#if CONFIG_PACER_TOUCH_ENABLED
#include "driver/i2c_master.h"
#include "esp_lcd_touch_gt911.h"
#endif

namespace {

const char *TAG = "dashboard_ui";

constexpr int kWidth = CONFIG_PACER_LCD_H_RES;
constexpr int kHeight = CONFIG_PACER_LCD_V_RES;

// Disabled bool Kconfig options generate no macro at all, so bring them
// into constexpr land before using them as values.
#ifdef CONFIG_PACER_LCD_MIRROR_X
constexpr bool kMirrorX = true;
#else
constexpr bool kMirrorX = true;
#endif
#ifdef CONFIG_PACER_LCD_MIRROR_Y
constexpr bool kMirrorY = true;
#else
constexpr bool kMirrorY = true;
#endif

lv_display_t *s_disp = nullptr;
lv_obj_t *s_lap_label = nullptr;
lv_obj_t *s_clock_label = nullptr;
lv_obj_t *s_delta_label = nullptr;
lv_obj_t *s_current_label = nullptr;
lv_obj_t *s_last_label = nullptr;
lv_obj_t *s_best_label = nullptr;
lv_obj_t *s_status_label = nullptr;
lv_obj_t *s_debug_label = nullptr;

// Debug menu (opened by a long press anywhere on the screen).
lv_obj_t *s_menu = nullptr;
lv_obj_t *s_nextline_page = nullptr;
lv_obj_t *s_nextline_label = nullptr;
lv_obj_t *s_offset_page = nullptr;
lv_obj_t *s_offset_label = nullptr;
lv_obj_t *s_offset_detail_label = nullptr;
lv_obj_t *s_logging_page = nullptr;
lv_obj_t *s_logstats_label = nullptr;
lv_obj_t *s_logtoggle_label = nullptr;

// Track map page: outline polylines + position marker, all in screen pixels
// recomputed from the meter-space data below on every position update.
constexpr int32_t kMapMarkerSize = 12;
lv_obj_t *s_map_page = nullptr;
lv_obj_t *s_map_left_line = nullptr;
lv_obj_t *s_map_right_line = nullptr;
lv_obj_t *s_map_start_line = nullptr;
lv_obj_t *s_map_marker = nullptr;
lv_obj_t *s_map_empty_label = nullptr;

// Gate endpoints in the caller's metric frame (see
// dashboard_ui_set_track_map), and their screen-pixel projections. The px
// vectors are persistent because lv_line only keeps a pointer to them. All
// of these are touched exclusively under the LVGL lock.
std::vector<pacer::Point> s_map_edge_first, s_map_edge_second;
std::vector<lv_point_precise_t> s_map_left_px, s_map_right_px;
lv_point_precise_t s_map_start_px[2];
bool s_map_have_pos = false;
pacer::Point s_map_pos{};

// Sector color of each infill quad (quad i spans gates i..i+1, quad n-1 is
// the wraparound back to gate 0), assigned like track_annotator: the quad
// reaching a sector-split gate keeps the old color, the next one advances.
std::vector<uint8_t> s_map_quad_sector;

// track_annotator's kSectorColors pre-blended at their alpha (160/255) onto
// its dark map background, so the opaque LVGL triangles look the same
// without any per-pixel blending.
const lv_color_t kMapSectorColors[] = {
    lv_color_hex(0x7892AD), lv_color_hex(0xA78665), lv_color_hex(0x78A87E),
    lv_color_hex(0xA77997), lv_color_hex(0x919265), lv_color_hex(0x8479AD),
};
constexpr size_t kMapSectorColorCount =
    sizeof(kMapSectorColors) / sizeof(kMapSectorColors[0]);
std::atomic<bool> s_reload_request{false};
std::atomic<bool> s_logging_enabled{true};

void FormatLapTime(char *buf, size_t n, double seconds) {
  if (std::isnan(seconds)) {
    snprintf(buf, n, "--:--.---");
    return;
  }
  int mins = (int)(seconds / 60);
  snprintf(buf, n, "%d:%06.3f", mins, seconds - mins * 60);
}

void FormatCountdown(char *buf, size_t n, double seconds) {
  if (std::isnan(seconds)) {
    snprintf(buf, n, "--:--");
    return;
  }
  int total = (int)std::fabs(seconds);
  snprintf(buf, n, "%s%d:%02d", seconds < 0 ? "-" : "", total / 60, total % 60);
}

//----------------------------- debug menu ---------------------------------//
// Long press anywhere -> menu of debug pages/actions. Callbacks run inside
// the LVGL task, so they touch widgets directly (no lvgl_port_lock needed).

void ShowMenu(bool show) {
  if (show) {
    lv_obj_remove_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
  }
}

void ShowNextLinePage(bool show) {
  if (show) {
    lv_obj_remove_flag(s_nextline_page, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_nextline_page, LV_OBJ_FLAG_HIDDEN);
  }
}

void ShowOffsetPage(bool show) {
  if (show) {
    lv_obj_remove_flag(s_offset_page, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_offset_page, LV_OBJ_FLAG_HIDDEN);
  }
}

void ShowMapPage(bool show) {
  if (show) {
    lv_obj_remove_flag(s_map_page, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_map_page, LV_OBJ_FLAG_HIDDEN);
  }
}

// Projects the meter-space outline (plus the current position, if any) into
// screen pixels: bounding box of everything, a margin, uniform scale to fit,
// centered, north up (screen y grows down, so y is flipped). Caller holds
// the LVGL lock.
void RedrawMap() {
  size_t n = s_map_edge_first.size();
  if (n < 2) {
    lv_obj_remove_flag(s_map_empty_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_map_left_line, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_map_right_line, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_map_start_line, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_map_marker, LV_OBJ_FLAG_HIDDEN);
    return;
  }

  double min_x = s_map_pos.x, max_x = s_map_pos.x;
  double min_y = s_map_pos.y, max_y = s_map_pos.y;
  bool first = !s_map_have_pos;
  for (const auto &edge : {&s_map_edge_first, &s_map_edge_second}) {
    for (const pacer::Point &p : *edge) {
      if (first) {
        min_x = max_x = p.x;
        min_y = max_y = p.y;
        first = false;
        continue;
      }
      min_x = std::min(min_x, p.x);
      max_x = std::max(max_x, p.x);
      min_y = std::min(min_y, p.y);
      max_y = std::max(max_y, p.y);
    }
  }

  constexpr double kMarginPx = 14.0;
  // A parked kart's bbox can degenerate to a point; keep the scale finite.
  double span_x = std::max(max_x - min_x, 1.0);
  double span_y = std::max(max_y - min_y, 1.0);
  double scale = std::min((kWidth - 2 * kMarginPx) / span_x,
                          (kHeight - 2 * kMarginPx) / span_y);
  double cx = (min_x + max_x) / 2, cy = (min_y + max_y) / 2;
  auto to_px = [&](const pacer::Point &p) {
    lv_point_precise_t px;
    px.x = (lv_value_precise_t)(kWidth / 2.0 + (p.x - cx) * scale);
    px.y = (lv_value_precise_t)(kHeight / 2.0 - (p.y - cy) * scale);
    return px;
  };

  // One edge through the gates' first endpoints, one through the seconds,
  // each closed back to its starting point.
  s_map_left_px.resize(n + 1);
  s_map_right_px.resize(n + 1);
  for (size_t i = 0; i < n; ++i) {
    s_map_left_px[i] = to_px(s_map_edge_first[i]);
    s_map_right_px[i] = to_px(s_map_edge_second[i]);
  }
  s_map_left_px[n] = s_map_left_px[0];
  s_map_right_px[n] = s_map_right_px[0];
  lv_line_set_points(s_map_left_line, s_map_left_px.data(), n + 1);
  lv_line_set_points(s_map_right_line, s_map_right_px.data(), n + 1);

  s_map_start_px[0] = to_px(s_map_edge_first[0]);
  s_map_start_px[1] = to_px(s_map_edge_second[0]);
  lv_line_set_points(s_map_start_line, s_map_start_px, 2);

  lv_obj_add_flag(s_map_empty_label, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(s_map_left_line, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(s_map_right_line, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(s_map_start_line, LV_OBJ_FLAG_HIDDEN);

  if (s_map_have_pos) {
    lv_point_precise_t px = to_px(s_map_pos);
    lv_obj_set_pos(s_map_marker, (int32_t)px.x - kMapMarkerSize / 2,
                   (int32_t)px.y - kMapMarkerSize / 2);
    lv_obj_remove_flag(s_map_marker, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_map_marker, LV_OBJ_FLAG_HIDDEN);
  }

  // The infill quads are painted in OnMapDraw from the px arrays above;
  // moving lines/marker alone wouldn't repaint the page background.
  lv_obj_invalidate(s_map_page);
}

// LV_EVENT_DRAW_MAIN on the map page: the annotator's infill, one quad
// between each consecutive pair of gates plus the wraparound one (a loaded
// track is always closed), each fanned into two triangles from its first
// vertex exactly like ImGui's AddConvexPolyFilled. Runs after the page
// background but before the children, so the edge lines, start line and
// marker stay on top.
void OnMapDraw(lv_event_t *e) {
  size_t n = s_map_edge_first.size();
  if (n < 2 || s_map_left_px.size() != n + 1 ||
      s_map_quad_sector.size() != n) {
    return;
  }
  lv_layer_t *layer = lv_event_get_layer(e);
  for (size_t i = 0; i < n; ++i) {
    // (a0, b0, b1, a1); the px arrays carry the closing point at index n,
    // which makes quad n-1 the wraparound automatically.
    const lv_point_precise_t &a0 = s_map_left_px[i];
    const lv_point_precise_t &b0 = s_map_right_px[i];
    const lv_point_precise_t &b1 = s_map_right_px[i + 1];
    const lv_point_precise_t &a1 = s_map_left_px[i + 1];

    lv_draw_triangle_dsc_t dsc;
    lv_draw_triangle_dsc_init(&dsc);
    dsc.color = kMapSectorColors[s_map_quad_sector[i] % kMapSectorColorCount];
    dsc.opa = LV_OPA_COVER;
    dsc.p[0] = a0;
    dsc.p[1] = b0;
    dsc.p[2] = b1;
    lv_draw_triangle(layer, &dsc);
    dsc.p[1] = b1;
    dsc.p[2] = a1;
    lv_draw_triangle(layer, &dsc);
  }
}

void RefreshLogToggleLabel() {
  lv_label_set_text(s_logtoggle_label,
                    s_logging_enabled ? "Stop writing" : "Start writing");
}

void ShowLoggingPage(bool show) {
  if (show) {
    RefreshLogToggleLabel();
    lv_obj_remove_flag(s_logging_page, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_logging_page, LV_OBJ_FLAG_HIDDEN);
  }
}

void OnScreenLongPress(lv_event_t *) {
  if (lv_obj_has_flag(s_menu, LV_OBJ_FLAG_HIDDEN) &&
      lv_obj_has_flag(s_nextline_page, LV_OBJ_FLAG_HIDDEN) &&
      lv_obj_has_flag(s_offset_page, LV_OBJ_FLAG_HIDDEN) &&
      lv_obj_has_flag(s_logging_page, LV_OBJ_FLAG_HIDDEN) &&
      lv_obj_has_flag(s_map_page, LV_OBJ_FLAG_HIDDEN)) {
    ShowMenu(true);
  }
}

void OnMenuNextLine(lv_event_t *) {
  ShowMenu(false);
  ShowNextLinePage(true);
}

void OnMenuOffset(lv_event_t *) {
  ShowMenu(false);
  ShowOffsetPage(true);
}

void OnOffsetBack(lv_event_t *) {
  ShowOffsetPage(false);
  ShowMenu(true);
}

void OnMenuTrackMap(lv_event_t *) {
  ShowMenu(false);
  ShowMapPage(true);
}

// The map is full-screen with no chrome; a tap anywhere backs out to the
// menu.
void OnMapClicked(lv_event_t *) {
  ShowMapPage(false);
  ShowMenu(true);
}

void OnMenuReload(lv_event_t *) {
  s_reload_request = true;
  ShowMenu(false);
}

void OnMenuLogging(lv_event_t *) {
  ShowMenu(false);
  ShowLoggingPage(true);
}

void OnLogToggle(lv_event_t *) {
  s_logging_enabled = !s_logging_enabled;
  RefreshLogToggleLabel();
}

void OnLoggingBack(lv_event_t *) {
  ShowLoggingPage(false);
  ShowMenu(true);
}

void OnMenuClose(lv_event_t *) { ShowMenu(false); }

void OnNextLineBack(lv_event_t *) {
  ShowNextLinePage(false);
  ShowMenu(true);
}

lv_obj_t *MakePanel(lv_obj_t *scr, const char *title) {
  lv_obj_t *panel = lv_obj_create(scr);
  lv_obj_set_size(panel, 320, 240);
  lv_obj_center(panel);
  lv_obj_set_style_bg_color(panel, lv_color_hex(0x181818), 0);
  lv_obj_set_style_border_color(panel, lv_color_hex(0x505050), 0);
  lv_obj_set_style_border_width(panel, 2, 0);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(panel, 8, 0);
  lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *t = lv_label_create(panel);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(t, lv_color_hex(0x808080), 0);
  lv_label_set_text(t, title);
  return panel;
}

lv_obj_t *MakeButton(lv_obj_t *parent, const char *text, lv_event_cb_t cb) {
  lv_obj_t *btn = lv_button_create(parent);
  lv_obj_set_width(btn, lv_pct(100));
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *label = lv_label_create(btn);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
  lv_label_set_text(label, text);
  lv_obj_center(label);
  return btn;
}

lv_obj_t *MakeMapLine(lv_color_t color, int32_t width) {
  lv_obj_t *line = lv_line_create(s_map_page);
  lv_obj_set_pos(line, 0, 0);
  lv_obj_set_size(line, kWidth, kHeight);
  lv_obj_set_style_line_color(line, color, 0);
  lv_obj_set_style_line_width(line, width, 0);
  lv_obj_set_style_line_rounded(line, true, 0);
  lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
  return line;
}

void BuildMapPage(lv_obj_t *scr) {
  s_map_page = lv_obj_create(scr);
  lv_obj_set_size(s_map_page, kWidth, kHeight);
  lv_obj_set_pos(s_map_page, 0, 0);
  lv_obj_set_style_bg_color(s_map_page, lv_color_black(), 0);
  lv_obj_set_style_border_width(s_map_page, 0, 0);
  lv_obj_set_style_radius(s_map_page, 0, 0);
  // Zero padding so the childrens' pixel coordinates equal RedrawMap()'s
  // screen coordinates.
  lv_obj_set_style_pad_all(s_map_page, 0, 0);
  lv_obj_remove_flag(s_map_page, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_map_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(s_map_page, OnMapClicked, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(s_map_page, OnMapDraw, LV_EVENT_DRAW_MAIN, nullptr);

  s_map_empty_label = lv_label_create(s_map_page);
  lv_obj_set_style_text_font(s_map_empty_label, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(s_map_empty_label, lv_color_hex(0x808080), 0);
  lv_label_set_text(s_map_empty_label, "no track");
  lv_obj_center(s_map_empty_label);

  s_map_left_line = MakeMapLine(lv_color_hex(0xB0B0B0), 2);
  s_map_right_line = MakeMapLine(lv_color_hex(0xB0B0B0), 2);
  s_map_start_line = MakeMapLine(lv_color_white(), 4);

  s_map_marker = lv_obj_create(s_map_page);
  lv_obj_set_size(s_map_marker, kMapMarkerSize, kMapMarkerSize);
  lv_obj_set_style_radius(s_map_marker, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(s_map_marker, lv_color_hex(0x30E050), 0);
  lv_obj_set_style_border_width(s_map_marker, 0, 0);
  // Not clickable, so taps on the marker still land on the page (= back).
  lv_obj_remove_flag(s_map_marker, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(s_map_marker, LV_OBJ_FLAG_HIDDEN);
}

void BuildDebugMenu(lv_obj_t *scr) {
  s_menu = MakePanel(scr, "DEBUG");
  MakeButton(s_menu, "Next timing line", OnMenuNextLine);
  MakeButton(s_menu, "Track offset", OnMenuOffset);
  MakeButton(s_menu, "Track map", OnMenuTrackMap);
  MakeButton(s_menu, "Logging", OnMenuLogging);
  MakeButton(s_menu, "Reload track", OnMenuReload);
  MakeButton(s_menu, "Close", OnMenuClose);

  s_nextline_page = MakePanel(scr, "NEXT TIMING LINE");
  s_nextline_label = lv_label_create(s_nextline_page);
  lv_obj_set_style_text_font(s_nextline_label, &lv_font_montserrat_48, 0);
  lv_label_set_text(s_nextline_label, "--");
  MakeButton(s_nextline_page, "Back", OnNextLineBack);

  s_offset_page = MakePanel(scr, "TRACK OFFSET");
  s_offset_label = lv_label_create(s_offset_page);
  lv_obj_set_style_text_font(s_offset_label, &lv_font_montserrat_48, 0);
  lv_label_set_text(s_offset_label, "--");
  s_offset_detail_label = lv_label_create(s_offset_page);
  lv_obj_set_style_text_font(s_offset_detail_label, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(s_offset_detail_label, lv_color_hex(0x808080),
                              0);
  lv_label_set_text(s_offset_detail_label, "--");
  MakeButton(s_offset_page, "Back", OnOffsetBack);

  s_logging_page = MakePanel(scr, "LOGGING");
  s_logstats_label = lv_label_create(s_logging_page);
  lv_obj_set_style_text_font(s_logstats_label, &lv_font_montserrat_24, 0);
  lv_label_set_text(s_logstats_label, "--");
  lv_obj_t *toggle = MakeButton(s_logging_page, "Stop writing", OnLogToggle);
  s_logtoggle_label = lv_obj_get_child(toggle, 0);
  MakeButton(s_logging_page, "Back", OnLoggingBack);

  BuildMapPage(scr);

  lv_obj_add_event_cb(scr, OnScreenLongPress, LV_EVENT_LONG_PRESSED, nullptr);
}

void BuildScreen() {
  lv_obj_t *scr = lv_display_get_screen_active(s_disp);
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_set_style_text_color(scr, lv_color_white(), 0);

  s_lap_label = lv_label_create(scr);
  lv_obj_set_style_text_font(s_lap_label, &lv_font_montserrat_24, 0);
  lv_obj_align(s_lap_label, LV_ALIGN_TOP_LEFT, 6, 4);
  lv_label_set_text(s_lap_label, "LAP -");

  s_clock_label = lv_label_create(scr);
  lv_obj_set_style_text_font(s_clock_label, &lv_font_montserrat_24, 0);
  lv_obj_align(s_clock_label, LV_ALIGN_TOP_RIGHT, -6, 4);
  lv_label_set_text(s_clock_label, "--:--");

  s_delta_label = lv_label_create(scr);
  lv_obj_set_style_text_font(s_delta_label, &lv_font_montserrat_48, 0);
  lv_obj_align(s_delta_label, LV_ALIGN_CENTER, 0, -30);
  lv_label_set_text(s_delta_label, "--.--");

  s_current_label = lv_label_create(scr);
  lv_obj_set_style_text_font(s_current_label, &lv_font_montserrat_32, 0);
  lv_obj_align(s_current_label, LV_ALIGN_CENTER, 0, 20);
  lv_label_set_text(s_current_label, "-:--.---");

  s_last_label = lv_label_create(scr);
  lv_obj_set_style_text_font(s_last_label, &lv_font_montserrat_20, 0);
  lv_obj_align(s_last_label, LV_ALIGN_BOTTOM_LEFT, 6, -26);
  lv_label_set_text(s_last_label, "LAST --:--.---");

  s_best_label = lv_label_create(scr);
  lv_obj_set_style_text_font(s_best_label, &lv_font_montserrat_20, 0);
  lv_obj_align(s_best_label, LV_ALIGN_BOTTOM_RIGHT, -6, -26);
  lv_label_set_text(s_best_label, "BEST --:--.---");

  // Full-width strip; long debug text scrolls marquee-style instead of
  // running off screen.
  s_status_label = lv_label_create(scr);
  lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x808080), 0);
  lv_obj_set_width(s_status_label, kWidth - 12);
  lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_LEFT, 6, -4);
  lv_label_set_text(s_status_label, "starting...");

  s_debug_label = lv_label_create(scr);
  lv_obj_set_style_text_font(s_debug_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_debug_label, lv_color_hex(0x808080), 0);
  lv_obj_align(s_debug_label, LV_ALIGN_TOP_MID, 0, 4);
  lv_label_set_text(s_debug_label, "");

  BuildDebugMenu(scr);
}

} // namespace

esp_err_t dashboard_ui_start() {
  // NV3041A panel on QSPI: one SCLK + four data lines, no DC pin.
  spi_bus_config_t bus_cfg = {};
  bus_cfg.sclk_io_num = CONFIG_PACER_LCD_PCLK_GPIO;
  bus_cfg.data0_io_num = CONFIG_PACER_LCD_DATA0_GPIO;
  bus_cfg.data1_io_num = CONFIG_PACER_LCD_DATA1_GPIO;
  bus_cfg.data2_io_num = CONFIG_PACER_LCD_DATA2_GPIO;
  bus_cfg.data3_io_num = CONFIG_PACER_LCD_DATA3_GPIO;
  bus_cfg.max_transfer_sz = kWidth * 80 * sizeof(uint16_t);
  ESP_ERROR_CHECK(spi_bus_initialize(
      (spi_host_device_t)CONFIG_PACER_LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

  esp_lcd_panel_io_handle_t io = nullptr;
  esp_lcd_panel_io_spi_config_t io_cfg = {};
  io_cfg.cs_gpio_num = CONFIG_PACER_LCD_CS_GPIO;
  io_cfg.dc_gpio_num = -1;
  io_cfg.spi_mode = 0;
  io_cfg.pclk_hz = 32 * 1000 * 1000;
  io_cfg.trans_queue_depth = 10;
  io_cfg.lcd_cmd_bits = 32;
  io_cfg.lcd_param_bits = 8;
  io_cfg.flags.quad_mode = true;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
      (esp_lcd_spi_bus_handle_t)CONFIG_PACER_LCD_SPI_HOST, &io_cfg, &io));

  esp_lcd_panel_handle_t panel = nullptr;
  nv3041a_vendor_config_t vendor_cfg = {};
  vendor_cfg.flags.use_qspi_interface = 1;
  esp_lcd_panel_dev_config_t panel_cfg = {};
  panel_cfg.reset_gpio_num = CONFIG_PACER_LCD_RST_GPIO;
  panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  panel_cfg.bits_per_pixel = 16;
  panel_cfg.vendor_config = &vendor_cfg;
  ESP_ERROR_CHECK(esp_lcd_new_panel_nv3041a(io, &panel_cfg, &panel));

  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
#if CONFIG_PACER_LCD_INVERT_COLORS
  ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
#endif
#if CONFIG_PACER_LCD_SWAP_XY
  ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
#endif
  ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, kMirrorX, kMirrorY));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

#if CONFIG_PACER_LCD_BL_GPIO >= 0
  gpio_config_t bl_cfg = {};
  bl_cfg.mode = GPIO_MODE_OUTPUT;
  bl_cfg.pin_bit_mask = 1ULL << CONFIG_PACER_LCD_BL_GPIO;
  gpio_config(&bl_cfg);
  gpio_set_level((gpio_num_t)CONFIG_PACER_LCD_BL_GPIO, 1);
#endif

  const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
  ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

  lvgl_port_display_cfg_t disp_cfg = {};
  disp_cfg.io_handle = io;
  disp_cfg.panel_handle = panel;
  disp_cfg.buffer_size = kWidth * 40;
  disp_cfg.double_buffer = true;
  disp_cfg.hres = kWidth;
  disp_cfg.vres = kHeight;
  disp_cfg.monochrome = false;
  disp_cfg.color_format = LV_COLOR_FORMAT_RGB565;
  disp_cfg.rotation.mirror_x = kMirrorX;
  disp_cfg.rotation.mirror_y = kMirrorY;
  disp_cfg.flags.buff_dma = true;
  disp_cfg.flags.swap_bytes = true;
  s_disp = lvgl_port_add_disp(&disp_cfg);
  if (!s_disp) {
    return ESP_FAIL;
  }

#if CONFIG_PACER_TOUCH_ENABLED
  // GT911 touch on I2C, fed to LVGL as a pointer device.
  i2c_master_bus_handle_t i2c_bus = nullptr;
  i2c_master_bus_config_t i2c_cfg = {};
  i2c_cfg.i2c_port = -1;
  i2c_cfg.sda_io_num = (gpio_num_t)CONFIG_PACER_TOUCH_SDA_GPIO;
  i2c_cfg.scl_io_num = (gpio_num_t)CONFIG_PACER_TOUCH_SCL_GPIO;
  i2c_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  i2c_cfg.glitch_ignore_cnt = 7;
  i2c_cfg.flags.enable_internal_pullup = true;
  esp_err_t touch_err = i2c_new_master_bus(&i2c_cfg, &i2c_bus);

  if (touch_err == ESP_OK) {
    // ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG initializes fields out of declaration
    // order, which C++ rejects — spell it out instead.
    esp_lcd_panel_io_handle_t tp_io = nullptr;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = {};
    tp_io_cfg.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
    tp_io_cfg.control_phase_bytes = 1;
    tp_io_cfg.dc_bit_offset = 0;
    tp_io_cfg.lcd_cmd_bits = 16;
    tp_io_cfg.flags.disable_control_phase = 1;
    tp_io_cfg.scl_speed_hz = 400 * 1000;
    touch_err = esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_cfg, &tp_io);

    esp_lcd_touch_handle_t tp = nullptr;
    if (touch_err == ESP_OK) {
      esp_lcd_touch_config_t tp_cfg = {};
      tp_cfg.x_max = kWidth;
      tp_cfg.y_max = kHeight;
      tp_cfg.rst_gpio_num = (gpio_num_t)CONFIG_PACER_TOUCH_RST_GPIO;
      tp_cfg.int_gpio_num = (gpio_num_t)CONFIG_PACER_TOUCH_INT_GPIO;
      tp_cfg.levels.reset = 0;
      tp_cfg.levels.interrupt = 0;
      // Touch reports in panel-native coordinates; apply the same transform
      // as the panel so taps land where the eye sees them.
#if CONFIG_PACER_LCD_SWAP_XY
      tp_cfg.flags.swap_xy = 1;
#endif
      tp_cfg.flags.mirror_x = !kMirrorX;
      tp_cfg.flags.mirror_y = !kMirrorY;
      touch_err = esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &tp);
    }

    if (touch_err == ESP_OK) {
      lvgl_port_touch_cfg_t touch_cfg = {};
      touch_cfg.disp = s_disp;
      touch_cfg.handle = tp;
      lvgl_port_add_touch(&touch_cfg);
      ESP_LOGI(TAG, "gt911 touch up");
    }
  }
  if (touch_err != ESP_OK) {
    // Touch is a nice-to-have; the dashboard is fully usable without it.
    ESP_LOGW(TAG, "touch init failed: %s", esp_err_to_name(touch_err));
  }
#endif

  lvgl_port_lock(0);
  BuildScreen();
  lvgl_port_unlock();

  ESP_LOGI(TAG, "display up (%dx%d)", kWidth, kHeight);
  return ESP_OK;
}

void dashboard_ui_update(const pacer::LiveSnapshot &snap) {
  if (!s_disp) {
    return;
  }
  char buf[48];
  lvgl_port_lock(0);

  snprintf(buf, sizeof(buf), "LAP %d", snap.lap_number);
  lv_label_set_text(s_lap_label, buf);

  FormatCountdown(buf, sizeof(buf), snap.session_remaining_s);
  lv_label_set_text(s_clock_label, buf);

  if (snap.delta_valid) {
    snprintf(buf, sizeof(buf), "%+.2f", snap.delta_s);
    lv_label_set_text(s_delta_label, buf);
    lv_obj_set_style_text_color(
        s_delta_label,
        snap.delta_s <= 0 ? lv_color_hex(0x30E050) : lv_color_hex(0xF04030), 0);
  } else {
    lv_label_set_text(s_delta_label, "--.--");
    lv_obj_set_style_text_color(s_delta_label, lv_color_hex(0x808080), 0);
  }

  FormatLapTime(buf, sizeof(buf), snap.current_lap_s);
  lv_label_set_text(s_current_label, buf);

  char timebuf[24];
  FormatLapTime(timebuf, sizeof(timebuf), snap.last_lap_s);
  snprintf(buf, sizeof(buf), "LAST %s", timebuf);
  lv_label_set_text(s_last_label, buf);

  FormatLapTime(timebuf, sizeof(timebuf), snap.best_lap_s);
  snprintf(buf, sizeof(buf), "BEST %s", timebuf);
  lv_label_set_text(s_best_label, buf);

  lvgl_port_unlock();
}

void dashboard_ui_set_status(const char *text) {
  if (!s_disp) {
    return;
  }
  lvgl_port_lock(0);
  lv_label_set_text(s_status_label, text);
  lvgl_port_unlock();
}

void dashboard_ui_set_debug(const char *text) {
  if (!s_disp) {
    return;
  }
  lvgl_port_lock(0);
  lv_label_set_text(s_debug_label, text);
  lvgl_port_unlock();
}

bool dashboard_ui_consume_track_reload() {
  return s_reload_request.exchange(false);
}

bool dashboard_ui_logging_enabled() { return s_logging_enabled; }

void dashboard_ui_set_log_stats(size_t written, size_t flushed) {
  if (!s_disp || !s_logstats_label) {
    return;
  }
  if (lv_obj_has_flag(s_logging_page, LV_OBJ_FLAG_HIDDEN)) {
    return;
  }
  char buf[48];
  snprintf(buf, sizeof(buf), "%u flushed\n%u written", (unsigned)flushed,
           (unsigned)written);
  lvgl_port_lock(0);
  lv_label_set_text(s_logstats_label, buf);
  lvgl_port_unlock();
}

void dashboard_ui_set_track_map(const std::vector<pacer::Segment> &gates,
                                const std::vector<int> &sector_splits) {
  if (!s_disp || !s_map_page) {
    return;
  }
  size_t n = gates.size();
  std::vector<bool> is_split(n, false);
  for (int idx : sector_splits) {
    if (idx >= 0 && static_cast<size_t>(idx) < n) {
      is_split[idx] = true;
    }
  }

  lvgl_port_lock(0);
  s_map_edge_first.clear();
  s_map_edge_second.clear();
  s_map_edge_first.reserve(n);
  s_map_edge_second.reserve(n);
  for (const pacer::Segment &g : gates) {
    s_map_edge_first.push_back(g.first);
    s_map_edge_second.push_back(g.second);
  }
  s_map_quad_sector.assign(n, 0);
  if (n >= 2) {
    uint8_t sector = 0;
    for (size_t i = 0; i + 1 < n; ++i) {
      s_map_quad_sector[i] = sector;
      if (is_split[i + 1]) {
        ++sector;
      }
    }
    s_map_quad_sector[n - 1] = sector; // wraparound quad
  }
  // Any previous position was in the old track's frame.
  s_map_have_pos = false;
  RedrawMap();
  lvgl_port_unlock();
}

bool dashboard_ui_track_map_visible() {
  return s_disp && s_map_page &&
         !lv_obj_has_flag(s_map_page, LV_OBJ_FLAG_HIDDEN);
}

void dashboard_ui_set_track_map_position(double x_m, double y_m) {
  if (!dashboard_ui_track_map_visible()) {
    return;
  }
  lvgl_port_lock(0);
  s_map_have_pos = true;
  s_map_pos = pacer::Point{x_m, y_m};
  RedrawMap();
  lvgl_port_unlock();
}

bool dashboard_ui_track_offset_visible() {
  return s_disp && s_offset_page &&
         !lv_obj_has_flag(s_offset_page, LV_OBJ_FLAG_HIDDEN);
}

void dashboard_ui_set_track_offset(double lateral_m, double half_width_m,
                                   size_t gate, size_t gate_count) {
  if (!dashboard_ui_track_offset_visible()) {
    return;
  }
  char buf[24];
  char detail[48];
  bool off_track = false;
  if (std::isnan(lateral_m)) {
    snprintf(buf, sizeof(buf), "no track");
    snprintf(detail, sizeof(detail), "--");
  } else {
    snprintf(buf, sizeof(buf), "%+.1f m", lateral_m);
    snprintf(detail, sizeof(detail), "gate %u/%u   half-width %.1f m",
             (unsigned)gate, (unsigned)gate_count, half_width_m);
    off_track = std::fabs(lateral_m) > half_width_m;
  }
  lvgl_port_lock(0);
  lv_label_set_text(s_offset_label, buf);
  lv_obj_set_style_text_color(
      s_offset_label, off_track ? lv_color_hex(0xF04030) : lv_color_white(),
      0);
  lv_label_set_text(s_offset_detail_label, detail);
  lvgl_port_unlock();
}

void dashboard_ui_set_next_line_distance(double meters) {
  if (!s_disp || !s_nextline_label) {
    return;
  }
  // Skip the LVGL lock round-trip while the page isn't visible.
  if (lv_obj_has_flag(s_nextline_page, LV_OBJ_FLAG_HIDDEN)) {
    return;
  }
  char buf[24];
  if (std::isnan(meters)) {
    snprintf(buf, sizeof(buf), "no track");
  } else {
    snprintf(buf, sizeof(buf), "%.0f m", meters);
  }
  lvgl_port_lock(0);
  lv_label_set_text(s_nextline_label, buf);
  lvgl_port_unlock();
}
