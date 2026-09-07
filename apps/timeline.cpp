#include <algorithm>
#include <format>
#include <filesystem>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <hello_imgui/docking_params.h>
#include <hello_imgui/hello_imgui.h>
#include <hello_imgui/runner_callbacks.h>
#include <hello_imgui/runner_params.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <implot.h>

#include <pacer/datatypes/datatypes.hpp>
#include <pacer/geometry/geometry.hpp>
#include <pacer/comparison-view/comparison-view.hpp>
#include <pacer/laps-display/laps-display.hpp>
#include <pacer/map-tiles/implot-tiles.hpp>
#include <pacer/map-tiles/tile-store.hpp>
#include <pacer/session/session.hpp>
#include <pacer/source-view/source-view.hpp>
#include <pacer/ui/theme.hpp>

namespace {

// One dockspace per kind of view, so the windows a second source brings
// become tabs next to the first source's -- adding a source never splits the
// layout further.
//
//   +----------------+---------------------------+---------------+
//   | Sources        |  Map (S1) | Map (S2)       | Delta         |
//   +----------------+                            |               |
//   | Track (S1|S2)  |                            +---------------+
//   +----------------+---------------------------+ Comparison Map|
//   | Files (S1|S2)  | Lap chart    | Lap table   | Lap Telemetry |
//   +----------------+--------------+-------------+---------------+
HelloImGui::DockingParams CreateDefaultLayout() {
  HelloImGui::DockingParams result;
  result.dockingSplits = {
      HelloImGui::DockingSplit{"MainDockSpace", "LeftSpace", ImGuiDir_Left,
                               0.23f},
      HelloImGui::DockingSplit{"MainDockSpace", "RightSpace", ImGuiDir_Right,
                               0.30f},
      // What is left of MainDockSpace is the map.
      HelloImGui::DockingSplit{"MainDockSpace", "DataSpace", ImGuiDir_Down,
                               0.35f},
      HelloImGui::DockingSplit{"DataSpace", "TableSpace", ImGuiDir_Right, 0.5f},
      // The top of LeftSpace stays the source list.
      HelloImGui::DockingSplit{"LeftSpace", "TrackSpace", ImGuiDir_Down, 0.72f},
      HelloImGui::DockingSplit{"TrackSpace", "FilesSpace", ImGuiDir_Down, 0.6f},
      HelloImGui::DockingSplit{"RightSpace", "CompMapSpace", ImGuiDir_Down,
                               0.5f}};
  return result;
}

// The per-source panels, in the order they are created. Kept as data so the
// windows, the View menu and the teardown all walk the same list.
struct PanelSpec {
  const char *name;
  const char *dock_space;
};

constexpr PanelSpec kSourcePanels[] = {
    {"Track", "TrackSpace"},     {"Files", "FilesSpace"},
    {"Map", "MainDockSpace"},    {"Samples", "MainDockSpace"},
    {"Lap chart", "DataSpace"},  {"Lap table", "TableSpace"},
};

// The panels a comparison brings, same idea.
constexpr PanelSpec kComparisonPanels[] = {
    {"Delta", "RightSpace"},
    {"Comparison map", "CompMapSpace"},
};

// A window's ImGui identity: everything after "###". Stays put while the
// visible part of the label follows the owner's name, so renaming a source
// or comparison relabels its tabs without ImGui treating them as new
// windows (and losing where they were docked). `owner` distinguishes the
// source and comparison id spaces.
std::string PanelWindowId(const char *panel, const char *owner, int id) {
  return std::format("{}_{}{}", panel, owner, id);
}

std::string PanelWindowLabel(const char *panel, const char *owner, int id,
                             const std::string &name) {
  return std::format("{} — {}###{}", panel, name,
                     PanelWindowId(panel, owner, id));
}

// Finds the live DockableWindow whose identity is `id`, or nullptr.
HelloImGui::DockableWindow *
FindWindow(HelloImGui::RunnerParams *params, const std::string &id) {
  for (auto &window : params->dockingParams.dockableWindows) {
    if (window.label.ends_with("###" + id))
      return &window;
  }
  return nullptr;
}

struct TimelineApp {
  HelloImGui::RunnerParams *params = nullptr;

