#pragma once

#include <string>
#include <vector>

namespace pacer {

// Native open/save dialogs. Where the platform has one, this is how the
// user names a file: typing a path is a fallback, not the interface.
//
// The calls are modal and return once the user is done -- unlike the
// browser's picker in file-transfer.hpp, which cannot block and hands the
// pick back frames later. Callers that support both worlds ask
// HasNativeFileDialog() first.

/// One entry of a dialog's file-type filter, e.g.
/// {"Recordings", {"MP4", "mp4", "dat"}} -- extensions, without the dot.
struct FileDialogFilter {
  std::string name;
  std::vector<std::string> extensions;
};

/// True where OpenFileDialog/SaveFileDialog put up a real dialog. False in
/// the browser and on platforms with no implementation here, where they
/// return empty and the UI has to offer a path field instead.
bool HasNativeFileDialog();

/// Asks the user for one existing file. `start_path` seeds the dialog's
/// directory and, if it names a file, its selection. Returns the chosen
/// path, or empty if the user cancelled.
std::string OpenFileDialog(const std::string &title,
                           const std::vector<FileDialogFilter> &filters,
                           const std::string &start_path = {});

/// Same, but the user may pick several files at once (GoPro splits one run
/// into several clips). Returns them in the order the dialog reports, or
/// empty on cancel.
std::vector<std::string>
OpenFilesDialog(const std::string &title,
                const std::vector<FileDialogFilter> &filters,
                const std::string &start_path = {});

/// Asks the user where to write a file. `start_path` seeds the directory
/// and the suggested name. Returns the chosen path, or empty on cancel.
std::string SaveFileDialog(const std::string &title,
                           const std::vector<FileDialogFilter> &filters,
                           const std::string &start_path = {});

} // namespace pacer
