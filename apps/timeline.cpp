#include <algorithm>
#include <format>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <hello_imgui/docking_params.h>
#include <hello_imgui/hello_imgui.h>
#include <hello_imgui/runner_callbacks.h>
#include <hello_imgui/runner_params.h>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <implot.h>

#include <pacer/datatypes/datatypes.hpp>
#include <pacer/geometry/geometry.hpp>
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

// A window's ImGui identity: everything after "###". Stays put while the
// visible part of the label follows the source's name, so renaming a source
// relabels its tabs without ImGui treating them as new windows (and losing
// where they were docked).
std::string PanelWindowId(const char *panel, int source_id) {
  return std::format("{}_src{}", panel, source_id);
}

std::string PanelWindowLabel(const char *panel, const std::string &source_name,
                             int source_id) {
  return std::format("{} — {}###{}", panel, source_name,
                     PanelWindowId(panel, source_id));
}

struct TimelineApp {
  HelloImGui::RunnerParams *params = nullptr;

  pacer::Session session;
  std::vector<std::unique_ptr<pacer::SourceView>> views;
  pacer::TileStore tile_store;

  // Until comparisons are first-class, the single delta view follows the
  // active source.
  pacer::DeltaLapsComparision delta;
  int active_source_id = -1;
  int delta_source_id = -1;
  int delta_track_generation = -1;

  bool show_map_tiles = true;

  // Structural edits are deferred to the top of the next frame: adding or
  // removing a source moves the window list and destroys GuiFunctions that
  // the frame being drawn is still walking.
  bool want_new_source = false;
  int want_remove_source = -1;

  //------------------------------- SOURCES ---------------------------------//

  pacer::SourceView *ViewFor(int source_id) {
    for (auto &view : views) {
      if (view->source->id == source_id)
        return view.get();
    }
    return nullptr;
  }

  pacer::SourceView *ActiveView() { return ViewFor(active_source_id); }

  /// Creates a source and its view, without touching the window list.
  pacer::SourceView *CreateSource() {
    pacer::Source *source = session.NewSource();
    views.push_back(std::make_unique<pacer::SourceView>(source));
    active_source_id = source->id;
    return views.back().get();
  }

  void AddSourceWindows(pacer::SourceView &view, bool run_time) {
    for (const PanelSpec &panel : kSourcePanels) {
      HelloImGui::DockableWindow window;
      window.label =
          PanelWindowLabel(panel.name, view.source->name, view.source->id);
      window.dockSpaceName = panel.dock_space;
      // The grouped View menu below replaces the flat list hello_imgui
      // would otherwise build, which grows unusable at one entry per panel
      // per source.
      window.includeInViewMenu = false;
      int source_id = view.source->id;
      int panel_index = (int)(&panel - kSourcePanels);
      window.GuiFunction = [this, source_id, panel_index]() {
        DrawSourcePanel(source_id, panel_index);
      };
      if (run_time) {
        HelloImGui::AddDockableWindow(window, /*forceDockspace=*/true);
      } else {
        params->dockingParams.dockableWindows.push_back(window);
      }
    }
  }

  void RemoveSourceWindows(int source_id) {
    for (const PanelSpec &panel : kSourcePanels) {
      const std::string id = PanelWindowId(panel.name, source_id);
      for (const auto &window : params->dockingParams.dockableWindows) {
        if (window.label.ends_with("###" + id)) {
          HelloImGui::RemoveDockableWindow(window.label);
          break;
        }
      }
    }
  }

  /// Re-labels a source's windows in place. The "###" identity is untouched,
  /// so the tabs keep their docking and their ImGui state.
  void RelabelSourceWindows(const pacer::Source &source) {
    for (const PanelSpec &panel : kSourcePanels) {
      const std::string id = PanelWindowId(panel.name, source.id);
      for (auto &window : params->dockingParams.dockableWindows) {
        if (window.label.ends_with("###" + id)) {
          window.label = PanelWindowLabel(panel.name, source.name, source.id);
          break;
        }
      }
    }
  }

