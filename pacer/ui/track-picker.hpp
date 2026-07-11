#pragma once

#include <string>
#include <vector>

namespace pacer {

// Combo over the .json track files in `directory` plus a free-text path
// field, so both picking an existing track and naming a new one work.
struct TrackFilePicker {
  std::string directory = "tracks";
  std::string path; // current selection or hand-typed target

  /// Renders combo + Refresh + path field. Returns true when the user
  /// picked an entry from the combo this frame (i.e. `path` now points at
  /// an existing file).
  bool Draw(const char *id);

  void Refresh();

private:
  std::vector<std::string> entries_; // full paths, sorted
  bool scanned_ = false;
};

} // namespace pacer
