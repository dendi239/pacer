#pragma once

namespace pacer {

// What appearance the host is currently set to: the OS setting on desktop,
// the page's prefers-color-scheme in the browser. kUnknown means the
// platform gives us no way to tell, and callers should keep whatever they
// already have.
enum class SystemTheme { kUnknown, kLight, kDark };

// Asks the host directly. Not free (a preferences read on macOS, a JS call
// under emscripten), so don't call it every frame -- FollowSystemTheme()
// below rate-limits for you.
SystemTheme QuerySystemTheme();

// Re-reads the host appearance a few times per second and applies the
// matching ImGui theme when it changes. Call once per frame, from
// RunnerParams::callbacks::PreNewFrame so the new style is in place before
// any widget reads it. Returns true on the frames where it switched.
bool FollowSystemTheme();

// The appearance FollowSystemTheme() last applied -- for colors chosen by
// hand (ImDrawList, ImPlot line styles) that the ImGui style doesn't cover.
// Dark until told otherwise, which is how these apps have always looked.
bool IsDarkTheme();

} // namespace pacer