  void ApplyPendingEdits() {
    if (want_new_source) {
      want_new_source = false;
      AddSourceWindows(*CreateSource(), /*run_time=*/true);
    }
    if (want_remove_source >= 0) {
      int source_id = want_remove_source;
      want_remove_source = -1;
      RemoveSourceWindows(source_id);
      // The windows are removed on the next PreNewFrame, before anything is
      // drawn, so the view backing their GuiFunctions can go now.
      std::erase_if(views, [&](const std::unique_ptr<pacer::SourceView> &view) {
        return view->source->id == source_id;
      });
      session.Remove(source_id);
      if (active_source_id == source_id) {
        active_source_id = views.empty() ? -1 : views.front()->source->id;
      }
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
    if (view.source->id == delta_source_id) {
      delta.PlotSticks();
    }
    ImPlot::EndPlot();
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
        RelabelSourceWindows(source);
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
        MenuItemForWindow(panel.name, PanelWindowId(panel.name, source.id));
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Make active", nullptr,
                          source.id == active_source_id)) {
        active_source_id = source.id;
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

  void NewFrame() {
    ApplyPendingEdits();
    for (auto &view : views) {
      view->Update();
    }

    // The comparison keeps its own copy of the track; re-take it when the
    // active source changes or its track is reloaded.
    pacer::SourceView *active = ActiveView();
    if (active && (active->source->id != delta_source_id ||
                   active->source->track_generation !=
                       delta_track_generation)) {
      delta_source_id = active->source->id;
      delta_track_generation = active->source->track_generation;
      delta.SetReferenceTrack(active->source->track);
      delta.selected_laps.clear();
    }

    // Drain finished downloads even when no Map window is drawn, so
    // PendingCount() falls back to zero and the idle rate can drop again.
    tile_store.ApplyResults();
    params->fpsIdling.fpsIdle = (tile_store.PendingCount() > 0) ? 30.f : 3.f;

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N)) {
      want_new_source = true;
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
  // from the shell the same way they would be from the Sources panel.
  std::vector<int> preselected_laps;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--source") {
      source = app.CreateSource()->source;
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

  HelloImGui::DockableWindow deltaWindow;
  deltaWindow.label = "Delta";
  deltaWindow.dockSpaceName = "RightSpace";
  deltaWindow.GuiFunction = [&]() {
    pacer::SourceView *view = app.ActiveView();
    if (!view) {
      ImGui::TextWrapped("No source selected.");
      return;
    }
    app.delta.Display(view->source->laps);
  };

  // Map view of the delta comparison: the selected laps' trajectories over
  // the reference track, with hover markers synced to the Delta window.
  HelloImGui::DockableWindow comparisonMapWindow;
  comparisonMapWindow.label = "Comparison Map";
  comparisonMapWindow.dockSpaceName = "CompMapSpace";
  comparisonMapWindow.GuiFunction = [&]() {
    pacer::SourceView *view = app.ActiveView();
    if (!view || app.delta.reference_track.segments.empty()) {
      ImGui::TextWrapped("Load a reference track to compare laps on the map.");
      return;
    }
    ImGui::Checkbox("Satellite", &app.delta.show_satellite);
    ImGui::SameLine();
    ImGui::Checkbox("Reference track", &app.delta.show_reference_track);
    if (ImPlot::BeginPlot("##comparison_map", ImVec2(-1, -1),
                          ImPlotFlags_Equal)) {
      app.delta.SetupComparisonMap();
      if (app.delta.show_satellite) {
        pacer::PlotSatelliteTiles(app.tile_store, app.delta.cs);
      }
      app.delta.PlotComparisonMap(view->source->laps);
      ImPlot::EndPlot();
    }
  };

  HelloImGui::DockableWindow lapTelemetryWindow;
  lapTelemetryWindow.label = "Lap Telemetry";
  lapTelemetryWindow.dockSpaceName = "CompMapSpace";
  lapTelemetryWindow.GuiFunction = [&]() {
    if (pacer::SourceView *view = app.ActiveView()) {
      view->display.DisplayLapTelemetry();
    }
  };

  runnerParams.dockingParams.dockableWindows = {sourcesWindow, deltaWindow,
                                                lapTelemetryWindow,
                                                comparisonMapWindow};
  // The startup sources' windows go into the initial list rather than
  // through AddDockableWindow, so the default layout places them on frame
  // one; sources added later go through AddDockableWindow instead.
  for (auto &view : app.views) {
    app.AddSourceWindows(*view, /*run_time=*/false);
  }

  runnerParams.callbacks.ShowGui = [&]() { app.NewFrame(); };

  HelloImGui::Run(runnerParams);

  ImPlot::DestroyContext(implotContext);

  return 0;
}
