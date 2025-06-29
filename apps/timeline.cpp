// This file based on ImGui's demo app with ImPlot's demo sprinkled on top.
// I'm gonna leave all boilerplate for now.

// Dear ImGui: standalone example application for GLFW + OpenGL 3, using
// programmable pipeline (GLFW is a cross-platform general purpose library for
// handling windows, inputs, OpenGL/Vulkan/Metal graphics context creation,
// etc.)

// Learn about Dear ImGui:
// - FAQ                  https://dearimgui.com/faq
// - Getting Started      https://dearimgui.com/getting-started
// - Documentation        https://dearimgui.com/docs (same as your local docs/
// folder).
// - Introduction, links and more at the top of imgui.cpp

#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "implot.h"
#include "implot_internal.h"

#include <pacer/datatypes/datatypes.hpp>
#include <pacer/geometry/geometry.hpp>
#include <pacer/gps-source/gps-source.hpp>
#include <pacer/laps-display/laps-display.hpp>
#include <pacer/laps/laps.hpp>

#include <stdio.h>
#include <strings.h>
#include <unordered_map>
#include <vector>
#define GL_SILENCE_DEPRECATION
#include <GLFW/glfw3.h> // Will drag system OpenGL headers

// [Win32] Our example includes a copy of glfw3.lib pre-compiled with VS2010 to
// maximize ease of testing and compatibility with old VS compilers. To link
// with VS2010-era libraries, VS2015+ requires linking with
// legacy_stdio_definitions.lib, which we do using this pragma. Your own project
// should not be affected, as you are likely to link with a newer binary of GLFW
// that is adequate for your version of Visual Studio.
#if defined(_MSC_VER) && (_MSC_VER >= 1900) &&                                 \
    !defined(IMGUI_DISABLE_WIN32_FUNCTIONS)
#pragma comment(lib, "legacy_stdio_definitions")
#endif

// This example can also compile and run with Emscripten! See
// 'Makefile.emscripten' for details.
#ifdef __EMSCRIPTEN__
#include "../libs/emscripten/emscripten_mainloop_stub.h"
#endif

