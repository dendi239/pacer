#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <hello_imgui/hello_imgui.h>
#include <imgui.h>
#include <imgui_stdlib.h>

#include <pacer/map-tiles/canvas-tiles.hpp>
#include <pacer/map-tiles/tile-store.hpp>
#include <pacer/reference-track/reference-track.hpp>
#include <pacer/ui/track-picker.hpp>

struct TrackPoint {
  float lat = 0.0f;
  float lon = 0.0f;
};

struct TrackSegment {
  TrackPoint a;                 // inner edge
  TrackPoint b;                 // outer edge
  bool complete = true;         // if false, b is not set yet
  bool is_sector_split = false; // marks this gate as a sector boundary
};

// The undoable part of the annotation: the drawn gates and whether the
// track loops back on itself. View/camera state and file paths are
// intentionally excluded from undo/redo history.
struct TrackDocument {
  std::vector<TrackSegment> segments;
  bool track_closed = false;
};

struct TrackState {
  std::vector<TrackSegment> segments;
  int selected_segment = -1;
  int selected_end = 0;      // 0 = a, 1 = b
  int hovered_segment = -1;  // row currently hovered in the segment table
  bool track_closed = false; // joins last gate to first; blocks new gates
  std::vector<TrackDocument> undo_stack;
  std::vector<TrackDocument> redo_stack;
  bool dragging_point = false;
  bool panning_map = false;
  bool show_infill = true;
  std::string last_message;

  pacer::TileCanvasView view;
  pacer::TileStore tiles;
  pacer::TrackFilePicker picker;
};

// Records the current document (segments + track_closed) onto the undo
// stack and clears redo history, as usual once a new edit is made. Call
// this right before a mutation, so the pushed snapshot is the pre-edit
// state.
static void PushUndo(TrackState &state) {
  constexpr size_t kMaxUndoHistory = 200;
  state.undo_stack.push_back(TrackDocument{state.segments, state.track_closed});
  state.redo_stack.clear();
  if (state.undo_stack.size() > kMaxUndoHistory) {
    state.undo_stack.erase(state.undo_stack.begin());
  }
}

static void RestoreDocument(TrackState &state, TrackDocument doc) {
  state.segments = std::move(doc.segments);
  state.track_closed = doc.track_closed;
  state.selected_segment = -1;
  state.dragging_point = false;
}

static void Undo(TrackState &state) {
  if (state.undo_stack.empty()) {
    return;
  }
  state.redo_stack.push_back(TrackDocument{state.segments, state.track_closed});
  TrackDocument doc = std::move(state.undo_stack.back());
  state.undo_stack.pop_back();
  RestoreDocument(state, std::move(doc));
}

static void Redo(TrackState &state) {
  if (state.redo_stack.empty()) {
    return;
  }
  state.undo_stack.push_back(TrackDocument{state.segments, state.track_closed});
  TrackDocument doc = std::move(state.redo_stack.back());
  state.redo_stack.pop_back();
  RestoreDocument(state, std::move(doc));
}

static ImVec2 WorldToScreen(const TrackPoint &point, const ImVec2 &canvas_min,
                            const ImVec2 &canvas_max, const TrackState &state) {
  return pacer::CanvasFromLatLon(point.lat, point.lon, state.view, canvas_min,
                                 canvas_max);
}

static TrackPoint ScreenToWorld(const ImVec2 &screen, const ImVec2 &canvas_min,
                                const ImVec2 &canvas_max,
                                const TrackState &state) {
  auto [lat, lon] =
      pacer::LatLonFromCanvas(screen, state.view, canvas_min, canvas_max);
  return TrackPoint{static_cast<float>(lat), static_cast<float>(lon)};
}

static bool PointHit(const ImVec2 &screen_pos, const ImVec2 &point_screen,
                     float radius) {
  float dx = screen_pos.x - point_screen.x;
  float dy = screen_pos.y - point_screen.y;
  return dx * dx + dy * dy <= radius * radius;
}

