#include <algorithm>
#include <array>
#include <format>
#include <filesystem>
#include <functional>
#include <map>
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
#include <pacer/ui/file-dialog.hpp>
#include <pacer/ui/theme.hpp>

namespace {

// Sessions are JSON, and the dialogs say so rather than showing the whole
// disk.
const std::vector<pacer::FileDialogFilter> kSessionFilters = {
    {"Sessions", {"json"}},
};

// Everything about one source, or one comparison, lives in that owner's own
// window: its panels are docked inside it, and the actions that apply to it
// are in its menu bar. So the app-level layout is only ever "which sources
// and comparisons are on screen", however many panels each of them shows.
//
//   +-- Source 1 -- Source 2 --------------+-- Comparison 1 -------------+
//   | Source  Track  Files  View           | Comparison  Laps  View      |
//   +--------+-----------------------------+-----------------------------+
//   | Track  |  Map | Samples              |  Delta        |  Map        |
//   | -------|                             |               |             |
//   | Files  +--------------+--------------+               |             |
//   |        | Lap chart    | Lap table    |               |             |
//   +--------+--------------+--------------+---------------+-------------+
HelloImGui::DockingParams CreateDefaultLayout() {
  HelloImGui::DockingParams result;
  // Sources on the left, comparisons on the right: the arrangement you want
  // while reading a delta against the run it came from.
  result.dockingSplits = {
      HelloImGui::DockingSplit{"MainDockSpace", "ComparisonSpace",
                               ImGuiDir_Right, 0.42f}};
  return result;
}

//------------------------------- PANEL LISTS -------------------------------//
//
// The panels each kind of owner brings, in the order the View menu lists
// them. Kept as data so the windows, the menu and the nested default layout
// all walk the same list.

constexpr const char *kSourcePanels[] = {
    "Map", "Samples", "Lap chart", "Lap table", "Telemetry", "Track", "Files",
};
constexpr const char *kComparisonPanels[] = {"Delta", "Map"};

// Fits both lists; OwnerUi keeps its visibility flags inline rather than
// heap-allocating one vector<bool> per source.
constexpr size_t kMaxPanels = 8;

// Owner kinds, as they appear in window identities. Sources and comparisons
// number from 1 independently, so the kind is part of the identity.
constexpr const char *kSourceKind = "src";
constexpr const char *kComparisonKind = "cmp";

// A window's ImGui identity: everything after "###". Stays put while the
// visible part of the label follows the owner's name, so renaming a source
// relabels its window without ImGui treating it as a new one (and losing
// where it was docked).
std::string HostWindowId(const char *kind, int id) {
  return std::format("host_{}{}", kind, id);
}

std::string HostWindowLabel(const char *kind, int id,
                            const std::string &name) {
  return std::format("{}###{}", name, HostWindowId(kind, id));
}

std::string PanelWindowId(const char *panel, const char *kind, int id) {
  return std::format("{}_{}{}", panel, kind, id);
}

// Panels are titled by the panel alone: the window they are docked in
// already says which source they belong to.
std::string PanelWindowLabel(const char *panel, const char *kind, int id) {
  return std::format("{}###{}", panel, PanelWindowId(panel, kind, id));
}

// Finds the live DockableWindow whose identity is `id`, or nullptr.
HelloImGui::DockableWindow *FindWindow(HelloImGui::RunnerParams *params,
                                       const std::string &id) {
  for (auto &window : params->dockingParams.dockableWindows) {
    if (window.label.ends_with("###" + id))
      return &window;
  }
  return nullptr;
}

/// The window state of one owner: which of its panels are open, and the
/// nested dockspace they are docked into.
struct OwnerUi {
  std::array<bool, kMaxPanels> visible{};

  /// The owner's dockspace, as hashed inside its host window. Zero until the
  /// host has been drawn once, which is also when there is nothing to keep
  /// alive.
  ImGuiID dockspace_id = 0;

  /// Set while drawing the host window, cleared at the top of every frame.
  /// False means the host is closed, collapsed, or an unselected tab -- in
  /// which case the panels are not drawn, and the node is only kept alive so
  /// they stay docked in it.
  bool dockspace_live = false;