static void glfw_error_callback(int error, const char *description) {
  fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

using pacer::GPSSample;

void ReadInput(pacer::Laps *plaps) {
  const char *filenames[] = {
      // "/Users/denys/Documents/gokarting-ui/GH010219.MP4",
      // "/Users/denys/Documents/gokarting-ui/GH020219.MP4",
      // "/Users/denys/Documents/gokarting-ui/GH030219.MP4",
      // "/Users/denys/Documents/gokarting-ui/GH040219.MP4",
      // "/Users/denys/Documents/gokarting-ui/GH050219.MP4",
      // "/Users/denys/Downloads/GX010079.MP4",
      // "/Users/denys/Downloads/GX020079.MP4",
      // "/Users/denys/Downloads/GX030079.MP4",
      "/Users/denys/Pictures/GH010251.MP4",
      "/Users/denys/Pictures/GH020251.MP4",
      "/Users/denys/Pictures/GH030251.MP4",
  };

  pacer::GPMFSource mm[] = {
      pacer::GPMFSource(filenames[0]), pacer::GPMFSource(filenames[1]),
      pacer::GPMFSource(filenames[2]),
      // pacer::GPMFSource(filenames[3]),
      // pacer::GPMFSource(filenames[4]),
  };
  pacer::SequentialGPSSource m12(&mm[0], &mm[1]), m(&m12, &mm[2]);
  // m14(&m13, &mm[3]), m(&m14, &mm[4]);

  // const char *filename = "/mnt/c/work/gokart-videos/GH010243.MP4";
  // pacer::GPMFSource m(filename);

  m.Seek(0);

  auto &laps = *plaps;

  pacer::CoordinateSystem cs;
  std::unordered_map<int, int> counts(20);

  std::vector<pacer::PointInTime<GPSSample>> samples;

  for (m.Seek(0); !m.IsEnd(); m.Next()) {
    auto [start, end] = m.CurrentTimeSpan();
    m.pacer::RawGPSSource::Samples(
        [&](GPSSample s, size_t current, size_t total) {
          if (s.full_speed > 1e-6) {
            laps.AddPoint(s, start + (end - start) / total * current);
          }
        });
  }
}

void ReadInputDat(pacer::Laps *plaps) {
  pacer::ReadDatFile(
      "/Users/denys/Downloads/1749283873879948155.dat",
      [&](pacer::GPSSample sample, double time) {
        plaps->AddPoint(sample, time);
        std::cerr << "Added sample: " << sample << " at time: " << time
                  << std::endl;
      },
      pacer::DatVersion::WITH_TIMESTAMP);
}

void DisplayTelemetry(pacer::RawGPSSource &m, std::vector<GPSSample> &gps,
                      float &current, float duration) {
  if (ImGui::Begin("timeline")) {
    ImGui::Text("Duration: %.2f", duration);
    ImGui::SetNextItemWidth(ImGui::GetWindowWidth() - 80);
    if (ImGui::SliderFloat("Time", &current, 0, duration))
      m.Seek(current);
    ImGui::SameLine();
    if (ImGui::Button(">"))
      m.Next();
    m.pacer::RawGPSSource::Samples(
        [&](auto s, size_t, size_t) { gps.push_back(s); });
  }
  ImGui::End();

  if (ImGui::Begin("Telemetry data")) {
    auto [start, end] = m.CurrentTimeSpan();
    ImGui::Text("Current time: %.3f %.3f", start, end);

    // Expose a few Borders related flags interactively
    enum ContentsType { CT_Text, CT_FillButton };
    static ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg;
    static bool display_headers = false;
    static int contents_type = CT_Text;

    ImGui::CheckboxFlags("ImGuiTableFlags_RowBg", &flags,
                         ImGuiTableFlags_RowBg);
    ImGui::CheckboxFlags("ImGuiTableFlags_Borders", &flags,
                         ImGuiTableFlags_Borders);
    // ImGui::SameLine();
    ImGui::Indent();

    ImGui::CheckboxFlags("ImGuiTableFlags_BordersH", &flags,
                         ImGuiTableFlags_BordersH);
    ImGui::Indent();
    ImGui::CheckboxFlags("ImGuiTableFlags_BordersOuterH", &flags,
                         ImGuiTableFlags_BordersOuterH);
    ImGui::CheckboxFlags("ImGuiTableFlags_BordersInnerH", &flags,
                         ImGuiTableFlags_BordersInnerH);
    ImGui::Unindent();

    ImGui::CheckboxFlags("ImGuiTableFlags_BordersV", &flags,
                         ImGuiTableFlags_BordersV);
    ImGui::Indent();
    ImGui::CheckboxFlags("ImGuiTableFlags_BordersOuterV", &flags,
                         ImGuiTableFlags_BordersOuterV);
    ImGui::CheckboxFlags("ImGuiTableFlags_BordersInnerV", &flags,
                         ImGuiTableFlags_BordersInnerV);
    ImGui::Unindent();

    ImGui::CheckboxFlags("ImGuiTableFlags_BordersOuter", &flags,
                         ImGuiTableFlags_BordersOuter);
    ImGui::CheckboxFlags("ImGuiTableFlags_BordersInner", &flags,
                         ImGuiTableFlags_BordersInner);
    ImGui::Unindent();

    ImGui::AlignTextToFramePadding();
    ImGui::Text("Cell contents:");
    ImGui::SameLine();
    ImGui::RadioButton("Text", &contents_type, CT_Text);
    ImGui::SameLine();
    ImGui::RadioButton("FillButton", &contents_type, CT_FillButton);
    ImGui::Checkbox("Display headers", &display_headers);
    ImGui::CheckboxFlags("ImGuiTableFlags_NoBordersInBody", &flags,
                         ImGuiTableFlags_NoBordersInBody);
    // ImGui::SameLine();

    if (ImGui::BeginTable("table1", 5, flags)) {
      // Display headers so we can inspect their interaction with borders
      // (Headers are not the main purpose of this section of the demo, so
      // we are not elaborating on them now. See other sections for
      // details)
      if (display_headers) {
        ImGui::TableSetupColumn("Latitude");
        ImGui::TableSetupColumn("Longitude");
        ImGui::TableSetupColumn("Altitude");
        ImGui::TableSetupColumn("Ground Speed");
        ImGui::TableSetupColumn("Full Speed");
        ImGui::TableHeadersRow();
      }

      for (int row = 0; row < gps.size(); row++) {
        ImGui::TableNextRow();
        for (int column = 0; column < 5; column++) {
          ImGui::TableSetColumnIndex(column);
          ImGui::Text("%.2f", reinterpret_cast<double *>(&gps[row])[column] *
                                  (column > 2 ? 3.6 : 1.0));
        }
      }
      ImGui::EndTable();
    }
  }
  ImGui::End();
}

// Main code
int main(int, char **) {
  glfwSetErrorCallback(glfw_error_callback);
  if (!glfwInit())
    return 1;

  // Decide GL+GLSL versions
#if defined(IMGUI_IMPL_OPENGL_ES2)
  // GL ES 2.0 + GLSL 100 (WebGL 1.0)
  const char *glsl_version = "#version 100";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
  glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
#elif defined(IMGUI_IMPL_OPENGL_ES3)
  // GL ES 3.0 + GLSL 300 es (WebGL 2.0)
  const char *glsl_version = "#version 300 es";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
  glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
#elif defined(__APPLE__)
  // GL 3.2 + GLSL 150
  const char *glsl_version = "#version 150";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE); // 3.2+ only
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE); // Required on Mac
  glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