  pacer::Session session;
  std::vector<std::unique_ptr<pacer::SourceView>> views;
  std::vector<std::unique_ptr<pacer::ComparisonView>> comparison_views;
  pacer::TileStore tile_store;

  int active_source_id = -1;

  bool show_map_tiles = true;

  // Structural edits are deferred to the top of the next frame: adding or
  // removing a source moves the window list and destroys GuiFunctions that
  // the frame being drawn is still walking.
  bool want_new_source = false;
  int want_remove_source = -1;
  bool want_new_comparison = false;
  int want_remove_comparison = -1;
  /// Session file to open at the top of the next frame; opening one tears
  /// down every window the current session owns.
  std::string want_open_session;
  /// Set once that teardown has been requested, and loaded a frame later.
  /// A restored source keeps its saved id, so its windows carry the same
  /// ImGui identities as the ones being torn down -- and hello_imgui only
  /// drops the old ones on the next PreNewFrame. Adding the new ones in the
  /// same frame leaves two windows sharing an identity, and some of them
  /// come back floating instead of docked.
  std::string opening_session;

  /// Windows added at runtime, waiting to be docked beside a sibling of the
  /// same kind: {new label, sibling label}. hello_imgui's own
  /// AddDockableWindow docking only lands reliably in MainDockSpace here --
  /// windows bound for the other dockspaces come up floating -- and docking
  /// beside the panel of the same kind that is already open is what the
  /// layout wants anyway: a second source's lap chart belongs next to the
  /// first source's, wherever the user has since moved it.
  std::vector<std::pair<std::string, std::string>> pending_dock;

  /// Counts down to the layout rebuild a session open asks for; 0 when
  /// none is pending. See OpenSession for why it is delayed.
  int frames_until_layout_reset_ = 0;
  std::string session_path = "session.json";
  std::string session_status;
  /// Lap to put into a comparison created by dropping it on "New
  /// comparison": the drop and the creation are a frame apart.
  pacer::LapRef pending_drop;

  //------------------------------- SOURCES ---------------------------------//

  pacer::SourceView *ViewFor(int source_id) {
    for (auto &view : views) {
      if (view->source->id == source_id)
        return view.get();
    }
    return nullptr;
  }

  pacer::SourceView *ActiveView() { return ViewFor(active_source_id); }

  pacer::ComparisonView *ComparisonViewFor(int comparison_id) {
    for (auto &view : comparison_views) {
      if (view->comparison->id == comparison_id)
        return view.get();
    }
    return nullptr;
  }

  pacer::ComparisonView *CreateComparison() {
    pacer::Comparison *comparison = session.NewComparison();
    comparison_views.push_back(
        std::make_unique<pacer::ComparisonView>(comparison));
    return comparison_views.back().get();
  }

  /// Creates a source and its view, without touching the window list.
  pacer::SourceView *CreateSource() {
    pacer::Source *source = session.NewSource();
    views.push_back(std::make_unique<pacer::SourceView>(source));
    active_source_id = source->id;
    return views.back().get();
  }

  /// Creates the windows for one owner (a source or a comparison), one per
  /// entry of `panels`. At startup they go straight into the initial list so
  /// the default layout places them; later they arrive through hello_imgui's
  /// runtime path instead.
  template <size_t N>
  void AddWindows(const PanelSpec (&panels)[N], const char *owner, int id,
                  const std::string &name, bool run_time,
                  const std::function<void(int, int)> &draw) {
    for (size_t i = 0; i < N; ++i) {
      HelloImGui::DockableWindow window;
      window.label = PanelWindowLabel(panels[i].name, owner, id, name);
      window.dockSpaceName = panels[i].dock_space;
      // The grouped View menu below replaces the flat list hello_imgui
      // would otherwise build, which grows unusable at one entry per panel
      // per source.
      window.includeInViewMenu = false;
      int panel_index = (int)i;
      window.GuiFunction = [draw, id, panel_index]() { draw(id, panel_index); };
      if (run_time) {
        // Find a window of the same kind that is already open, to dock
        // beside once hello_imgui has created this one.
        for (const auto &sibling : params->dockingParams.dockableWindows) {
          if (sibling.label.starts_with(std::string(panels[i].name) + " —") &&
              sibling.label != window.label) {
            pending_dock.push_back({window.label, sibling.label});
            break;
          }
        }
        HelloImGui::AddDockableWindow(window, /*forceDockspace=*/true);
      } else {
        params->dockingParams.dockableWindows.push_back(window);
      }
    }
  }

