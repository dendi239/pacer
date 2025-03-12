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

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>

#include "GPMF_common.h"
#include "GPMF_parser.h"
#include "demo/GPMF_mp4reader.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "implot.h"

#include <stdexcept>
#include <stdio.h>
#include <strings.h>
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

struct GPSSample {
  double lat, lon, altitude, ground_speed, full_speed;
};

ImPlotPoint ToPoint(int index, void *data) {
  GPSSample *data_ = reinterpret_cast<GPSSample *>(data);
  return ImPlotPoint(data_[index].lon, data_[index].lat);
}

struct MovieHandler {
  uint32_t index = 0;
  size_t mp4handle;

  explicit MovieHandler(size_t mp4handle) : mp4handle(mp4handle) {}
  explicit MovieHandler(char *filename)
      : mp4handle(OpenMP4Source(filename, MOV_GPMF_TRAK_TYPE,
                                MOV_GPMF_TRAK_SUBTYPE, 0)) {
    if (mp4handle == 0) {
      throw std::runtime_error(
          (std::string("Failed to open file: ") + std::string(filename))
              .c_str());
    }
  }

  uint32_t samples(std::vector<double> *lat, std::vector<double> *lon) {
    return samples([&](GPSSample s) {
      lat->push_back(s.lat);
      lon->push_back(s.lon);
    });
  }

  template <class F> uint32_t samples(F on_sample) {
    uint32_t payloadsize = GetPayloadSize(mp4handle, index);
    size_t payloadres = 0;
    payloadres = GetPayloadResource(mp4handle, payloadres, payloadsize);
    uint32_t *payload = GetPayload(mp4handle, payloadres, index);

    if (payload == nullptr) {
      printf("No payload\n");
      return 1;
    }

    GPMF_stream metadata_stream, *ms = &metadata_stream;
    auto ret = GPMF_Init(ms, payload, payloadsize);
    if (ret != GPMF_OK) {
      printf("No init\n");
      return ret;
    }

    while (GPMF_OK ==
           GPMF_FindNext(ms, STR2FOURCC("STRM"),
                         GPMF_LEVELS(GPMF_RECURSE_LEVELS | GPMF_TOLERANT))) {

      if (ret != GPMF_OK) {
        printf("No FindNext gps data\n");
        return ret;
      }

      ret = GPMF_SeekToSamples(ms);
      if (ret != GPMF_OK) {
        printf("No Seek to Samples\n");
        return ret;
      }

      char *rawdata = (char *)GPMF_RawData(ms);
      uint32_t key = GPMF_Key(ms);
      GPMF_SampleType type = GPMF_Type(ms);
      uint32_t samples = GPMF_Repeat(ms);
      uint32_t elements = GPMF_ElementsInStruct(ms);

      if (key != STR2FOURCC("GPS5")) {
        continue;
      }

      if (samples) {
        uint32_t buffersize = samples * elements * sizeof(double);
        GPMF_stream find_stream;
        double *ptr, *tmpbuffer = (double *)malloc(buffersize);

#define MAX_UNITS 64
#define MAX_UNITLEN 8
        char units[MAX_UNITS][MAX_UNITLEN] = {""};
        uint32_t unit_samples = 1;

        char complextype[MAX_UNITS] = {""};
        uint32_t type_samples = 1;

        if (tmpbuffer) {
          uint32_t i, j;

          // Search for any units to display
          GPMF_CopyState(ms, &find_stream);
          if (GPMF_OK == GPMF_FindPrev(
                             &find_stream, GPMF_KEY_SI_UNITS,
                             GPMF_LEVELS(GPMF_CURRENT_LEVEL | GPMF_TOLERANT)) ||
              GPMF_OK == GPMF_FindPrev(
                             &find_stream, GPMF_KEY_UNITS,
                             GPMF_LEVELS(GPMF_CURRENT_LEVEL | GPMF_TOLERANT))) {
            char *data = (char *)GPMF_RawData(&find_stream);
            uint32_t ssize = GPMF_StructSize(&find_stream);
            if (ssize > MAX_UNITLEN - 1)
              ssize = MAX_UNITLEN - 1;
            unit_samples = GPMF_Repeat(&find_stream);

            for (i = 0; i < unit_samples && i < MAX_UNITS; i++) {
              memcpy(units[i], data, ssize);
              units[i][ssize] = 0;
              data += ssize;
            }
          }

          // GPMF_FormattedData(ms, tmpbuffer, buffersize, 0, samples); //
          // Output data in LittleEnd, but no scale
          if (GPMF_OK ==
              GPMF_ScaledData(ms, tmpbuffer, buffersize, 0, samples,
                              GPMF_TYPE_DOUBLE)) // Output scaled data as floats
          {

            ptr = tmpbuffer;
            int pos = 0;
            union {
              GPSSample gps;
              double values[5];
            } r;

            for (i = 0; i < samples; i++) {
              printf("  %c%c%c%c ", PRINTF_4CC(key));

              for (j = 0; j < elements; j++) {
                if (type == GPMF_TYPE_STRING_ASCII) {

                  printf("%c", rawdata[pos]);
                  pos++;
                  ptr++;
                } else if (type_samples == 0) // no TYPE structure
                {

                  printf("%.3f%s, ", *ptr++, units[j % unit_samples]);
                } else if (complextype[j] != 'F') {
                  r.values[j % unit_samples] = *ptr;
                  if ((j + 1) % unit_samples == 0) {
                    on_sample(r.gps);
                  }

                  printf("%.3f%s, ", *ptr++, units[j % unit_samples]);

                  pos += GPMF_SizeofType((GPMF_SampleType)complextype[j]);
                } else if (type_samples && complextype[j] == GPMF_TYPE_FOURCC) {
                  ptr++;

                  printf("%c%c%c%c, ", rawdata[pos], rawdata[pos + 1],
                         rawdata[pos + 2], rawdata[pos + 3]);
                  pos += GPMF_SizeofType((GPMF_SampleType)complextype[j]);
                }
              }

              printf("\n");
            }
          }
          free(tmpbuffer);
        }
      }
    }

    return ret;
  }

