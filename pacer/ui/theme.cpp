#include "theme.hpp"

#include <hello_imgui/hello_imgui.h>
#include <hello_imgui/imgui_theme.h>
#include <imgui.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten/em_js.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace {

// How often the host is asked. The apps idle at 3 fps, so anything much
// shorter than this is pointless anyway.
constexpr double kPollIntervalSeconds = 0.5;

// The two themes we switch between. DarculaDarker is hello_imgui's default
// and what these apps looked like before they followed the host.
constexpr ImGuiTheme::ImGuiTheme_ kDarkTheme = ImGuiTheme::ImGuiTheme_DarculaDarker;
constexpr ImGuiTheme::ImGuiTheme_ kLightTheme = ImGuiTheme::ImGuiTheme_LightRounded;

pacer::SystemTheme g_applied = pacer::SystemTheme::kUnknown;

#if defined(__EMSCRIPTEN__)
// -1 when the browser has no matchMedia at all; matchMedia().matches is a
// plain property read, cheap enough to poll.
EM_JS(int, QueryPrefersDark, (), {
  if (!globalThis.matchMedia)
    return -1;
  return globalThis.matchMedia('(prefers-color-scheme: dark)').matches ? 1 : 0;
});
#endif

} // namespace

pacer::SystemTheme pacer::QuerySystemTheme() {
#if defined(__EMSCRIPTEN__)
  switch (QueryPrefersDark()) {
  case 0:
    return SystemTheme::kLight;
  case 1:
    return SystemTheme::kDark;
  default:
    return SystemTheme::kUnknown;
  }
#elif defined(__APPLE__)
  // The global AppleInterfaceStyle preference is the string "Dark" in dark
  // mode and simply absent in light mode. Synchronizing first is what makes
  // the value change under us when the user flips the system setting; without
  // it we would keep reading whatever was cached at process start.
  CFPreferencesAppSynchronize(kCFPreferencesAnyApplication);
  CFPropertyListRef value = CFPreferencesCopyAppValue(
      CFSTR("AppleInterfaceStyle"), kCFPreferencesAnyApplication);
  if (value == nullptr)
    return SystemTheme::kLight;
  SystemTheme theme = SystemTheme::kLight;
  if (CFGetTypeID(value) == CFStringGetTypeID() &&
      CFStringHasPrefix(static_cast<CFStringRef>(value), CFSTR("Dark")))
    theme = SystemTheme::kDark;
  CFRelease(value);
  return theme;
#else
  // No Linux/Windows probe yet: those hosts keep the dark default.
  return SystemTheme::kUnknown;
#endif
}

bool pacer::FollowSystemTheme() {
  static double next_poll = -1.0;

  double now = ImGui::GetTime();
  if (now < next_poll)
    return false;
  next_poll = now + kPollIntervalSeconds;

  SystemTheme detected = QuerySystemTheme();
  if (detected == SystemTheme::kUnknown || detected == g_applied)
    return false;
  g_applied = detected;

  auto *params = HelloImGui::GetRunnerParams();
  params->imGuiWindowParams.tweakedTheme.Theme =
      (detected == SystemTheme::kDark) ? kDarkTheme : kLightTheme;
  ImGuiTheme::ApplyTweakedTheme(params->imGuiWindowParams.tweakedTheme);
  return true;
}

bool pacer::IsDarkTheme() { return g_applied != SystemTheme::kLight; }
