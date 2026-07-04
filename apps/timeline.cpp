#include <algorithm>
#include <cctype>
#include <fstream>
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
#include <implot_internal.h>

#include <pacer/datatypes/datatypes.hpp>
#include <pacer/geometry/geometry.hpp>
#include <pacer/gps-source/gps-source.hpp>
#include <pacer/laps-display/laps-display.hpp>
#include <pacer/laps/laps.hpp>

using pacer::GPSSample;

static bool HasExtension(const std::string &filename, const std::string &ext) {
  if (filename.size() < ext.size())
    return false;
  std::string lower_ext = filename.substr(filename.size() - ext.size());
  std::transform(lower_ext.begin(), lower_ext.end(), lower_ext.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return lower_ext == ext;
}

static bool LoadLapsFromFiles(pacer::Laps *plaps,
                              const std::vector<std::string> &filenames,
                              std::string &message) {
  plaps->ClearPoints();
  std::vector<std::string> errors;
  bool loaded_any = false;
  double time_offset = 0.0;

  for (const auto &filename : filenames) {
    if (filename.empty())
      continue;
    std::ifstream file(filename);
    if (!file.is_open()) {
      errors.push_back(filename + ": not found");
      continue;
    }
    file.close();

    if (HasExtension(filename, ".dat")) {
      pacer::ReadDatFile(
          filename.c_str(),
          [&](pacer::GPSSample sample, double time) {
            plaps->AddPoint(sample, time + time_offset);
          },
          pacer::DatVersion::WITH_TIMESTAMP);
      loaded_any = true;
    } else {
      pacer::GPMFSource source(filename.c_str());
      source.Seek(0);
      int64_t file_start_timestamp_ms = 0;
      bool have_file_timestamp = false;
      double file_time_offset = 0.0;
      double last_timeline_time = time_offset;
      while (!source.IsEnd()) {
        auto [start, end] = source.CurrentTimeSpan();
        source.RawGPSSource::Samples(
            [&](pacer::GPSSample sample, size_t current, size_t total) {
              double timeline_time;
              if (sample.timestamp_ms != 0) {
                if (!have_file_timestamp) {
                  file_start_timestamp_ms = sample.timestamp_ms;
                  have_file_timestamp = true;
                }
                timeline_time =
                    time_offset + static_cast<double>(sample.timestamp_ms -
                                                      file_start_timestamp_ms) /
                                      1000.0;
              } else {
                timeline_time = time_offset + file_time_offset + start +
                                (end - start) * current / total;
              }
              plaps->AddPoint(sample, timeline_time);
              last_timeline_time = std::max(last_timeline_time, timeline_time);
            });
        if (!have_file_timestamp) {
          file_time_offset += (end - start);
        }
        source.Next();
      }
      time_offset =
          std::max(last_timeline_time, time_offset + file_time_offset);
      loaded_any = true;
    }
  }

  if (!loaded_any) {
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
            std::to_string(filenames.size()) + " files.";
  return true;
}

void ShowMenu(HelloImGui::RunnerParams &runnerParams) {
  if (ImGui::BeginMenu("My Menu")) {
    bool clicked = ImGui::MenuItem("Test me", "F", false);
    if (clicked) {
      HelloImGui::Log(HelloImGui::LogLevel::Warning, "It works");
    }
    ImGui::EndMenu();
  }
  HelloImGui::ShowViewMenu(runnerParams);
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
  pacer::Laps full_laps;

  std::vector<std::string> load_filenames = {""};
  std::string load_message = "Enter file paths and click Load files.";

  if (full_laps.PointCount() > 0)
    full_laps.sectors.start_line = full_laps.PickRandomStart();
  auto laps = full_laps;

  auto laps_display = pacer::LapsDisplay{&laps};
  pacer::DeltaLapsComparision delta;

  float duration = 0;
  if (laps.PointCount() > 0) {
    duration =
        laps.GetPoint(laps.PointCount() - 1).time - laps.GetPoint(0).time;
  }
  float current = 0;

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

  runnerParams.callbacks.ShowMenus = [&]() { ShowMenu(runnerParams); };

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
        if (LoadLapsFromFiles(&full_laps, load_filenames, load_message)) {
          laps_display.bounds = {{1.0, 1.0}, {0.0, 0.0}};
          delta.selected_laps.clear();
          full_laps.sectors.start_line = full_laps.PickRandomStart();
          laps = full_laps;
          laps_display.laps = &laps;
          laps_display.selected_lap = laps.LapsCount() > 0 ? 0 : -1;
          if (laps.PointCount() > 0) {
            duration = laps.GetPoint(laps.PointCount() - 1).time -
                       laps.GetPoint(0).time;
          } else {
            duration = 0;
          }
          current = 0;
          if (laps.LapsCount() > 0) {
            delta.reference_lap = laps.GetLap(laps_display.selected_lap);
          }
        }
      }
      if (!load_message.empty()) {
        ImGui::TextWrapped("%s", load_message.c_str());
      }
    }
    ImGui::End();
  };

  HelloImGui::DockableWindow dataSubsetWindow;
  dataSubsetWindow.label = "Data Subset";
  dataSubsetWindow.dockSpaceName = "LeftSpace";
  dataSubsetWindow.callBeginEnd = false;
  dataSubsetWindow.canBeClosed = false;
  dataSubsetWindow.GuiFunction = [&]() {
    static float start_p = 0, end_p = 0;
    float total_points = static_cast<float>(full_laps.PointCount());
    if (end_p != total_points) {
      end_p = total_points;
      start_p = 0;
    }
    start_p = std::clamp(start_p, 0.0f, total_points);
    end_p = std::clamp(end_p, start_p, total_points);

    if (ImGui::Begin("Data Subset")) {
      ImGui::Text("Select data subset to display on the map");
      ImGui::SetNextItemWidth(ImGui::GetWindowWidth() / 2);
      if (ImGui::SliderFloat("Start", &start_p, 0, end_p) ||
          (ImGui::SameLine(),
           ImGui::SliderFloat("End", &end_p, start_p, total_points))) {
        start_p = std::clamp(start_p, 0.0f, total_points);
        end_p = std::clamp(end_p, start_p, total_points);
        laps.ClearPoints();
        for (size_t i = static_cast<size_t>(start_p);
             i < static_cast<size_t>(end_p); ++i) {
          auto [gps, time] = full_laps.GetPoint(i);
          laps.AddPoint(gps, time);
        }
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
    std::vector<GPSSample> gps;
    gps.reserve(laps.PointCount());
    for (size_t i = 0; i < laps.PointCount(); ++i) {
      gps.push_back(laps.GetPoint(i).point);
    }
    if (ImGui::Begin("Map")) {
      if (ImPlot::BeginPlot("GPS", ImVec2(-1, -1), ImPlotFlags_Equal)) {
        laps_display.DisplayMap();
        auto getter = [](int index, void *data) {
          auto &[gps, ld] = *reinterpret_cast<
              std::pair<std::vector<GPSSample> &, pacer::LapsDisplay &> *>(
              data);
          return ld.ToImPlotPoint(gps[index]);
        };

        std::pair<std::vector<GPSSample> &, pacer::LapsDisplay &> data = {
            gps, laps_display};

        ImPlot::PlotScatterG("data", getter, &data, (int)gps.size());

        if (!gps.empty()) {
          std::stringstream ss;
          ss << "Speed: " << gps.back().full_speed * 3.6 << "km/h";
          auto point = laps_display.ToImPlotPoint(gps.back());
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
    static int old_selected_lap = laps_display.selected_lap;
    if (old_selected_lap != laps_display.selected_lap) {
      float width = delta.reference_lap.width;
      delta.reference_lap = laps.GetLap(laps_display.selected_lap);
      delta.reference_lap.width = width;
      old_selected_lap = laps_display.selected_lap;
    }

    if (ImGui::Begin("Laps")) {
      delta.DrawSlider();
      ImGui::SameLine();
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
      loadFilesWindow, dataSubsetWindow, mapWindow,         lapsWindow,
      lapChartWindow,  deltaWindow,      lapTelemetryWindow};

  runnerParams.callbacks.ShowGui = [&]() { laps.Update(); };

  HelloImGui::Run(runnerParams);

  ImPlot::DestroyContext(implotContext);

  return 0;
}