static bool SaveState(const TrackState &state, const std::string &filename) {
  std::vector<TrackSegment> complete_segments;
  for (const auto &seg : state.segments) {
    if (seg.complete) {
      complete_segments.push_back(seg);
    }
  }

  pacer::ReferenceTrack track;
  if (!complete_segments.empty()) {
    track.cs = pacer::CoordinateSystem(pacer::GPSSample{
        .lat = complete_segments.front().a.lat,
        .lon = complete_segments.front().a.lon,
    });
  }
  for (int i = 0; i < (int)complete_segments.size(); ++i) {
    const auto &seg = complete_segments[i];
    pacer::GPSSample a{.lat = seg.a.lat, .lon = seg.a.lon};
    pacer::GPSSample b{.lat = seg.b.lat, .lon = seg.b.lon};
    track.segments.push_back(pacer::Segment{pacer::ToPoint(track.cs.Local(a)),
                                            pacer::ToPoint(track.cs.Local(b))});
    if (seg.is_sector_split) {
      track.sector_indices.push_back(i);
    }
  }

  try {
    track.SaveToFile(filename);
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

static bool ReadSegmentsFromFile(const std::string &filename,
                                 std::vector<TrackSegment> &out_segments,
                                 std::string &error) {
  try {
    pacer::ReferenceTrack track = pacer::ReferenceTrack::FromFile(filename);
    std::vector<TrackSegment> segments;
    segments.reserve(track.segments.size());
    for (const auto &seg : track.segments) {
      auto a = track.cs.Global(pacer::Vec3f{seg.first.x, seg.first.y, 0});
      auto b = track.cs.Global(pacer::Vec3f{seg.second.x, seg.second.y, 0});
      TrackSegment s;
      s.a.lat = static_cast<float>(a.lat);
      s.a.lon = static_cast<float>(a.lon);
      s.b.lat = static_cast<float>(b.lat);
      s.b.lon = static_cast<float>(b.lon);
      s.complete = true;
      segments.push_back(s);
    }
    for (int index : track.sector_indices) {
      if (index >= 0 && index < (int)segments.size()) {
        segments[index].is_sector_split = true;
      }
    }
    out_segments = std::move(segments);
  } catch (const std::exception &e) {
    error = std::string("Invalid state file: ") + e.what();
    return false;
  }

  return true;
}

// Centers the map on the geometric center of state.segments. Does not touch
// the zoom.
static void RecenterMapOnSegments(TrackState &state) {
  double sum_lat = 0.0;
  double sum_lon = 0.0;
  int count = 0;
  for (const auto &s : state.segments) {
    sum_lat += s.a.lat;
    sum_lon += s.a.lon;
    ++count;
    sum_lat += s.b.lat;
    sum_lon += s.b.lon;
    ++count;
  }
  if (count > 0) {
    state.view.lat = sum_lat / count;
    state.view.lon = sum_lon / count;
  }
}

static bool LoadState(TrackState &state, const std::string &filename) {
  std::vector<TrackSegment> segments;
  std::string error;
  if (!ReadSegmentsFromFile(filename, segments, error))
    return false;

  state.segments = std::move(segments);

  // The view is not part of the file; center the map on the loaded segments.
  RecenterMapOnSegments(state);

  state.selected_segment = -1;
  state.dragging_point = false;
  state.track_closed = true;

  return true;
}

// Left column: view controls, selected-segment editor, file load/save, and
// the segment table.
static void DrawControlPanel(TrackState &state, float width) {
  // Child window (scrollable) so it doesn't scroll the map.
  ImGui::BeginChild("left_panel", ImVec2(width, 0), true,
                    ImGuiWindowFlags_None);
  ImGui::TextWrapped(
      "Draw a sequence of gates (inner edge to outer edge) around the "
      "circuit, in the direction of travel.");
  ImGui::TextWrapped(
      "Map: left-click to pan, or left-click and drag a point to move it. "
      "Right-click to place a gate point (first click sets the inner "
      "edge, the next sets the outer edge; the following right-click "
      "starts a new gate). Scroll to zoom; tiles load automatically.");
  ImGui::Spacing();

  if (ImGui::Button("Recenter")) {
    RecenterMapOnSegments(state);
  }
  ImGui::SameLine();
  if (ImGui::Button("Reset Track")) {
    PushUndo(state);
    state.segments.clear();
    state.selected_segment = -1;
    state.selected_end = 0;
    state.track_closed = false;
  }
  {
    bool old_closed = state.track_closed;
    if (ImGui::Checkbox("Close the track", &state.track_closed)) {
      bool new_closed = state.track_closed;
      state.track_closed = old_closed;
      PushUndo(state);
      state.track_closed = new_closed;
    }
  }
  ImGui::SameLine();
  ImGui::Checkbox("Show infill", &state.show_infill);

  ImGui::Separator();

  // Selected segment editor, save/load and segment table live in left column.
  if (state.selected_segment >= 0 &&
      state.selected_segment < (int)state.segments.size()) {
    auto &seg = state.segments[state.selected_segment];
    ImGui::PushItemWidth(80.0f);
    ImGui::Text("Point A");
    ImGui::SameLine();
    ImGui::DragFloat("##a_lat", &seg.a.lat, 0.00005f, -85.0f, 85.0f, "%.5f");
    if (ImGui::IsItemActivated())
      PushUndo(state);
    ImGui::SameLine();
    ImGui::DragFloat("##a_lon", &seg.a.lon, 0.00005f, -180.0f, 180.0f, "%.5f");
    if (ImGui::IsItemActivated())
      PushUndo(state);
    ImGui::Text("Point B");
    ImGui::SameLine();
    ImGui::DragFloat("##b_lat", &seg.b.lat, 0.00005f, -85.0f, 85.0f, "%.5f");
    if (ImGui::IsItemActivated())
      PushUndo(state);
    ImGui::SameLine();
    ImGui::DragFloat("##b_lon", &seg.b.lon, 0.00005f, -180.0f, 180.0f, "%.5f");
    if (ImGui::IsItemActivated())
      PushUndo(state);
    ImGui::PopItemWidth();
    if (ImGui::Button("Delete segment") && state.selected_segment >= 0) {
      PushUndo(state);
      auto it = state.segments.begin() + state.selected_segment;
      state.segments.erase(it);
      state.selected_segment = -1;
    }

    // Insert a gate interpolated from the selected one and its neighbor, for
    // refining corners without hand-placing a whole new point.
    int i = state.selected_segment;
    bool has_next = i + 1 < (int)state.segments.size();
    bool has_prev = i > 0;
    ImGui::BeginDisabled(!has_next);
    if (ImGui::Button("Add Next")) {
      PushUndo(state);
      int j = i + 1;
      TrackSegment mid;
      mid.a.lat = (state.segments[i].a.lat + state.segments[j].a.lat) / 2.0f;
      mid.a.lon = (state.segments[i].a.lon + state.segments[j].a.lon) / 2.0f;
      mid.b.lat = (state.segments[i].b.lat + state.segments[j].b.lat) / 2.0f;
      mid.b.lon = (state.segments[i].b.lon + state.segments[j].b.lon) / 2.0f;
      mid.complete = true;
      mid.is_sector_split = false;
      state.segments.insert(state.segments.begin() + j, mid);
      state.selected_segment = j;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!has_prev);
    if (ImGui::Button("Add Prev")) {
      PushUndo(state);
      int j = i - 1;
      TrackSegment mid;
      mid.a.lat = (state.segments[j].a.lat + state.segments[i].a.lat) / 2.0f;
      mid.a.lon = (state.segments[j].a.lon + state.segments[i].a.lon) / 2.0f;
      mid.b.lat = (state.segments[j].b.lat + state.segments[i].b.lat) / 2.0f;
      mid.b.lon = (state.segments[j].b.lon + state.segments[i].b.lon) / 2.0f;
      mid.complete = true;
      mid.is_sector_split = false;
      state.segments.insert(state.segments.begin() + i, mid);
      state.selected_segment = i;
    }
    ImGui::EndDisabled();
  }

  ImGui::Spacing();
  ImGui::TextWrapped(
      "Save / Load read and write the gates and sector markings to the "
      "selected track file.");
  state.picker.Draw("track_file");
  if (ImGui::Button("Save##save")) {
    if (state.picker.path.empty() ||
        !SaveState(state, state.picker.path)) {
      state.last_message =
          "Unable to write state to '" + state.picker.path + "'";
      HelloImGui::Log(HelloImGui::LogLevel::Error, "%s",
                      state.last_message.c_str());
    } else {
      state.last_message = "Saved state to '" + state.picker.path + "'";
      state.picker.Refresh();
      HelloImGui::Log(HelloImGui::LogLevel::Info, "%s",
                      state.last_message.c_str());
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Load##load")) {
    PushUndo(state);
    if (!LoadState(state, state.picker.path)) {
      state.undo_stack.pop_back(); // load failed, nothing actually changed
      state.last_message =
          "Unable to load state from '" + state.picker.path + "'";
      HelloImGui::Log(HelloImGui::LogLevel::Error, "%s",
                      state.last_message.c_str());
      // Try to provide more context: attempt to stat the file from cwd
      std::ifstream f(state.picker.path);
      if (!f.is_open()) {
        try {
          std::filesystem::path p =
              std::filesystem::current_path() / state.picker.path;
          state.last_message += " (tried: " + p.string() + ")";
        } catch (...) {
        }
      }
    } else {
      state.last_message = "Loaded state from '" + state.picker.path + "'";
      HelloImGui::Log(HelloImGui::LogLevel::Info, "%s",
                      state.last_message.c_str());
    }
  }

  ImGui::Spacing();
  ImGui::TextWrapped(
      "Segment table: click a row to select that gate for editing above. "
      "Check Sector to mark it as a sector-split boundary; gate 0 is "
      "always the start/finish line.");
  state.hovered_segment = -1;
  if (ImGui::BeginTable("points_table", 3, ImGuiTableFlags_Borders)) {
    ImGui::TableSetupColumn("Index");
    ImGui::TableSetupColumn("Point");
    ImGui::TableSetupColumn("Sector");
    ImGui::TableHeadersRow();
    for (int i = 0; i < (int)state.segments.size(); ++i) {
      ImGui::PushID(i);
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      std::string index_label =
          i == 0 ? std::to_string(i) + " (start/finish)" : std::to_string(i);
      if (ImGui::Selectable(index_label.c_str(), state.selected_segment == i,
                            ImGuiSelectableFlags_SpanAllColumns |
                                ImGuiSelectableFlags_AllowOverlap)) {
        state.selected_segment = i;
      }
      if (ImGui::IsItemHovered()) {
        state.hovered_segment = i;
      }
      ImGui::TableSetColumnIndex(1);
      ImGui::Text("A: %.5f, %.5f | B: %.5f, %.5f", state.segments[i].a.lat,
                  state.segments[i].a.lon, state.segments[i].b.lat,
                  state.segments[i].b.lon);
      ImGui::TableSetColumnIndex(2);
      if (i > 0) {
        bool old_value = state.segments[i].is_sector_split;
        if (ImGui::Checkbox("##sector", &state.segments[i].is_sector_split)) {
          bool new_value = state.segments[i].is_sector_split;
          state.segments[i].is_sector_split = old_value;
          PushUndo(state);
          state.segments[i].is_sector_split = new_value;
        }
      } else {
        // Disabled placeholder so row 0 has the same height as every other
        // row (which all have a real checkbox here) -- otherwise this row
        // renders shorter and every checkbox below it visually creeps up by
        // about a row.
        bool not_applicable = false;
        ImGui::BeginDisabled();
        ImGui::Checkbox("##sector", &not_applicable);
        ImGui::EndDisabled();
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  if (!state.last_message.empty()) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.9f, 0.5f, 1.0f));
    ImGui::TextWrapped("%s", state.last_message.c_str());
    ImGui::PopStyleColor();
  }

  ImGui::EndChild();
}

// Applies pan/zoom/click/drag input for the map canvas. hovered indicates
// whether the mouse is over the canvas item drawn just before this call.
static void HandleMapInput(TrackState &state, const ImVec2 &canvas_min,
                           const ImVec2 &canvas_max, bool hovered) {
  ImVec2 mouse_pos = ImGui::GetIO().MousePos;
  bool left_clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  bool right_clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
  bool left_drag = hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left);
  float wheel = hovered ? ImGui::GetIO().MouseWheel : 0.0f;

  if (wheel != 0.0f) {
    pacer::ZoomCanvasAt(state.view, wheel > 0 ? 1.1f : 0.9f, mouse_pos,
                        canvas_min, canvas_max);
  }

  if (left_drag && state.panning_map) {
    ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
    pacer::PanCanvas(state.view, delta);
    ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
  }

  if (right_clicked && !state.track_closed) {
    PushUndo(state);
    TrackPoint world = ScreenToWorld(mouse_pos, canvas_min, canvas_max, state);
    if (state.segments.empty() || state.segments.back().complete) {
      TrackSegment s;
      s.a = world;
      s.b = world;
      s.complete = false;
      state.segments.push_back(s);
      state.selected_segment = static_cast<int>(state.segments.size() - 1);
      state.selected_end = 0;
      state.dragging_point = true;
    } else {
      // complete the last segment's b point
      int idx = static_cast<int>(state.segments.size() - 1);
      state.segments[idx].b = world;
      state.segments[idx].complete = true;
      state.selected_segment = idx;
      state.selected_end = 1;
      state.dragging_point = true;
    }
  }

  int hit_segment = -1;
  int hit_end = 0;
  float hit_radius = 10.0f;
  for (int i = 0; i < (int)state.segments.size(); ++i) {
    ImVec2 sa =
        WorldToScreen(state.segments[i].a, canvas_min, canvas_max, state);
    ImVec2 sb =
        WorldToScreen(state.segments[i].b, canvas_min, canvas_max, state);
    if (PointHit(mouse_pos, sa, hit_radius)) {
      hit_segment = i;
      hit_end = 0;
      break;
    }
    if (PointHit(mouse_pos, sb, hit_radius)) {
      hit_segment = i;
      hit_end = 1;
      break;
    }
  }

  if (left_clicked) {
    if (hit_segment != -1) {
      PushUndo(state);
      state.selected_segment = hit_segment;
      state.selected_end = hit_end;
      state.dragging_point = true;
      state.panning_map = false;
    } else {
      state.selected_segment = -1;
      state.dragging_point = false;
      state.panning_map = true;
    }
  }
  if (state.dragging_point && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
      state.selected_segment >= 0 &&
      state.selected_segment < (int)state.segments.size()) {
    TrackPoint pt = ScreenToWorld(mouse_pos, canvas_min, canvas_max, state);
    if (state.selected_end == 0)
      state.segments[state.selected_segment].a = pt;
    else
      state.segments[state.selected_segment].b = pt;
  }
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    state.dragging_point = false;
    state.panning_map = false;
  }
}

// Draws the track quads (infill + outline) and segment endpoint handles.
static void DrawTrackOverlay(const TrackState &state, ImDrawList *draw_list,
                             const ImVec2 &canvas_min,
                             const ImVec2 &canvas_max) {
  // Cycled per sector (a run of gates between sector-split boundaries) so
  // adjacent sectors are visually distinguishable.
  static const ImU32 kSectorColors[] = {
      IM_COL32(180, 220, 255, 160), IM_COL32(255, 200, 140, 160),
      IM_COL32(180, 255, 180, 160), IM_COL32(255, 180, 220, 160),
      IM_COL32(220, 220, 140, 160), IM_COL32(200, 180, 255, 160),
  };
  constexpr int kSectorColorCount =
      sizeof(kSectorColors) / sizeof(kSectorColors[0]);

  ImU32 selected_color = IM_COL32(255, 120, 120, 220);
  ImU32 hover_color = IM_COL32(255, 220, 80, 220);

  // Draw quads between consecutive segments (a_i, b_i, b_{i+1}, a_{i+1}),
  // colored per sector; a gate marked as a sector split ends its sector, so
  // the quad reaching it still uses the old color and the next one advances.
  if (state.segments.size() >= 2) {
    int sector = 0;
    for (int i = 0; i + 1 < (int)state.segments.size(); ++i) {
      ImVec2 a0 =
          WorldToScreen(state.segments[i].a, canvas_min, canvas_max, state);
      ImVec2 b0 =
          WorldToScreen(state.segments[i].b, canvas_min, canvas_max, state);
      ImVec2 a1 =
          WorldToScreen(state.segments[i + 1].a, canvas_min, canvas_max, state);
      ImVec2 b1 =
          WorldToScreen(state.segments[i + 1].b, canvas_min, canvas_max, state);
      ImVec2 poly[4] = {a0, b0, b1, a1};
      if (state.show_infill) {
        draw_list->AddConvexPolyFilled(
            poly, 4, kSectorColors[sector % kSectorColorCount]);
      }
      draw_list->AddPolyline(poly, 4, IM_COL32(40, 60, 80, 200), true, 2.0f);
      if (state.segments[i + 1].is_sector_split) {
        ++sector;
      }
    }

    if (state.track_closed) {
      // Wraparound quad joining the last gate back to the start/finish gate.
      int last = static_cast<int>(state.segments.size() - 1);
      ImVec2 a0 =
          WorldToScreen(state.segments[last].a, canvas_min, canvas_max, state);
      ImVec2 b0 =
          WorldToScreen(state.segments[last].b, canvas_min, canvas_max, state);
      ImVec2 a1 =
          WorldToScreen(state.segments[0].a, canvas_min, canvas_max, state);
      ImVec2 b1 =
          WorldToScreen(state.segments[0].b, canvas_min, canvas_max, state);
      ImVec2 poly[4] = {a0, b0, b1, a1};
      if (state.show_infill) {
        draw_list->AddConvexPolyFilled(
            poly, 4, kSectorColors[sector % kSectorColorCount]);
      }
      draw_list->AddPolyline(poly, 4, IM_COL32(40, 60, 80, 200), true, 2.0f);
    }
  }

  // Draw segment endpoints
  for (int i = 0; i < (int)state.segments.size(); ++i) {
    ImVec2 sa =
        WorldToScreen(state.segments[i].a, canvas_min, canvas_max, state);
    ImVec2 sb =
        WorldToScreen(state.segments[i].b, canvas_min, canvas_max, state);
    bool hovered = state.hovered_segment == i;
    ImU32 colorA = hovered ? hover_color
                   : (state.selected_segment == i && state.selected_end == 0)
                       ? selected_color
                       : IM_COL32(220, 240, 255, 220);
    ImU32 colorB = hovered ? hover_color
                   : (state.selected_segment == i && state.selected_end == 1)
                       ? selected_color
                       : IM_COL32(200, 220, 200, 220);
    bool bold = state.segments[i].is_sector_split;
    float radius = bold ? 8.0f : 6.0f;
    float outline_thickness = bold ? 3.0f : 2.0f;
    float line_thickness = bold ? 4.0f : 2.0f;
    draw_list->AddLine(sa, sb, IM_COL32(180, 200, 220, 160), line_thickness);
    draw_list->AddCircleFilled(sa, radius, colorA);
    draw_list->AddCircle(sa, radius + 0.5f, IM_COL32(60, 80, 100, 180), 12,
                         outline_thickness);
    draw_list->AddCircleFilled(sb, radius, colorB);
    draw_list->AddCircle(sb, radius + 0.5f, IM_COL32(60, 80, 100, 180), 12,
                         outline_thickness);
  }

  draw_list->AddRect(canvas_min, canvas_max, IM_COL32(255, 255, 255, 80), 0.0f,
                     0, 2.0f);
}

// Right column: satellite map, track overlay, and pan/zoom/edit input.
static void DrawMapCanvas(TrackState &state, const ImVec2 &canvas_size) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(12, 12, 18, 255));
  ImGui::BeginChild("map_canvas", canvas_size, true,
                    ImGuiWindowFlags_NoScrollbar);
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f);
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4.0f);

  ImVec2 canvas_min = ImGui::GetCursorScreenPos();
  ImVec2 canvas_max =
      ImVec2(canvas_min.x + canvas_size.x, canvas_min.y + canvas_size.y);
  ImDrawList *draw_list = ImGui::GetWindowDrawList();

  ImGui::Dummy(canvas_size);
  bool hovered = ImGui::IsItemHovered();

  pacer::RequestVisibleCanvasTiles(state.tiles, state.view, canvas_min,
                                   canvas_max);
  pacer::RenderCanvasTiles(state.tiles, state.view, draw_list, canvas_min,
                           canvas_max);

  HandleMapInput(state, canvas_min, canvas_max, hovered);
  DrawTrackOverlay(state, draw_list, canvas_min, canvas_max);

  ImGui::EndChild();
  ImGui::PopStyleColor();
}