  template <size_t N>
  void RemoveWindows(const PanelSpec (&panels)[N], const char *owner, int id) {
    for (size_t i = 0; i < N; ++i) {
      if (auto *window =
              FindWindow(params, PanelWindowId(panels[i].name, owner, id))) {
        HelloImGui::RemoveDockableWindow(window->label);
      }
    }
  }

  /// Re-labels an owner's windows in place. The "###" identity is untouched,
  /// so the tabs keep their docking and their ImGui state.
  template <size_t N>
  void RelabelWindows(const PanelSpec (&panels)[N], const char *owner, int id,
                      const std::string &name) {
    for (size_t i = 0; i < N; ++i) {
      if (auto *window =
              FindWindow(params, PanelWindowId(panels[i].name, owner, id))) {
        window->label = PanelWindowLabel(panels[i].name, owner, id, name);
      }
    }
  }

  void AddSourceWindows(pacer::SourceView &view, bool run_time) {
    AddWindows(kSourcePanels, "src", view.source->id, view.source->name,
               run_time,
               [this](int id, int panel) { DrawSourcePanel(id, panel); });
  }

  void AddComparisonWindows(pacer::ComparisonView &view, bool run_time) {
    AddWindows(kComparisonPanels, "cmp", view.comparison->id,
               view.comparison->name, run_time,
               [this](int id, int panel) { DrawComparisonPanel(id, panel); });
  }

  void ApplyPendingEdits() {
    if (want_new_source) {
      want_new_source = false;
      AddSourceWindows(*CreateSource(), /*run_time=*/true);
    }
    if (want_remove_source >= 0) {
      int source_id = want_remove_source;
      want_remove_source = -1;
      RemoveWindows(kSourcePanels, "src", source_id);
      // The windows are removed on the next PreNewFrame, before anything is
      // drawn, so the view backing their GuiFunctions can go now.
      std::erase_if(views, [&](const std::unique_ptr<pacer::SourceView> &view) {
        return view->source->id == source_id;
      });
      // Session::Remove also drops that source's laps from every
      // comparison, so their views have to redo their resampling.
      session.Remove(source_id);
      for (auto &view : comparison_views) {
        view->Invalidate();
      }
      if (active_source_id == source_id) {
        active_source_id = views.empty() ? -1 : views.front()->source->id;
      }
    }
    if (want_new_comparison) {
      want_new_comparison = false;
      pacer::ComparisonView *view = CreateComparison();
      if (pending_drop.Valid()) {
        session.AddLap(view->comparison, pending_drop);
        pending_drop = {};
        view->Invalidate();
      }
      AddComparisonWindows(*view, /*run_time=*/true);
    }
    if (want_remove_comparison >= 0) {
      int comparison_id = want_remove_comparison;
      want_remove_comparison = -1;
      RemoveWindows(kComparisonPanels, "cmp", comparison_id);
      std::erase_if(comparison_views,
                    [&](const std::unique_ptr<pacer::ComparisonView> &view) {
                      return view->comparison->id == comparison_id;
                    });
      session.RemoveComparison(comparison_id);
    }
    if (!opening_session.empty()) {
      OpenSession(std::exchange(opening_session, {}));
    } else if (!want_open_session.empty()) {
      CloseSession();
      opening_session = std::exchange(want_open_session, {});
    }
  }

  /// Drops every window the current session owns. The windows themselves go
  /// on the next PreNewFrame, which is why loading waits a frame.
  void CloseSession() {
    for (auto &view : views) {
      RemoveWindows(kSourcePanels, "src", view->source->id);
    }
    for (auto &view : comparison_views) {
      RemoveWindows(kComparisonPanels, "cmp", view->comparison->id);
    }
    views.clear();
    comparison_views.clear();
    active_source_id = -1;
  }