#else
  // GL 3.0 + GLSL 130
  const char *glsl_version = "#version 130";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
  // glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+
  // only glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE); // 3.0+ only
#endif

  // Create window with graphics context
  GLFWwindow *window = glfwCreateWindow(
      1440, 900, "Dear ImGui & ImPlot GLFW+OpenGL3 example", nullptr, nullptr);
  if (window == nullptr)
    return 1;

  glfwMakeContextCurrent(window);
  glfwSwapInterval(1); // Enable vsync

  // Setup Dear ImGui context
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImPlot::CreateContext();

  float xscale = 1, yscale = 1;
  ImGuiIO &io = ImGui::GetIO();

#ifdef __APPLE__

  glfwGetWindowContentScale(window, &xscale, &yscale);
  // io.DisplayFramebufferScale = ImVec2(xscale, yscale);

  ImGuiStyle &style = ImGui::GetStyle();
  // style.ScaleAllSizes(1 / xscale);

  float base_font_size = 16.0f; // Desired font size in points
  ImFont *font = io.Fonts->AddFontFromFileTTF(
      "/Users/denys/dev/pacer/3rdparty/imgui/misc/fonts/Roboto-Medium.ttf",
      base_font_size * xscale);

  font->Scale = 1 / yscale;

  int display_w, display_h;
  glfwGetFramebufferSize(window, &display_w, &display_h);
  glViewport(0, 0, display_w * xscale, display_h * yscale);

  io.DisplayFramebufferScale = ImVec2(xscale, yscale);
  io.DisplaySize = ImVec2(display_w, display_h);

  // Example using OpenGL glMatrixMode(GL_PROJECTION);
  // glLoadIdentity();
  // glOrtho(0.0f, 1 / xscale * display_w, 1 / yscale * display_h, 0.0f,
  // -1.0f,
  //         1.0f);
#endif

  pacer::Laps full_laps;
  ReadInput(&full_laps);

  full_laps.sectors.start_line = full_laps.PickRandomStart();
  auto laps = full_laps;

  auto laps_display = pacer::LapsDisplay{&laps};
  pacer::DeltaLapsComparision delta;

  float duration =
            laps.GetPoint(laps.PointCount() - 1).time - laps.GetPoint(0).time,
        current = 0;

  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

  // Setup Dear ImGui style
  ImGui::StyleColorsDark();
  // ImGui::StyleColorsLight();

  // Setup Platform/Renderer backends
  ImGui_ImplGlfw_InitForOpenGL(window, true);
#ifdef __EMSCRIPTEN__
  ImGui_ImplGlfw_InstallEmscriptenCallbacks(window, "#canvas");
#endif
  ImGui_ImplOpenGL3_Init(glsl_version);

  // Load Fonts
  // - If no fonts are loaded, dear imgui will use the default font. You can
  // also load multiple fonts and use ImGui::PushFont()/PopFont() to select
  // them.
  // - AddFontFromFileTTF() will return the ImFont* so you can store it if
  // you need to select the font among multiple.
  // - If the file cannot be loaded, the function will return a nullptr.
  // Please handle those errors in your application (e.g. use an assertion,
  // or display an error and quit).
  // - The fonts will be rasterized at a given size (w/ oversampling) and
  // stored into a texture when calling
  // ImFontAtlas::Build()/GetTexDataAsXXXX(), which ImGui_ImplXXXX_NewFrame
  // below will call.
  // - Use '#define IMGUI_ENABLE_FREETYPE' in your imconfig file to use
  // Freetype for higher quality font rendering.
  // - Read 'docs/FONTS.md' for more instructions and details.
  // - Remember that in C/C++ if you want to include a backslash \ in a
  // string literal you need to write a double backslash \\ !
  // - Our Emscripten build process allows embedding fonts to be accessible
  // at runtime from the "fonts/" folder. See Makefile.emscripten for
  // details. io.Fonts->AddFontDefault();
  // io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\segoeui.ttf", 18.0f);
  // io.Fonts->AddFontFromFileTTF("../../misc/fonts/DroidSans.ttf", 16.0f);
  // io.Fonts->AddFontFromFileTTF("../../misc/fonts/Roboto-Medium.ttf", 16.0f);
  // io.Fonts->AddFontFromFileTTF("../../misc/fonts/Cousine-Regular.ttf", 15.0f);
  // ImFont* font =
  // io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\ArialUni.ttf", 18.0f,
  // nullptr, io.Fonts->GetGlyphRangesJapanese()); IM_ASSERT(font !=
  // nullptr);

  // Our state
  bool show_imgui_demo_window = true;
  bool show_implot_demo_window = true;
  bool show_another_window = false;
  ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

  // Main loop
