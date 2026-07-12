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
#include <pacer/gps-source/gps-source.hpp>
#include <pacer/laps-display/laps-display.hpp>
#include <pacer/laps/laps.hpp>
#include <pacer/map-tiles/implot-tiles.hpp>
#include <pacer/map-tiles/tile-store.hpp>

using pacer::GPSSample;

static bool LoadLapsFromFiles(pacer::Laps *plaps,
                              const std::vector<std::string> &filenames,
                              std::string &message) {
  plaps->ClearPoints();
  std::vector<std::string> errors;
  size_t loaded_files = pacer::LoadGPSFiles(
      filenames, [&](GPSSample sample) { plaps->AddPoint(sample); }, &errors);

  if (loaded_files == 0) {
    message = "No files loaded.";
    if (!errors.empty())
      message += " " + errors.front();
    return false;
  }

  if (!errors.empty()) {
    message = "Loaded files with errors: ";
    for (size_t i = 0; i < errors.size(); ++i) {
      if (i > 0)
        message += "; ";
      message += errors[i];
    }
    return true;
  }

  message = "Loaded " + std::to_string(plaps->PointCount()) + " points from " +
            std::to_string(loaded_files) + " files.";
  return true;
}

// Sensible default docking layout setup
// We split the screen into Left column (controls), Center (Map/Chart), and
// Right column (Delta/Telemetry)
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
int main(int, char **) {
  pacer::Laps laps;

  std::vector<std::string> load_filenames = {""};
  std::string load_message = "Enter file paths and click Load files.";

  auto laps_display = pacer::LapsDisplay{&laps};
  pacer::DeltaLapsComparision delta;
  pacer::TileStore tile_store;

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

  runnerParams.imGuiWindowParams.showMenuBar = true;
  runnerParams.imGuiWindowParams.showMenu_App = false;
  runnerParams.imGuiWindowParams.showMenu_View = false;

  runnerParams.callbacks.ShowMenus = [&]() {
    HelloImGui::ShowViewMenu(runnerParams);
  };

  // Define GUI Dockable Windows
  HelloImGui::DockableWindow loadFilesWindow;
  loadFilesWindow.label = "Load Data Files";
  loadFilesWindow.dockSpaceName = "LeftSpace";
  loadFilesWindow.callBeginEnd = false;
  loadFilesWindow.canBeClosed = false;
  loadFilesWindow.GuiFunction = [&]() {
    if (ImGui::Begin("Load Data Files")) {
      ImGui::Text("Data files to load:");
      ImGui::SameLine();
      if (ImGui::Button("+")) {
        load_filenames.emplace_back("");
      }
      for (int i = 0; i < (int)load_filenames.size(); ++i) {
        ImGui::PushID(i);
        ImGui::SetNextItemWidth(ImGui::GetWindowWidth() - 120);
        ImGui::InputText("File", &load_filenames[i]);
        ImGui::SameLine();
        if (ImGui::Button("Remove") && load_filenames.size() > 1) {
          load_filenames.erase(load_filenames.begin() + i);
          ImGui::PopID();
          break;
        }
        ImGui::PopID();
      }
      if (ImGui::Button("Load files")) {
        if (LoadLapsFromFiles(&laps, load_filenames, load_message)) {
          laps_display.bounds = {{1.0, 1.0}, {0.0, 0.0}};
          laps_display.selected_lap = -1;
          delta.selected_laps.clear();
          if (delta.reference_track.segments.empty()) {
            laps.sectors = pacer::Sectors{};
          } else {
            // The map frame is supplied by the reference track, so it
            // survives a data reload; re-apply the sectors in that frame.
            laps.sectors =
                delta.reference_track.BuildSectors(delta.reference_track.cs);
          }
        }
      }
      if (!load_message.empty()) {
        ImGui::TextWrapped("%s", load_message.c_str());
      }
    }
    ImGui::End();
  };

  HelloImGui::DockableWindow mapWindow;
  mapWindow.label = "Map";
  mapWindow.dockSpaceName = "MainDockSpace";
  mapWindow.callBeginEnd = false;
  mapWindow.canBeClosed = false;
  mapWindow.GuiFunction = [&]() {
    if (ImGui::Begin("Map")) {
      if (ImPlot::BeginPlot("GPS", ImVec2(-1, -1), ImPlotFlags_Equal)) {
        laps_display.SetupMap();
        if (laps_display.HasMapFrame()) {
          pacer::PlotSatelliteTiles(tile_store, laps_display.cs);
        }
        laps_display.PlotMapItems();

        if (laps.PointCount() > 0) {
          auto last = laps.GetPoint(laps.PointCount() - 1);
          std::stringstream ss;
          ss << "Speed: " << last.full_speed * 3.6 << "km/h";
          auto point = laps_display.ToImPlotPoint(last);
          ImPlot::PlotText(ss.str().data(), point[0], point[1]);
        }
        delta.PlotSticks();
        ImPlot::EndPlot();
      }
    }
    ImGui::End();
  };

  HelloImGui::DockableWindow lapsWindow;
  lapsWindow.label = "Laps";
  lapsWindow.dockSpaceName = "LeftBottomSpace";
  lapsWindow.callBeginEnd = false;
  lapsWindow.canBeClosed = false;
  lapsWindow.GuiFunction = [&]() {
    delta.cs = laps_display.cs;

    if (ImGui::Begin("Laps")) {
      delta.DrawReferenceTrackLoader(laps, laps_display);
      if (laps.PointCount() > 0 && laps.LapsCount() == 0) {
        ImGui::TextWrapped(
            "Load a reference track to split the data into laps and sectors.");
      }
      laps_display.DisplayTable();
    }
    ImGui::End();
  };

  HelloImGui::DockableWindow lapChartWindow;
  lapChartWindow.label = "Lap chart";
  lapChartWindow.dockSpaceName = "BottomCenterSpace";
  lapChartWindow.callBeginEnd = false;
  lapChartWindow.canBeClosed = false;
  lapChartWindow.GuiFunction = [&]() {
    if (ImGui::Begin("Lap chart")) {
      static float lap_cutoff = 107;
      ImGui::SliderFloat("Cutoff", &lap_cutoff, 100, 125);

      if (ImPlot::BeginPlot("Lap time chart", ImVec2(-1, -1),
                            ImPlotFlags_NoTitle)) {
        float best_lap = 1e9f;
        for (size_t i = 1; i < laps.LapsCount(); ++i) {
          float t = laps.LapTime(i);
          if (t > 1.0f && t < best_lap) {
            best_lap = t;
          }
        }
        if (best_lap == 1e9f) {
          best_lap = (laps.LapsCount() > 0) ? laps.LapTime(0) : 0.0f;
        }

        std::tuple<pacer::Laps &, float, float &> data{laps, best_lap,
                                                       lap_cutoff};

        auto getter = [](int index, void *data) {
          auto &[laps, best, cutoff] =
              *reinterpret_cast<std::tuple<pacer::Laps &, float, float &> *>(
                  data);
          auto lap = laps.GetLap(index);
          float time = lap.LapTime();
          if (time > cutoff * best / 100 || time < best)
            time = NAN;
          return ImPlotPoint((float)index, time);
        };

        ImPlot::PlotLineG("Lap Time", getter, &data, laps.LapsCount());
        ImPlot::PlotScatterG("Lap Time", getter, &data, laps.LapsCount());
        ImPlot::EndPlot();
      }
    }
    ImGui::End();
  };

  HelloImGui::DockableWindow deltaWindow;
  deltaWindow.label = "Delta";
  deltaWindow.dockSpaceName = "RightSpace";
  deltaWindow.callBeginEnd = false;
  deltaWindow.canBeClosed = false;
  deltaWindow.GuiFunction = [&]() {
    if (ImGui::Begin("Delta")) {
      delta.Display(laps);
    }
    ImGui::End();
  };

  HelloImGui::DockableWindow lapTelemetryWindow;
  lapTelemetryWindow.label = "Lap Telemetry";
  lapTelemetryWindow.dockSpaceName = "RightBottomSpace";
  lapTelemetryWindow.callBeginEnd = false;
  lapTelemetryWindow.canBeClosed = false;
  lapTelemetryWindow.GuiFunction = [&]() {
    if (ImGui::Begin("Lap Telemetry")) {
      laps_display.DisplayLapTelemetry();
    }
    ImGui::End();
  };

  runnerParams.dockingParams.dockableWindows = {
      loadFilesWindow, mapWindow,   lapsWindow,
      lapChartWindow,  deltaWindow, lapTelemetryWindow};

  runnerParams.callbacks.ShowGui = [&]() { laps.Update(); };

  HelloImGui::Run(runnerParams);

  ImPlot::DestroyContext(implotContext);

  return 0;
}
