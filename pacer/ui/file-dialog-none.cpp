// Platforms with no native dialog wired up here -- including the browser,
// where a pick cannot block and goes through file-transfer.hpp instead.
// Callers check HasNativeFileDialog() and fall back to a path field.

#include "file-dialog.hpp"

namespace pacer {

bool HasNativeFileDialog() { return false; }

std::string OpenFileDialog(const std::string &,
                           const std::vector<FileDialogFilter> &,
                           const std::string &) {
  return {};
}

std::vector<std::string> OpenFilesDialog(const std::string &,
                                         const std::vector<FileDialogFilter> &,
                                         const std::string &) {
  return {};
}

std::string SaveFileDialog(const std::string &,
                           const std::vector<FileDialogFilter> &,
                           const std::string &) {
  return {};
}

} // namespace pacer