  uint32_t seek(float target) {
    double in, out;
    uint32_t ret = GPMF_OK;
    do {
      ret = GetPayloadTime(mp4handle, index, &in, &out);
      if (ret == GPMF_OK) {
        if (out <= target)
          ++index;
        if (target < in)
          --index;
        printf("Index: %d ret: %d, target: %.2f, in: %.2f, out: %.2f\n", index,
               ret, target, in, out);
      }
    } while (ret == GPMF_OK && in < out && (target > out) || (target < in));
    if (!(in < out)) {
      --index;
    }

    return ret;
  }

  void next() { ++index; }

  bool is_end() {
    double in, out;
    if (GetPayloadTime(mp4handle, index, &in, &out) != GPMF_OK) {
      return true;
    }
    return in + 1e-9 >= out;
  }

  std::pair<double, double> current_time_span() const {
    double in, out;
    GetPayloadTime(mp4handle, index, &in, &out);
    return {in, out};
  }

  float get_duration() const { return GetDuration(mp4handle); }
};

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
      1280, 720, "Dear ImGui & ImPlot GLFW+OpenGL3 example", nullptr, nullptr);
  if (window == nullptr)
    return 1;
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1); // Enable vsync

  // Setup Dear ImGui context
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImPlot::CreateContext();

  char *filename = "/mnt/c/work/gokart-videos/GH010224.MP4";
  MovieHandler m(filename);

  m.seek(0);
  std::vector<double> lats, lons;
  for (m.seek(0); !m.is_end(); m.next()) {
    m.samples(&lats, &lons);
  }
  m.seek(0);

  float duration = m.get_duration(), current = 0;

  ImGuiIO &io = ImGui::GetIO();
  (void)io;
  io.ConfigFlags |=
      ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
  io.ConfigFlags |=
      ImGuiConfigFlags_NavEnableGamepad; // Enable Gamepad Controls

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
  // - AddFontFromFileTTF() will return the ImFont* so you can store it if you
  // need to select the font among multiple.
  // - If the file cannot be loaded, the function will return a nullptr. Please
  // handle those errors in your application (e.g. use an assertion, or display
  // an error and quit).
  // - The fonts will be rasterized at a given size (w/ oversampling) and stored
  // into a texture when calling ImFontAtlas::Build()/GetTexDataAsXXXX(), which
  // ImGui_ImplXXXX_NewFrame below will call.
  // - Use '#define IMGUI_ENABLE_FREETYPE' in your imconfig file to use Freetype
  // for higher quality font rendering.
  // - Read 'docs/FONTS.md' for more instructions and details.
  // - Remember that in C/C++ if you want to include a backslash \ in a string
  // literal you need to write a double backslash \\ !
  // - Our Emscripten build process allows embedding fonts to be accessible at
  // runtime from the "fonts/" folder. See Makefile.emscripten for details.
  // io.Fonts->AddFontDefault();
  // io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\segoeui.ttf", 18.0f);
  // io.Fonts->AddFontFromFileTTF("../../misc/fonts/DroidSans.ttf", 16.0f);
  // io.Fonts->AddFontFromFileTTF("../../misc/fonts/Roboto-Medium.ttf", 16.0f);
  // io.Fonts->AddFontFromFileTTF("../../misc/fonts/Cousine-Regular.ttf", 15.0f);
  // ImFont* font =
  // io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\ArialUni.ttf", 18.0f,
  // nullptr, io.Fonts->GetGlyphRangesJapanese()); IM_ASSERT(font != nullptr);

  // Our state
  bool show_imgui_demo_window = true;
  bool show_implot_demo_window = true;
  bool show_another_window = false;
  ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

  // Main loop