  /// Reads a session and gives every restored source and comparison its
  /// windows back. The restored ids are the saved ones, so the windows come
  /// back with the identities the layout file remembers and land where they
  /// were.
  void OpenSession(const std::string &path) {
    try {
      session.LoadFromFile(path);
    } catch (const std::exception &e) {
      session_status = std::string("Open failed: ") + e.what();
      // The old session's views are already gone, so leave an empty one
      // rather than a half-torn-down window set.
      session.sources.clear();
      session.comparisons.clear();
      AddSourceWindows(*CreateSource(), /*run_time=*/true);
      return;
    }

    session_path = path;
    for (auto &source : session.sources) {
      views.push_back(std::make_unique<pacer::SourceView>(source.get()));
      AddSourceWindows(*views.back(), /*run_time=*/true);
    }
    for (auto &comparison : session.comparisons) {
      comparison_views.push_back(
          std::make_unique<pacer::ComparisonView>(comparison.get()));
      AddComparisonWindows(*comparison_views.back(), /*run_time=*/true);
    }
    if (views.empty()) {
      AddSourceWindows(*CreateSource(), /*run_time=*/true);
    } else {
      active_source_id = views.front()->source->id;
    }
    // Opening a session replaces every window, so there is no arrangement
    // left to preserve -- and rebuilding the layout is the one placement
    // mechanism that reliably reaches every dockspace. It has to wait until
    // the added windows are actually in dockingParams.dockableWindows,
    // which hello_imgui does two PreNewFrames from here; a reset before
    // that rebuilds the layout without them and leaves them floating.
    frames_until_layout_reset_ = 3;
    session_status = std::format("Opened {} ({} sources, {} comparisons).",
                                 path, views.size(), comparison_views.size());
  }

  void SaveSession() {
    try {
      session.SaveToFile(session_path);
      session_status = std::format("Saved {}.", session_path);
    } catch (const std::exception &e) {
      session_status = std::string("Save failed: ") + e.what();
    }
  }

  //-------------------------------- PANELS ---------------------------------//

  /// `panel_index` indexes kSourcePanels. A removed source's windows live
  /// one more frame than its view does (hello_imgui removes them on the next
  /// PreNewFrame), so a missing view is expected, not an error.
  void DrawSourcePanel(int source_id, int panel_index) {
    pacer::SourceView *view = ViewFor(source_id);
    if (!view)
      return;
    // Working in a source's window is what makes it the active one, so the
    // comparison views follow along without a separate click.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
      active_source_id = source_id;
    }

    switch (panel_index) {
    case 0:
      view->DrawTrackPanel();
      break;
    case 1:
      view->DrawFilesPanel();
      break;
    case 2:
      DrawMapPanel(*view);
      break;
    case 3:
      view->DrawSamplesPanel();
      break;
    case 4:
      view->DrawLapChartPanel();
      break;
    case 5:
      view->DrawLapTablePanel();
      break;
    }
  }

  void DrawMapPanel(pacer::SourceView &view) {
    pacer::LapsDisplay &display = view.display;
    pacer::Laps &laps = view.source->laps;

    ImGui::Checkbox("Show map", &show_map_tiles);
    if (!ImPlot::BeginPlot("GPS", ImVec2(-1, -1), ImPlotFlags_Equal))
      return;

    display.SetupMap();
    if (show_map_tiles && display.HasMapFrame()) {
      pacer::PlotSatelliteTiles(tile_store, display.cs);
    }
    display.PlotMapItems();

    if (laps.PointCount() > 0) {
      auto last = laps.GetPoint(laps.PointCount() - 1);
      std::stringstream ss;
      ss << "Speed: " << last.full_speed * 3.6 << "km/h";
      auto point = display.ToImPlotPoint(last);
      ImPlot::PlotText(ss.str().data(), point[0], point[1]);
    }
    view.PlotTrackGates();
    ImPlot::EndPlot();
  }