#ifdef __EMSCRIPTEN__
  // For an Emscripten build we are disabling file-system access, so let's
  // not attempt to do a fopen() of the imgui.ini file. You may manually
  // call LoadIniSettingsFromMemory() to load settings from your own
  // storage.
  io.IniFilename = nullptr;
  EMSCRIPTEN_MAINLOOP_BEGIN
#else
  while (!glfwWindowShouldClose(window))
#endif
  {
    // Poll and handle events (inputs, window resize, etc.)
    // You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to
    // tell if dear imgui wants to use your inputs.
    // - When io.WantCaptureMouse is true, do not dispatch mouse input data
    // to your main application, or clear/overwrite your copy of the mouse
    // data.
    // - When io.WantCaptureKeyboard is true, do not dispatch keyboard input
    // data to your main application, or clear/overwrite your copy of the
    // keyboard data. Generally you may always pass all inputs to dear
    // imgui, and hide them from your application based on those two flags.
    glfwPollEvents();
    if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0) {
      ImGui_ImplGlfw_Sleep(10);
      continue;
    }

    // Start the Dear ImGui frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    laps.Update();

    // 1. Show the big demo window (Most of the sample code is in
    // ImGui::ShowDemoWindow()! You can browse its code to learn more about
    // Dear ImGui!).
    if (show_imgui_demo_window)
      ImGui::ShowDemoWindow(&show_imgui_demo_window);

    if (show_implot_demo_window)
      ImPlot::ShowDemoWindow(&show_implot_demo_window);

    std::vector<double> x, y;
    std::vector<GPSSample> gps;

    static float start = 0, end = full_laps.PointCount();
    if (ImGui::Begin("Data Subset")) {
      ImGui::Text("Select data subset to display on the map");
      ImGui::SetNextItemWidth(ImGui::GetWindowWidth() / 2);
      if (ImGui::SliderFloat("Start", &start, 0, end) ||
          (ImGui::SameLine(),
           ImGui::SliderFloat("End", &end, start, full_laps.PointCount()))) {
        laps.ClearPoints();
        for (size_t i = start; i < end; ++i) {
          auto [gps, time] = full_laps.GetPoint(i);
          laps.AddPoint(gps, time);
        }
      }
    }
    ImGui::End();

    ImGui::Text("Application average %.3f ms/frame (%.1f FPS)",
                1000.0f / io.Framerate, io.Framerate);

    delta.cs = laps_display.cs;
    static int old_selected_lap = laps_display.selected_lap;
    if (old_selected_lap != laps_display.selected_lap) {
      float width = delta.reference_lap.width;
      delta.reference_lap = laps.GetLap(laps_display.selected_lap);
      delta.reference_lap.width = width;
    }

    if (ImGui::Begin("Map")) {
      if (ImPlot::BeginPlot("GPS", ImVec2(-1, -1)), ImPlotFlags_Equal) {
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

    if (ImGui::Begin("Laps")) {
      delta.DrawSlider();
      ImGui::SameLine();
      laps_display.DisplayTable();
    }
    ImGui::End();

    if (ImGui::Begin("Delta")) {
      delta.Display(laps);
    }
    ImGui::End();

    // single_lap.Display();

    if (ImGui::Begin("Lap Telemetry")) {
      laps_display.DisplayLapTelemetry();
    }
    ImGui::End();

    // 3. Show another simple window.
    if (show_another_window) {
      ImGui::Begin(
          "Another Window",
          &show_another_window); // Pass a pointer to our bool variable (the
                                 // window will have a closing button that
                                 // will clear the bool when clicked)
      ImGui::Text("Hello from another window!");
      if (ImGui::Button("Close Me"))
        show_another_window = false;
      ImGui::End();
    }

    // Rendering
    ImGui::Render();
    int display_w, display_h;
    glfwGetFramebufferSize(window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w,
                 clear_color.z * clear_color.w, clear_color.w);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glfwSwapBuffers(window);
  }
#ifdef __EMSCRIPTEN__
  EMSCRIPTEN_MAINLOOP_END;
#endif

  // Cleanup
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImPlot::DestroyContext();
  ImGui::DestroyContext();

  glfwDestroyWindow(window);
  glfwTerminate();

  return 0;
}