  /// Rebuild the nested layout from scratch on the next draw, ignoring
  /// whatever the .ini restored. Set for a brand new owner and by
  /// "Reset panel layout".
  bool rebuild_layout = false;
};

struct TimelineApp {
  HelloImGui::RunnerParams *params = nullptr;

  pacer::Session session;
  std::vector<std::unique_ptr<pacer::SourceView>> views;
  std::vector<std::unique_ptr<pacer::ComparisonView>> comparison_views;
  pacer::TileStore tile_store;

  /// Keyed by owner id. Kept beside the views rather than inside them: it is
  /// the app that owns windows, the views only draw into them.
  std::map<int, OwnerUi> source_ui;
  std::map<int, OwnerUi> comparison_ui;

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
  /// A restored source keeps its saved id, so its window carries the same
  /// ImGui identity as the one being torn down -- and hello_imgui only drops
  /// the old one on the next PreNewFrame. Adding the new one in the same
  /// frame leaves two windows sharing an identity, and some of them come
  /// back floating instead of docked.
  std::string opening_session;

  /// Set by a session open, and honoured once every restored window has
  /// actually arrived. See OpenSession.
  bool want_layout_reset_ = false;
  std::string session_path = "session.json";
  std::string session_status;

  //-------------------------------- LOOKUP ---------------------------------//

  pacer::SourceView *ViewFor(int source_id) {
    for (auto &view : views) {
      if (view->source->id == source_id)
        return view.get();
    }
    return nullptr;
  }

  pacer::ComparisonView *ComparisonViewFor(int comparison_id) {
    for (auto &view : comparison_views) {
      if (view->comparison->id == comparison_id)
        return view.get();
    }
    return nullptr;
  }

  //-------------------------------- OWNERS ---------------------------------//

  /// A new owner's panels all start open: they arrive tabbed and docked by
  /// BuildSourceLayout, so "all of them" is a readable window rather than a
  /// wall of floating ones.
  static OwnerUi NewOwnerUi(size_t panel_count) {
    OwnerUi ui;
    ui.visible.fill(false);
    for (size_t i = 0; i < panel_count; ++i)
      ui.visible[i] = true;
    ui.rebuild_layout = true;
    return ui;
  }

  /// Creates a source and its view, without touching the window list.
  pacer::SourceView *CreateSource() {
    pacer::Source *source = session.NewSource();
    views.push_back(std::make_unique<pacer::SourceView>(source));
    source_ui[source->id] = NewOwnerUi(std::size(kSourcePanels));
    return views.back().get();
  }

  pacer::ComparisonView *CreateComparison() {
    pacer::Comparison *comparison = session.NewComparison();
    comparison_views.push_back(
        std::make_unique<pacer::ComparisonView>(comparison));
    comparison_ui[comparison->id] = NewOwnerUi(std::size(kComparisonPanels));
    return comparison_views.back().get();
  }

  //------------------------------ HOST WINDOWS -----------------------------//

  /// Adds the one dockable window an owner gets. At startup it goes straight
  /// into the initial list so the default layout places it; later it arrives
  /// through hello_imgui's runtime path instead.
  void AddHostWindow(const char *kind, int id, const std::string &name,
                     const char *dock_space, bool run_time,
                     const std::function<void()> &draw) {
    HelloImGui::DockableWindow window;
    window.label = HostWindowLabel(kind, id, name);
    window.dockSpaceName = dock_space;
    // The window hosts its own menu bar: the panels inside it are reached
    // from there, not from an app-wide list that grows one entry per panel
    // per source.
    window.imGuiWindowFlags = ImGuiWindowFlags_MenuBar;
    window.includeInViewMenu = false;
    window.GuiFunction = draw;
    if (run_time) {
      HelloImGui::AddDockableWindow(window, /*forceDockspace=*/true);
    } else {
      params->dockingParams.dockableWindows.push_back(window);
    }
  }

  void AddSourceWindow(pacer::SourceView &view, bool run_time) {
    int id = view.source->id;
    AddHostWindow(kSourceKind, id, view.source->name, "MainDockSpace",
                  run_time, [this, id] { DrawSourceHost(id); });
  }