#ifdef __EMSCRIPTEN__
  // For an Emscripten build we are disabling file-system access, so let's not
  // attempt to do a fopen() of the imgui.ini file. You may manually call
  // LoadIniSettingsFromMemory() to load settings from your own storage.
  io.IniFilename = nullptr;
  EMSCRIPTEN_MAINLOOP_BEGIN
#else
  while (!glfwWindowShouldClose(window))
#endif
  {
    // Poll and handle events (inputs, window resize, etc.)
    // You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to
    // tell if dear imgui wants to use your inputs.
    // - When io.WantCaptureMouse is true, do not dispatch mouse input data to
    // your main application, or clear/overwrite your copy of the mouse data.
    // - When io.WantCaptureKeyboard is true, do not dispatch keyboard input
    // data to your main application, or clear/overwrite your copy of the
    // keyboard data. Generally you may always pass all inputs to dear imgui,
    // and hide them from your application based on those two flags.
    glfwPollEvents();
    if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0) {
      ImGui_ImplGlfw_Sleep(10);
      continue;
    }

    // Start the Dear ImGui frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // 1. Show the big demo window (Most of the sample code is in
    // ImGui::ShowDemoWindow()! You can browse its code to learn more about Dear
    // ImGui!).
    if (show_imgui_demo_window)
      ImGui::ShowDemoWindow(&show_imgui_demo_window);

    if (show_implot_demo_window)
      ImPlot::ShowDemoWindow(&show_implot_demo_window);

    std::vector<double> x, y;
    std::vector<GPSSample> gps;

    if (ImGui::Begin("timeline")) {
      ImGui::Text("Duration: %.2f", duration);
      ImGui::SliderFloat("Time", &current, 0, duration);
      m.seek(current);
      m.samples([&](auto s) { gps.push_back(s); });
    }
    ImGui::End();

    if (ImGui::Begin("Map")) {
      if (ImPlot::BeginPlot("GPS", ImVec2(-1, -1))) {
        ImPlot::SetupAxisLimits(ImAxis_Y1,
                                *std::min_element(lats.begin(), lats.end()),
                                *std::max_element(lats.begin(), lats.end()));
        ImPlot::SetupAxisLimits(ImAxis_X1,
                                *std::min_element(lons.begin(), lons.end()),
                                *std::max_element(lons.begin(), lons.end()));
        ImPlot::PlotScatterG("data", ToPoint, gps.data(), gps.size());
        ImPlot::PlotLine("trace", lons.data(), lats.data(), lats.size());
        std::stringstream ss;
        ss << "Speed: " << gps.back().full_speed * 3.6 << "km/h";
        ImPlot::PlotText(ss.str().data(), gps.back().lon, gps.back().lat);
        ImPlot::EndPlot();
      }
    }
    ImGui::End();

    // 2. Show a simple window that we create ourselves. We use a Begin/End pair
    // to create a named window.
    {
      static float f = 0.0f;
      static int counter = 0;

      ImGui::Begin("Hello, world!"); // Create a window called "Hello, world!"
                                     // and append into it.

      ImGui::Text("This is some useful text."); // Display some text (you can
                                                // use a format strings too)
      ImGui::Checkbox("ImGui demo Window",
                      &show_imgui_demo_window); // Edit bools storing our window
                                                // open/close state
      ImGui::Checkbox("ImPlot demo Window",
                      &show_implot_demo_window); // Edit bools storing our
                                                 // window open/close state
      ImGui::Checkbox("Another Window", &show_another_window);

      ImGui::SliderFloat("float", &f, 0.0f,
                         1.0f); // Edit 1 float using a slider from 0.0f to 1.0f
      ImGui::ColorEdit3(
          "clear color",
          (float *)&clear_color); // Edit 3 floats representing a color

      if (ImGui::Button("Button")) // Buttons return true when clicked (most
                                   // widgets return true when edited/activated)
        counter++;
      ImGui::SameLine();
      ImGui::Text("counter = %d", counter);

      ImGui::Text("Application average %.3f ms/frame (%.1f FPS)",
                  1000.0f / io.Framerate, io.Framerate);
      ImGui::End();
    }

    if (ImGui::Begin("Telemetry data")) {
      auto [start, end] = m.current_time_span();
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
      ImGui::SameLine();
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
        // (Headers are not the main purpose of this section of the demo, so we
        // are not elaborating on them now. See other sections for details)
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
            ImGui::Text("%.2f", reinterpret_cast<double *>(&gps[row])[column] * (column > 2 ? 3.6 : 1.0));
          }
        }
        ImGui::EndTable();
      }
    }
    ImGui::End();

    // 3. Show another simple window.
    if (show_another_window) {
      ImGui::Begin(
          "Another Window",
          &show_another_window); // Pass a pointer to our bool variable (the
                                 // window will have a closing button that will
                                 // clear the bool when clicked)
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