  /// `panel_index` indexes kComparisonPanels; see DrawSourcePanel on why a
  /// missing view is expected rather than an error.
  void DrawComparisonPanel(int comparison_id, int panel_index) {
    pacer::ComparisonView *view = ComparisonViewFor(comparison_id);
    if (!view)
      return;

    if (panel_index == 0) {
      view->Display(session);
      return;
    }

    if (!view->comparison->HasTrack()) {
      ImGui::TextWrapped("Drop a lap into this comparison to see it on the "
                         "map.");
      return;
    }
    ImGui::Checkbox("Satellite", &view->show_satellite);
    ImGui::SameLine();
    ImGui::Checkbox("Reference track", &view->show_reference_track);
    if (ImPlot::BeginPlot("##comparison_map", ImVec2(-1, -1),
                          ImPlotFlags_Equal)) {
      view->SetupComparisonMap();
      if (view->show_satellite) {
        pacer::PlotSatelliteTiles(tile_store, view->cs);
      }
      view->PlotComparisonMap(session);
      ImPlot::EndPlot();
    }
  }

  /// Accepts a lap dropped on the item just submitted. Returns the dropped
  /// lap, or an invalid LapRef. `reason` explains a refused drop.
  pacer::LapRef AcceptLapDrop(const std::string &reason) {
    pacer::LapRef dropped;
    if (!ImGui::BeginDragDropTarget())
      return dropped;
    if (const ImGuiPayload *payload =
            ImGui::AcceptDragDropPayload(pacer::kLapDragPayload,
                                         reason.empty()
                                             ? 0
                                             : ImGuiDragDropFlags_AcceptBeforeDelivery |
                                                   ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
      if (reason.empty()) {
        dropped = *reinterpret_cast<const pacer::LapRef *>(payload->Data);
      } else {
        // Refused: say why rather than swallowing the drop silently.
        ImGui::SetTooltip("Can't add: %s", reason.c_str());
      }
    }
    ImGui::EndDragDropTarget();
    return dropped;
  }

  void DrawComparisonsPanel() {
    if (ImGui::Button("New comparison")) {
      want_new_comparison = true;
    }

    for (auto &view : comparison_views) {
      pacer::Comparison &comparison = *view->comparison;
      ImGui::PushID(comparison.id);

      ImGui::SetNextItemWidth(-60);
      if (ImGui::InputText("##name", &comparison.name)) {
        RelabelWindows(kComparisonPanels, "cmp", comparison.id,
                       comparison.name);
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Close")) {
        want_remove_comparison = comparison.id;
      }

      // The row is a drop target too, so laps can be filed into a
      // comparison without its own window being visible.
      ImGui::Indent();
      std::string summary =
          comparison.laps.empty()
              ? std::string("empty — drop a lap here")
              : std::format("{} laps · {}", comparison.laps.size(),
                            std::filesystem::path(comparison.track_path)
                                .stem()
                                .string());
      ImGui::Selectable(summary.c_str(), false, 0,
                        ImVec2(ImGui::GetContentRegionAvail().x, 0));
      if (pacer::LapRef dropped = AcceptLapDrop(
              PendingDropReason(comparison));
          dropped.Valid()) {
        session.AddLap(&comparison, dropped);
        view->Invalidate();
      }
      ImGui::Unindent();

      ImGui::PopID();
    }

    // Dropping onto empty space below the list starts a new comparison with
    // that lap already in it -- the shortest path from "this lap looks
    // interesting" to a delta.
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x,
                        std::max(ImGui::GetContentRegionAvail().y,
                                 ImGui::GetTextLineHeight() * 2)));
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload *payload =
              ImGui::AcceptDragDropPayload(pacer::kLapDragPayload)) {
        pending_drop = *reinterpret_cast<const pacer::LapRef *>(payload->Data);
        want_new_comparison = true;
      }
      ImGui::EndDragDropTarget();
    }
    if (comparison_views.empty()) {
      ImVec2 start = ImGui::GetCursorStartPos();
      ImGui::SetCursorPos(
          ImVec2(start.x, start.y + ImGui::GetTextLineHeightWithSpacing() * 2));
      ImGui::TextWrapped("Drop a lap here to start a comparison.");
    }
  }

  /// Why the lap currently being dragged could not join `comparison`, or
  /// empty. Peeks at the in-flight payload, since the reason has to be
  /// known before the drop is accepted.
  std::string PendingDropReason(const pacer::Comparison &comparison) {
    const ImGuiPayload *payload = ImGui::GetDragDropPayload();
    if (!payload || !payload->IsDataType(pacer::kLapDragPayload))
      return {};
    return session.WhyNotAddable(
        comparison, *reinterpret_cast<const pacer::LapRef *>(payload->Data));
  }

  void DrawSourcesPanel() {
    if (ImGui::Button("New source")) {
      want_new_source = true;
    }

    for (auto &view : views) {
      pacer::Source &source = *view->source;
      ImGui::PushID(source.id);

      bool active = source.id == active_source_id;
      if (ImGui::Selectable(std::format("F{}", source.id).c_str(), active,
                            ImGuiSelectableFlags_AllowOverlap,
                            ImVec2(28, 0))) {
        active_source_id = source.id;
      }
      ImGui::SameLine();

      ImGui::SetNextItemWidth(-60);
      if (ImGui::InputText("##name", &source.name)) {
        RelabelWindows(kSourcePanels, "src", source.id, source.name);
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Close")) {
        want_remove_source = source.id;
      }

      ImGui::Indent();
      ImGui::TextDisabled("%zu files, %zu samples, %zu laps",
                          source.files.size(), source.UsedSampleCount(),
                          source.LapsCount());
      ImGui::Unindent();

      ImGui::PopID();
    }

    if (views.empty()) {
      ImGui::TextWrapped("No sources. Add one to load a recording.");
    }
  }

  //--------------------------------- MENUS ---------------------------------//

  void DrawFileMenu() {
    if (!ImGui::BeginMenu("File"))
      return;
    if (ImGui::MenuItem("New source", "Ctrl+N")) {
      want_new_source = true;
    }
    if (ImGui::MenuItem("New comparison", "Ctrl+Shift+N")) {
      want_new_comparison = true;
    }
    ImGui::Separator();

    // No native file dialog here, so the path is typed. A session file
    // records the setup -- paths, trims, tracks, which laps each comparison
    // holds -- and re-reads the recordings on open.
    ImGui::SetNextItemWidth(280);
    ImGui::InputText("##session_path", &session_path);
    if (ImGui::MenuItem("Open session", "Ctrl+O") && !session_path.empty()) {
      want_open_session = session_path;
    }
    if (ImGui::MenuItem("Save session", "Ctrl+S") && !session_path.empty()) {
      SaveSession();
    }
    if (!session_status.empty()) {
      ImGui::TextDisabled("%s", session_status.c_str());
    }

    ImGui::Separator();
    if (ImGui::MenuItem("Quit")) {
      params->appShallExit = true;
    }
    ImGui::EndMenu();
  }

  /// Toggles the window whose ImGui identity is `id`, if it is there.
  void MenuItemForWindow(const char *label, const std::string &id) {
    for (auto &window : params->dockingParams.dockableWindows) {
      if (!window.label.ends_with("###" + id))
        continue;
      if (ImGui::MenuItem(label, nullptr, window.isVisible)) {
        window.isVisible = !window.isVisible;
      }
      return;
    }
    ImGui::BeginDisabled();
    ImGui::MenuItem(label, nullptr, false);
    ImGui::EndDisabled();
  }

  void DrawViewMenu() {
    if (!ImGui::BeginMenu("View"))
      return;

    // The app-wide windows first, then one submenu per source, so the menu
    // stays the same shape however many sources are open.
    for (auto &window : params->dockingParams.dockableWindows) {
      if (!window.includeInViewMenu)
        continue;
      if (ImGui::MenuItem(window.label.c_str(), nullptr, window.isVisible)) {
        window.isVisible = !window.isVisible;
      }
    }

    if (!views.empty()) {
      ImGui::SeparatorText("Sources");
    }
    for (auto &view : views) {
      const pacer::Source &source = *view->source;
      if (!ImGui::BeginMenu(source.name.c_str()))
        continue;
      for (const PanelSpec &panel : kSourcePanels) {
        MenuItemForWindow(panel.name,
                          PanelWindowId(panel.name, "src", source.id));
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Make active", nullptr,
                          source.id == active_source_id)) {
        active_source_id = source.id;
      }
      ImGui::EndMenu();
    }

    if (!comparison_views.empty()) {
      ImGui::SeparatorText("Comparisons");
    }
    for (auto &view : comparison_views) {
      const pacer::Comparison &comparison = *view->comparison;
      if (!ImGui::BeginMenu(comparison.name.c_str()))
        continue;
      for (const PanelSpec &panel : kComparisonPanels) {
        MenuItemForWindow(panel.name,
                          PanelWindowId(panel.name, "cmp", comparison.id));
      }
      ImGui::EndMenu();
    }

    ImGui::Separator();
    if (ImGui::MenuItem("Restore default layout")) {
      params->dockingParams.layoutReset = true;
    }
    ImGui::MenuItem("Status bar", nullptr,
                    &params->imGuiWindowParams.showStatusBar);
    ImGui::EndMenu();
  }

  //--------------------------------- FRAME ---------------------------------//

  /// Docks each freshly added window into the node its sibling of the same
  /// kind occupies. Both windows have to exist in ImGui first, which is a
  /// frame or two after AddDockableWindow was called; entries whose sibling
  /// has since gone are dropped rather than retried forever.
  void ApplyPendingDocking() {
    std::erase_if(pending_dock, [](const auto &entry) {
      ImGuiWindow *sibling = ImGui::FindWindowByName(entry.second.c_str());
      if (!sibling)
        return true; // the sibling closed; leave the new window where it is
      if (!ImGui::FindWindowByName(entry.first.c_str()))
        return false; // not created yet
      if (sibling->DockId == 0)
        return true; // the sibling is floating, so there is nothing to join
      ImGui::DockBuilderDockWindow(entry.first.c_str(), sibling->DockId);
      return true;
    });
  }

  void NewFrame() {
    ApplyPendingEdits();
    for (auto &view : views) {
      view->Update();
    }
    for (auto &view : comparison_views) {
      view->Update();
    }

    ApplyPendingDocking();

    if (frames_until_layout_reset_ > 0 && --frames_until_layout_reset_ == 0) {
      params->dockingParams.layoutReset = true;
    }

    // Drain finished downloads even when no Map window is drawn, so
    // PendingCount() falls back to zero and the idle rate can drop again.
    tile_store.ApplyResults();
    params->fpsIdling.fpsIdle = (tile_store.PendingCount() > 0) ? 30.f : 3.f;

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift |
                                 ImGuiKey_N)) {
      want_new_comparison = true;
    } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N)) {
      want_new_source = true;
    } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
      SaveSession();
    } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) {
      want_open_session = session_path;
    }
  }
};

} // namespace