  void AddComparisonWindow(pacer::ComparisonView &view, bool run_time) {
    int id = view.comparison->id;
    AddHostWindow(kComparisonKind, id, view.comparison->name,
                  "ComparisonSpace", run_time,
                  [this, id] { DrawComparisonHost(id); });
  }

  void RemoveHostWindow(const char *kind, int id) {
    if (auto *window = FindWindow(params, HostWindowId(kind, id))) {
      HelloImGui::RemoveDockableWindow(window->label);
    }
  }

  /// Re-labels an owner's window in place. The "###" identity is untouched,
  /// so the tab keeps its docking and its ImGui state.
  void RelabelHostWindow(const char *kind, int id, const std::string &name) {
    if (auto *window = FindWindow(params, HostWindowId(kind, id))) {
      window->label = HostWindowLabel(kind, id, name);
    }
  }

  void ApplyPendingEdits() {
    if (want_new_source) {
      want_new_source = false;
      AddSourceWindow(*CreateSource(), /*run_time=*/true);
    }
    if (want_remove_source >= 0) {
      int source_id = want_remove_source;
      want_remove_source = -1;
      RemoveHostWindow(kSourceKind, source_id);
      // The window is removed on the next PreNewFrame, before anything is
      // drawn, so the view backing its GuiFunction can go now.
      std::erase_if(views, [&](const std::unique_ptr<pacer::SourceView> &view) {
        return view->source->id == source_id;
      });
      source_ui.erase(source_id);
      // Session::Remove also drops that source's laps from every
      // comparison, so their views have to redo their resampling.
      session.Remove(source_id);
      for (auto &view : comparison_views) {
        view->Invalidate();
      }
    }
    if (want_new_comparison) {
      want_new_comparison = false;
      AddComparisonWindow(*CreateComparison(), /*run_time=*/true);
    }
    if (want_remove_comparison >= 0) {
      int comparison_id = want_remove_comparison;
      want_remove_comparison = -1;
      RemoveHostWindow(kComparisonKind, comparison_id);
      std::erase_if(comparison_views,
                    [&](const std::unique_ptr<pacer::ComparisonView> &view) {
                      return view->comparison->id == comparison_id;
                    });
      comparison_ui.erase(comparison_id);
      session.RemoveComparison(comparison_id);
    }
    if (!opening_session.empty()) {
      OpenSession(std::exchange(opening_session, {}));
    } else if (!want_open_session.empty()) {
      CloseSession();
      opening_session = std::exchange(want_open_session, {});
    }
  }

  //------------------------------- SESSIONS --------------------------------//

  /// Drops every window the current session owns. The windows themselves go
  /// on the next PreNewFrame, which is why loading waits a frame.
  void CloseSession() {
    for (auto &view : views) {
      RemoveHostWindow(kSourceKind, view->source->id);
    }
    for (auto &view : comparison_views) {
      RemoveHostWindow(kComparisonKind, view->comparison->id);
    }
    views.clear();
    comparison_views.clear();
    source_ui.clear();
    comparison_ui.clear();
  }

  /// Reads a session and gives every restored source and comparison its
  /// window back. The restored ids are the saved ones, so the windows come
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
      AddSourceWindow(*CreateSource(), /*run_time=*/true);
      return;
    }

