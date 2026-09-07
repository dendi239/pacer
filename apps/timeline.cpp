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

// Sensible default docking layout setup.
//
// Each dockspace holds one kind of view, so the windows a second source adds
// become tabs in the spaces that are already there instead of splitting the
// layout further.
HelloImGui::DockingParams CreateDefaultLayout() {
  HelloImGui::DockingParams result;
  result.dockingSplits = {
      // Split MainDockSpace to create LeftSpace on the left
      HelloImGui::DockingSplit{"MainDockSpace", "LeftSpace", ImGuiDir_Left,
                               0.23f},
      // Split remaining MainDockSpace to create RightSpace on the right
      HelloImGui::DockingSplit{"MainDockSpace", "RightSpace", ImGuiDir_Right,
                               0.30f},
      // Split remaining MainDockSpace (Center) to create BottomCenterSpace at
      // the bottom
      HelloImGui::DockingSplit{"MainDockSpace", "BottomCenterSpace",
                               ImGuiDir_Down, 0.35f},
      // Split LeftSpace to create LeftBottomSpace at the bottom
      HelloImGui::DockingSplit{"LeftSpace", "LeftBottomSpace", ImGuiDir_Down,
                               0.5f},
      // Split RightSpace to create RightBottomSpace at the bottom
      HelloImGui::DockingSplit{"RightSpace", "RightBottomSpace", ImGuiDir_Down,
                               0.5f}};
  return result;
}

// Main code
int main(int argc, char **argv) {
  pacer::Session session;
  pacer::Source *source = session.NewSource();

  pacer::TileStore tile_store;
  bool show_map_tiles = true;

  // Dev convenience: `timeline data.MP4 ... track.json --laps 3,5` loads
  // everything a manual session would click together: data files, the
  // reference track (any .json argument), and the delta lap selection.
  std::vector<int> preselected_laps;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--laps" && i + 1 < argc) {
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

  // Built after the CLI load so the view picks up the track's map frame.
  auto source_view = pacer::SourceView{source};
  pacer::DeltaLapsComparision delta;
  int synced_track_generation = -1;
  for (int lap : preselected_laps) {
    delta.selected_laps.insert(lap);
  }

  auto implotContext = ImPlot::CreateContext();

  HelloImGui::RunnerParams runnerParams;
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
  // ShowGui below raises this while tile downloads are in flight, since the
  // worker threads cannot wake the event loop themselves. The status bar
  // shows the idling state and a checkbox to turn it off.
  runnerParams.fpsIdling.fpsIdle = 3.f;
  runnerParams.imGuiWindowParams.showStatusBar = true;

  runnerParams.callbacks.ShowMenus = [&]() {
    HelloImGui::ShowViewMenu(runnerParams);
  };

  // Define GUI Dockable Windows
  HelloImGui::DockableWindow trackWindow;
  trackWindow.label = "Track";
  trackWindow.dockSpaceName = "LeftSpace";
  trackWindow.GuiFunction = [&]() { source_view.DrawTrackPanel(); };

  HelloImGui::DockableWindow filesWindow;
  filesWindow.label = "Files";
  filesWindow.dockSpaceName = "LeftSpace";
  filesWindow.GuiFunction = [&]() { source_view.DrawFilesPanel(); };

  HelloImGui::DockableWindow mapWindow;
  mapWindow.label = "Map";
  mapWindow.dockSpaceName = "MainDockSpace";
  mapWindow.GuiFunction = [&]() {
    pacer::LapsDisplay &display = source_view.display;
    ImGui::Checkbox("Show map", &show_map_tiles);
    if (ImPlot::BeginPlot("GPS", ImVec2(-1, -1), ImPlotFlags_Equal)) {
      display.SetupMap();
      if (show_map_tiles && display.HasMapFrame()) {
        pacer::PlotSatelliteTiles(tile_store, display.cs);
      }
      display.PlotMapItems();

      if (source->laps.PointCount() > 0) {
        auto last = source->laps.GetPoint(source->laps.PointCount() - 1);
        std::stringstream ss;
        ss << "Speed: " << last.full_speed * 3.6 << "km/h";
        auto point = display.ToImPlotPoint(last);
        ImPlot::PlotText(ss.str().data(), point[0], point[1]);
      }
      delta.PlotSticks();
      ImPlot::EndPlot();
    }
  };

  HelloImGui::DockableWindow lapsWindow;
  lapsWindow.label = "Laps";
  lapsWindow.dockSpaceName = "LeftBottomSpace";
  lapsWindow.GuiFunction = [&]() { source_view.DrawLapTablePanel(); };

  HelloImGui::DockableWindow lapChartWindow;
  lapChartWindow.label = "Lap chart";
  lapChartWindow.dockSpaceName = "BottomCenterSpace";
  lapChartWindow.GuiFunction = [&]() { source_view.DrawLapChartPanel(); };

  HelloImGui::DockableWindow deltaWindow;
  deltaWindow.label = "Delta";
  deltaWindow.dockSpaceName = "RightSpace";
  deltaWindow.GuiFunction = [&]() { delta.Display(source->laps); };

  // Map view of the delta comparison: the selected laps' trajectories over
  // the reference track, with hover markers synced to the Delta window.
  HelloImGui::DockableWindow comparisonMapWindow;
  comparisonMapWindow.label = "Comparison Map";
  comparisonMapWindow.dockSpaceName = "RightBottomSpace";
  comparisonMapWindow.GuiFunction = [&]() {
    if (delta.reference_track.segments.empty()) {
      ImGui::TextWrapped("Load a reference track to compare laps on the map.");
      return;
    }
    ImGui::Checkbox("Satellite", &delta.show_satellite);
    ImGui::SameLine();
    ImGui::Checkbox("Reference track", &delta.show_reference_track);
    if (ImPlot::BeginPlot("##comparison_map", ImVec2(-1, -1),
                          ImPlotFlags_Equal)) {
      delta.SetupComparisonMap();
      if (delta.show_satellite) {
        pacer::PlotSatelliteTiles(tile_store, delta.cs);
      }
      delta.PlotComparisonMap(source->laps);
      ImPlot::EndPlot();
    }
  };

  HelloImGui::DockableWindow lapTelemetryWindow;
  lapTelemetryWindow.label = "Lap Telemetry";
  lapTelemetryWindow.dockSpaceName = "RightBottomSpace";
  lapTelemetryWindow.GuiFunction = [&]() {
    source_view.display.DisplayLapTelemetry();
  };

  // Windows are docked in list order; the last one docked into a dockspace
  // becomes its selected tab, so Comparison Map goes after Lap Telemetry.
  runnerParams.dockingParams.dockableWindows = {
      trackWindow,    filesWindow,        lapsWindow,         mapWindow,
      lapChartWindow, deltaWindow,        lapTelemetryWindow,
      comparisonMapWindow};

  runnerParams.callbacks.ShowGui = [&]() {
    source_view.Update();
    // The comparison keeps its own copy of the track (it will adopt one from
    // the laps dropped into it once comparisons are first-class); re-take it
    // whenever the source's track changes.
    if (synced_track_generation != source->track_generation) {
      synced_track_generation = source->track_generation;
      delta.SetReferenceTrack(source->track);
    }
    // Drain finished downloads even when the Map window is not drawn, so
    // PendingCount() falls back to zero and the idle rate can drop again.
    tile_store.ApplyResults();
    HelloImGui::GetRunnerParams()->fpsIdling.fpsIdle =
        (tile_store.PendingCount() > 0) ? 30.f : 3.f;
  };

  HelloImGui::Run(runnerParams);

  ImPlot::DestroyContext(implotContext);

  return 0;
}