int main(int argc, char **argv) {
  TimelineApp app;
  pacer::Source *source = app.CreateSource()->source;

  // Dev convenience: `timeline data.MP4 ... track.json --laps 3,5` loads
  // everything a manual session would click together: data files, the
  // reference track (any .json argument), and the delta lap selection.
  // `--source` starts a further source, so two recordings can be set up
  // from the shell the same way they would be from the Sources panel, and
  // `--session file.json` reopens a saved one.
  std::vector<int> preselected_laps;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--source") {
      source = app.CreateSource()->source;
    } else if (arg == "--session" && i + 1 < argc) {
      // Deferred to the first frame so it goes through exactly the same
      // teardown-and-rebuild the File menu does.
      app.want_open_session = argv[++i];
    } else if (arg == "--laps" && i + 1 < argc) {
      std::stringstream ss(argv[++i]);
      for (std::string id; std::getline(ss, id, ',');) {
        preselected_laps.push_back(std::stoi(id));
      }
    } else if (arg.ends_with(".json")) {
      source->LoadTrack(arg);
    } else {
      source->AddFile(arg);
    }
  }
  // The views were built before the CLI load, so hand them their frames.
  for (auto &view : app.views) {
    view->AdoptTrack();
  }
  // `--laps` seeds a comparison from the last source named on the command
  // line, which is what the flag has always meant. A `--session` load
  // replaces all of this on the first frame.
  if (!preselected_laps.empty()) {
    app.session.Update();
    pacer::ComparisonView *comparison = app.CreateComparison();
    for (int lap : preselected_laps) {
      app.session.AddLap(comparison->comparison,
                         {.source_id = source->id, .lap_index = lap});
    }
    comparison->Update();
  }

  auto implotContext = ImPlot::CreateContext();

  HelloImGui::RunnerParams runnerParams;
  app.params = &runnerParams;
  runnerParams.iniFolderType = HelloImGui::IniFolderType::TempFolder;

  runnerParams.appWindowParams.windowTitle = "Pacer";
  runnerParams.imGuiWindowParams.menuAppTitle = "Pacer";
  runnerParams.appWindowParams.windowGeometry.size = {1200, 1000};
  runnerParams.appWindowParams.restorePreviousGeometry = true;

  runnerParams.imGuiWindowParams.defaultImGuiWindowType =
      HelloImGui::DefaultImGuiWindowType::ProvideFullScreenDockSpace;
  runnerParams.dockingParams = CreateDefaultLayout();

  // Follow the host's dark/light setting instead of a theme of our own, and
  // don't let the .ini restore a stale one over it. ImPlot needs nothing
  // here: its default style is ImPlot::StyleColorsAuto, which derives plot
  // colors from the ImGui style we just swapped.
  runnerParams.imGuiWindowParams.rememberTheme = false;
  runnerParams.callbacks.PreNewFrame = [] { pacer::FollowSystemTheme(); };

  runnerParams.imGuiWindowParams.showMenuBar = true;
  runnerParams.imGuiWindowParams.showMenu_App = false;
  runnerParams.imGuiWindowParams.showMenu_View = false;

  // Power save: idle at a low frame rate when there is no interaction.
  // NewFrame raises this while tile downloads are in flight, since the
  // worker threads cannot wake the event loop themselves. The status bar
  // shows the idling state and a checkbox to turn it off.
  runnerParams.fpsIdling.fpsIdle = 3.f;
  runnerParams.imGuiWindowParams.showStatusBar = true;

  runnerParams.callbacks.ShowMenus = [&]() {
    app.DrawFileMenu();
    app.DrawViewMenu();
  };

  HelloImGui::DockableWindow sourcesWindow;
  sourcesWindow.label = "Sources";
  sourcesWindow.dockSpaceName = "LeftSpace";
  sourcesWindow.GuiFunction = [&]() { app.DrawSourcesPanel(); };

  HelloImGui::DockableWindow comparisonsWindow;
  comparisonsWindow.label = "Comparisons";
  comparisonsWindow.dockSpaceName = "LeftSpace";
  comparisonsWindow.GuiFunction = [&]() { app.DrawComparisonsPanel(); };

  HelloImGui::DockableWindow lapTelemetryWindow;
  lapTelemetryWindow.label = "Lap Telemetry";
  lapTelemetryWindow.dockSpaceName = "CompMapSpace";
  lapTelemetryWindow.GuiFunction = [&]() {
    if (pacer::SourceView *view = app.ActiveView()) {
      view->display.DisplayLapTelemetry();
    }
  };

  runnerParams.dockingParams.dockableWindows = {
      sourcesWindow, comparisonsWindow, lapTelemetryWindow};
  // The startup sources' and comparisons' windows go into the initial list
  // rather than through AddDockableWindow, so the default layout places them
  // on frame one; anything added later goes through AddDockableWindow.
  for (auto &view : app.views) {
    app.AddSourceWindows(*view, /*run_time=*/false);
  }
  for (auto &view : app.comparison_views) {
    app.AddComparisonWindows(*view, /*run_time=*/false);
  }

  runnerParams.callbacks.ShowGui = [&]() { app.NewFrame(); };

  HelloImGui::Run(runnerParams);

  ImPlot::DestroyContext(implotContext);

  return 0;
}