    session_path = path;
    for (auto &source : session.sources) {
      views.push_back(std::make_unique<pacer::SourceView>(source.get()));
      source_ui[source->id] = NewOwnerUi(std::size(kSourcePanels));
      AddSourceWindow(*views.back(), /*run_time=*/true);
    }
    for (auto &comparison : session.comparisons) {
      comparison_views.push_back(
          std::make_unique<pacer::ComparisonView>(comparison.get()));
      comparison_ui[comparison->id] =
          NewOwnerUi(std::size(kComparisonPanels));
      AddComparisonWindow(*comparison_views.back(), /*run_time=*/true);
    }
    if (views.empty()) {
      AddSourceWindow(*CreateSource(), /*run_time=*/true);
    }
    // Opening a session replaces every window, so there is no arrangement
    // left to preserve -- and rebuilding the layout is the one placement
    // mechanism that reliably reaches every dockspace. It has to wait for
    // the restored windows to exist: a reset that runs before them lays the
    // dockspaces out without them, and they stay wherever they were added
    // instead -- comparisons tabbed in among the sources.
    want_layout_reset_ = true;
    session_status = std::format("Opened {} ({} sources, {} comparisons).",
                                 path, views.size(), comparison_views.size());
  }

  /// Asks for a session file and queues it for opening. A no-op if the
  /// user cancels, or where there is no dialog to put up (the menu offers a
  /// path field there instead).
  void PromptOpenSession() {
    std::string path = pacer::OpenFileDialog("Open a session",
                                             kSessionFilters, session_path);
    if (!path.empty()) {
      want_open_session = path;
    }
  }

  /// Asks where to write the session, and saves there. The chosen path
  /// becomes the one plain Ctrl+S writes to from then on.
  void PromptSaveSessionAs() {
    std::string path = pacer::SaveFileDialog("Save the session as",
                                             kSessionFilters, session_path);
    if (!path.empty()) {
      session_path = path;
      SaveSession();
    }
  }

  void SaveSession() {
    try {
      session.SaveToFile(session_path);
      session_status = std::format("Saved {}.", session_path);
    } catch (const std::exception &e) {
      session_status = std::string("Save failed: ") + e.what();
    }
  }

  //--------------------------- NESTED DOCKSPACES ---------------------------//

  /// Submits the owner's dockspace, building its default arrangement the
  /// first time (or when the ini has none to restore). Call from inside the
  /// host window; `build` receives the root node and docks the panels into
  /// it. Returns the dockspace id.
  ImGuiID DrawOwnerDockspace(OwnerUi &ui,
                             const std::function<void(ImGuiID)> &build) {
    ImGuiID dockspace_id = ImGui::GetID("panels");
    ui.dockspace_id = dockspace_id;
    ImVec2 size = ImGui::GetContentRegionAvail();
    if (ui.rebuild_layout || !ImGui::DockBuilderGetNode(dockspace_id)) {
      // The splits are ratios of the node the builder is handed, and ImGui
      // keeps each child's size across later resizes rather than its share
      // -- so a layout built against a placeholder size stays wrong. The
      // frame hello_imgui first renders a runtime-added window in, it is a
      // bare offscreen dummy; wait for a frame where the host is really on
      // screen, however many that takes.
      if (size.x < 200 || size.y < 150) {
        ui.rebuild_layout = true;
      } else {
        ui.rebuild_layout = false;
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, size);
        build(dockspace_id);
        ImGui::DockBuilderFinish(dockspace_id);
      }
    }
    ImGui::DockSpace(dockspace_id);
    ui.dockspace_live = true;
    return dockspace_id;
  }

  /// The setup column on the left, the map above the lap results, and the
  /// two views of the same thing (map/samples, lap table/telemetry) tabbed
  /// together.
  void BuildSourceLayout(ImGuiID root, int id) {
    ImGuiID center = root;
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.28f,
                                               nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.38f,
                                                 nullptr, &center);
    ImGuiID bottom_right = ImGui::DockBuilderSplitNode(
        bottom, ImGuiDir_Right, 0.5f, nullptr, &bottom);
    ImGuiID left_bottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down,
                                                      0.55f, nullptr, &left);

    auto dock = [&](const char *panel, ImGuiID node) {
      ImGui::DockBuilderDockWindow(
          PanelWindowLabel(panel, kSourceKind, id).c_str(), node);
    };
    dock("Map", center);
    dock("Samples", center);
    dock("Lap chart", bottom);
    dock("Lap table", bottom_right);
    dock("Telemetry", bottom_right);
    dock("Track", left);
    dock("Files", left_bottom);
  }

  void BuildComparisonLayout(ImGuiID root, int id) {
    ImGuiID left = root;
    ImGuiID right = ImGui::DockBuilderSplitNode(left, ImGuiDir_Right, 0.45f,
                                                nullptr, &left);
    auto dock = [&](const char *panel, ImGuiID node) {
      ImGui::DockBuilderDockWindow(
          PanelWindowLabel(panel, kComparisonKind, id).c_str(), node);
    };
    dock("Delta", left);
    dock("Map", right);
  }

  //------------------------------ HOST WINDOWS -----------------------------//

  void DrawSourceHost(int source_id) {
    pacer::SourceView *view = ViewFor(source_id);
    if (!view)
      return;
    OwnerUi &ui = source_ui[source_id];

    if (ImGui::BeginMenuBar()) {
      if (ImGui::BeginMenu("Source")) {
        ImGui::TextDisabled("Name");
        ImGui::SetNextItemWidth(200);
        if (ImGui::InputText("##name", &view->source->name)) {
          RelabelHostWindow(kSourceKind, source_id, view->source->name);
        }
        ImGui::Separator();
        ImGui::TextDisabled("%zu files, %zu samples, %zu laps",
                            view->source->files.size(),
                            view->source->UsedSampleCount(),
                            view->source->LapsCount());
        ImGui::Separator();
        if (ImGui::MenuItem("New source", "Ctrl+N")) {
          want_new_source = true;
        }
        if (ImGui::MenuItem("Close source")) {
          want_remove_source = source_id;
        }
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("Track")) {
        view->DrawTrackMenu();
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("Files")) {
        view->DrawFilesMenu();
        ImGui::EndMenu();
      }
      DrawPanelsMenu(ui, kSourcePanels, std::size(kSourcePanels));
      ImGui::EndMenuBar();
    }

    DrawOwnerDockspace(
        ui, [this, source_id](ImGuiID root) {
          BuildSourceLayout(root, source_id);
        });
  }

  void DrawComparisonHost(int comparison_id) {
    pacer::ComparisonView *view = ComparisonViewFor(comparison_id);
    if (!view)
      return;
    OwnerUi &ui = comparison_ui[comparison_id];

    if (ImGui::BeginMenuBar()) {
      if (ImGui::BeginMenu("Comparison")) {
        ImGui::TextDisabled("Name");
        ImGui::SetNextItemWidth(200);
        if (ImGui::InputText("##name", &view->comparison->name)) {
          RelabelHostWindow(kComparisonKind, comparison_id,
                            view->comparison->name);
        }
        ImGui::Separator();
        if (view->comparison->HasTrack()) {
          ImGui::TextDisabled(
              "%zu laps on %s", view->comparison->laps.size(),
              std::filesystem::path(view->comparison->track_path)
                  .stem()
                  .string()
                  .c_str());
        } else {
          ImGui::TextDisabled("empty");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("New comparison", "Ctrl+Shift+N")) {
          want_new_comparison = true;
        }
        if (ImGui::MenuItem("Close comparison")) {
          want_remove_comparison = comparison_id;
        }
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("Laps")) {
        view->DrawLapsMenu(session);
        ImGui::EndMenu();
      }
      DrawPanelsMenu(ui, kComparisonPanels, std::size(kComparisonPanels));
      ImGui::EndMenuBar();
    }

    DrawOwnerDockspace(ui, [this, comparison_id](ImGuiID root) {
      BuildComparisonLayout(root, comparison_id);
    });
  }

  /// The owner's own View menu: which of its panels are open, plus a way
  /// back to the arrangement they started in.
  void DrawPanelsMenu(OwnerUi &ui, const char *const *panels, size_t count) {
    if (!ImGui::BeginMenu("View"))
      return;
    for (size_t i = 0; i < count; ++i) {
      if (ImGui::MenuItem(panels[i], nullptr, ui.visible[i])) {
        ui.visible[i] = !ui.visible[i];
      }
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Show all panels")) {
      for (size_t i = 0; i < count; ++i)
        ui.visible[i] = true;
    }
    if (ImGui::MenuItem("Reset panel layout")) {
      for (size_t i = 0; i < count; ++i)
        ui.visible[i] = true;
      ui.rebuild_layout = true;
    }
    ImGui::EndMenu();
  }

  //-------------------------------- PANELS ---------------------------------//

  /// Draws every open panel of every owner whose host window is on screen.
  /// Runs after the host windows (PostRenderDockableWindows), so the
  /// dockspace each panel docks into has already been submitted this frame.
  void DrawPanels() {
    for (auto &view : views) {
      OwnerUi &ui = source_ui[view->source->id];
      if (!ui.dockspace_live) {
        KeepDockspaceAlive(ui);
        continue;
      }
      for (size_t i = 0; i < std::size(kSourcePanels); ++i) {
        if (!ui.visible[i])
          continue;
        std::string label =
            PanelWindowLabel(kSourcePanels[i], kSourceKind, view->source->id);
        if (ImGui::Begin(label.c_str(), &ui.visible[i])) {
          DrawSourcePanel(*view, i);
        }
        ImGui::End();
      }
    }

    for (auto &view : comparison_views) {
      OwnerUi &ui = comparison_ui[view->comparison->id];
      if (!ui.dockspace_live) {
        KeepDockspaceAlive(ui);
        continue;
      }
      for (size_t i = 0; i < std::size(kComparisonPanels); ++i) {
        if (!ui.visible[i])
          continue;
        std::string label = PanelWindowLabel(
            kComparisonPanels[i], kComparisonKind, view->comparison->id);
        if (ImGui::Begin(label.c_str(), &ui.visible[i])) {
          DrawComparisonPanel(*view, i);
        }
        ImGui::End();
      }
    }
  }

  /// A host window that is closed, collapsed or an unselected tab does not
  /// submit its dockspace, and ImGui drops nodes nobody claimed -- which
  /// would undock every panel inside it. This claims the node without
  /// drawing it, so the arrangement survives until the host is back.
  static void KeepDockspaceAlive(const OwnerUi &ui) {
    if (ui.dockspace_id == 0)
      return; // never drawn, so there is no node yet
    ImGui::DockSpace(ui.dockspace_id, ImVec2(0, 0),
                     ImGuiDockNodeFlags_KeepAliveOnly);
  }

  /// `panel` indexes kSourcePanels.
  void DrawSourcePanel(pacer::SourceView &view, size_t panel) {
    switch (panel) {
    case 0:
      DrawMapPanel(view);
      break;
    case 1:
      view.DrawSamplesPanel();
      break;
    case 2:
      view.DrawLapChartPanel();
      break;
    case 3:
      view.DrawLapTablePanel();
      break;
    case 4:
      view.display.DisplayLapTelemetry();
      break;
    case 5:
      view.DrawTrackPanel();
      break;
    case 6:
      view.DrawFilesPanel();
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

  /// `panel` indexes kComparisonPanels.
  void DrawComparisonPanel(pacer::ComparisonView &view, size_t panel) {
    if (panel == 0) {
      view.Display(session);
      return;
    }

    if (!view.comparison->HasTrack()) {
      ImGui::TextWrapped("Drop a lap into this comparison to see it on the "
                         "map.");
      return;
    }
    ImGui::Checkbox("Satellite", &view.show_satellite);
    ImGui::SameLine();
    ImGui::Checkbox("Reference track", &view.show_reference_track);
    if (ImPlot::BeginPlot("##comparison_map", ImVec2(-1, -1),
                          ImPlotFlags_Equal)) {
      view.SetupComparisonMap();
      if (view.show_satellite) {
        pacer::PlotSatelliteTiles(tile_store, view.cs);
      }
      view.PlotComparisonMap(session);
      ImPlot::EndPlot();
    }
  }

  //------------------------------- APP MENUS -------------------------------//

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

    // A session file records the setup -- paths, trims, tracks, which laps
    // each comparison holds -- and re-reads the recordings on open.
    if (pacer::HasNativeFileDialog()) {
      if (ImGui::MenuItem("Open session...", "Ctrl+O")) {
        PromptOpenSession();
      }
      if (ImGui::MenuItem("Save session", "Ctrl+S") &&
          !session_path.empty()) {
        SaveSession();
      }
      if (ImGui::MenuItem("Save session as...", "Ctrl+Shift+S")) {
        PromptSaveSessionAs();
      }
      ImGui::TextDisabled("%s", session_path.empty() ? "unsaved"
                                                     : session_path.c_str());
    } else {
      ImGui::SetNextItemWidth(280);
      ImGui::InputText("##session_path", &session_path);
      if (ImGui::MenuItem("Open session", "Ctrl+O") &&
          !session_path.empty()) {
        want_open_session = session_path;
      }
      if (ImGui::MenuItem("Save session", "Ctrl+S") &&
          !session_path.empty()) {
        SaveSession();
      }
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

  /// Toggles the host window whose ImGui identity is `id`.
  void MenuItemForHost(const char *label, const std::string &id) {
    for (auto &window : params->dockingParams.dockableWindows) {
      if (!window.label.ends_with("###" + id))
        continue;
      if (ImGui::MenuItem(label, nullptr, window.isVisible)) {
        window.isVisible = !window.isVisible;
        if (window.isVisible)
          window.focusWindowAtNextFrame = true;
      }
      return;
    }
    ImGui::BeginDisabled();
    ImGui::MenuItem(label, nullptr, false);
    ImGui::EndDisabled();
  }

  /// The app's View menu is now only about which sources and comparisons
  /// are on screen -- what is inside each of them is that window's own
  /// business.
  void DrawViewMenu() {
    if (!ImGui::BeginMenu("View"))
      return;

    if (!views.empty()) {
      ImGui::SeparatorText("Sources");
    }
    for (auto &view : views) {
      MenuItemForHost(view->source->name.c_str(),
                      HostWindowId(kSourceKind, view->source->id));
    }
    if (!comparison_views.empty()) {
      ImGui::SeparatorText("Comparisons");
    }
    for (auto &view : comparison_views) {
      MenuItemForHost(view->comparison->name.c_str(),
                      HostWindowId(kComparisonKind, view->comparison->id));
    }

    ImGui::Separator();
    if (ImGui::MenuItem("Restore default layout")) {
      params->dockingParams.layoutReset = true;
      for (auto &[id, ui] : source_ui)
        ui.rebuild_layout = true;
      for (auto &[id, ui] : comparison_ui)
        ui.rebuild_layout = true;
    }
    ImGui::MenuItem("Status bar", nullptr,
                    &params->imGuiWindowParams.showStatusBar);
    ImGui::EndMenu();
  }

  //--------------------------------- FRAME ---------------------------------//

  /// True once every owner's window has been registered with hello_imgui
  /// (which happens two PreNewFrames after AddDockableWindow) and created
  /// in ImGui. Only then does a layout reset have anything to place.
  bool HostWindowsReady() {
    auto ready = [this](const char *kind, int id, const std::string &name) {
      return FindWindow(params, HostWindowId(kind, id)) != nullptr &&
             ImGui::FindWindowByName(
                 HostWindowLabel(kind, id, name).c_str()) != nullptr;
    };
    for (const auto &view : views) {
      if (!ready(kSourceKind, view->source->id, view->source->name))
        return false;
    }
    for (const auto &view : comparison_views) {
      if (!ready(kComparisonKind, view->comparison->id,
                 view->comparison->name))
        return false;
    }
    return true;
  }

  void NewFrame() {
    ApplyPendingEdits();
    for (auto &view : views) {
      view->Update();
    }
    for (auto &view : comparison_views) {
      view->Update();
    }
    // Cleared here and set again by whichever host windows get drawn this
    // frame, a few calls later.
    for (auto &[id, ui] : source_ui)
      ui.dockspace_live = false;
    for (auto &[id, ui] : comparison_ui)
      ui.dockspace_live = false;

    if (want_layout_reset_ && HostWindowsReady()) {
      want_layout_reset_ = false;
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
    } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift |
                                       ImGuiKey_S)) {
      if (pacer::HasNativeFileDialog())
        PromptSaveSessionAs();
    } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
      SaveSession();
    } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) {
      if (pacer::HasNativeFileDialog()) {
        PromptOpenSession();
      } else {
        want_open_session = session_path;
      }
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

  // The startup owners' windows go into the initial list rather than
  // through AddDockableWindow, so the default layout places them on frame
  // one; anything added later goes through AddDockableWindow.
  for (auto &view : app.views) {
    app.AddSourceWindow(*view, /*run_time=*/false);
  }
  for (auto &view : app.comparison_views) {
    app.AddComparisonWindow(*view, /*run_time=*/false);
  }

  runnerParams.callbacks.ShowGui = [&]() { app.NewFrame(); };
  // The panels live inside their owner's dockspace, so they are drawn after
  // the host windows that submit it.
  runnerParams.callbacks.PostRenderDockableWindows = [&]() {
    app.DrawPanels();
  };

  HelloImGui::Run(runnerParams);

  ImPlot::DestroyContext(implotContext);

  return 0;
}