static void ShowAnnotatorGui(TrackState &state) {
  state.tiles.ApplyResults();

  // Power save: idle slowly, but poll faster while tile downloads are in
  // flight -- the worker threads cannot wake the idle event loop, so late
  // tiles would otherwise only appear on the next slow idle frame.
  HelloImGui::GetRunnerParams()->fpsIdling.fpsIdle =
      (state.tiles.PendingCount() > 0) ? 30.f : 3.f;

  // Platform-native undo shortcut: Cmd+Z / Cmd+Shift+Z on macOS,
  // Ctrl+Z / Ctrl+Shift+Z everywhere else.
#ifdef __APPLE__
  constexpr ImGuiKeyChord kUndoMod = ImGuiMod_Super;
#else
  constexpr ImGuiKeyChord kUndoMod = ImGuiMod_Ctrl;
#endif
  if (!ImGui::GetIO().WantTextInput) {
    if (ImGui::IsKeyChordPressed(kUndoMod | ImGuiMod_Shift | ImGuiKey_Z)) {
      Redo(state);
    } else if (ImGui::IsKeyChordPressed(kUndoMod | ImGuiKey_Z)) {
      Undo(state);
    }
  }

  ImGui::Begin("Track Annotator");

  // Two-column layout: left = controls (~30%), right = map (~70%)
  float avail_w = ImGui::GetContentRegionAvail().x;
  float left_w = avail_w * 0.30f;
  ImGui::Columns(2, "main_columns", false);
  ImGui::SetColumnWidth(0, left_w);

  DrawControlPanel(state, left_w);
  ImGui::NextColumn();

  ImVec2 avail = ImGui::GetContentRegionAvail();
  float canvas_h = std::max(200.0f, avail.y);
  DrawMapCanvas(state, ImVec2(avail.x, canvas_h));

  ImGui::Columns(1);
  ImGui::End();
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::printf(
          "Usage: track_annotator [--lat LAT] [--lon LON] [--file PATH]\n"
          "  --lat LAT    initial map center latitude (default 51.376)\n"
          "  --lon LON    initial map center longitude (default -0.361)\n"
          "  --file PATH  track file to load on startup (if it exists) and\n"
          "               use as the default Save/Load path\n"
          "               (default track_annotation.json; the picker lists\n"
          "               tracks/*.json)\n");
      return 0;
    }
  }

  TrackState state;
  state.view.lat = 51.37600;
  state.view.lon = -0.36100;
  state.picker.path = "track_annotation.json";

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--lat" && i + 1 < argc) {
      state.view.lat = std::stod(argv[++i]);
    } else if (arg == "--lon" && i + 1 < argc) {
      state.view.lon = std::stod(argv[++i]);
    } else if (arg == "--file" && i + 1 < argc) {
      state.picker.path = argv[++i];
    }
  }

  if (std::filesystem::exists(state.picker.path)) {
    if (LoadState(state, state.picker.path)) {
      state.last_message = "Loaded state from '" + state.picker.path + "'";
    } else {
      state.last_message =
          "Unable to load state from '" + state.picker.path + "'";
    }
  }

  HelloImGui::RunnerParams runnerParams;
  runnerParams.appWindowParams.windowTitle = "Track Annotator";
  runnerParams.imGuiWindowParams.menuAppTitle = "Track Annotator";
  runnerParams.appWindowParams.windowGeometry.size = {1200, 900};
  runnerParams.appWindowParams.restorePreviousGeometry = true;
  runnerParams.imGuiWindowParams.showMenuBar = false;
  runnerParams.imGuiWindowParams.defaultImGuiWindowType =
      HelloImGui::DefaultImGuiWindowType::ProvideFullScreenDockSpace;
  // Power save: low idle frame rate (raised dynamically in ShowAnnotatorGui
  // while tiles download). The status bar exposes the idling state and an
  // on/off checkbox.
  runnerParams.fpsIdling.fpsIdle = 3.f;
  runnerParams.imGuiWindowParams.showStatusBar = true;
  runnerParams.iniFolderType = HelloImGui::IniFolderType::AppUserConfigFolder;
  runnerParams.iniFilename = "TrackAnnotator/track_annotator.ini";
  runnerParams.callbacks.ShowGui = [&state] { ShowAnnotatorGui(state); };
  HelloImGui::Run(runnerParams);
  return 0;
}
