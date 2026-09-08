// macOS file dialogs, over AppKit's NSOpenPanel/NSSavePanel.
//
// runModal blocks until the user is done, which is exactly what the calling
// UI code wants: the pick is handled inside the frame that asked for it,
// with no state machine spanning frames. It is safe here because the render
// loop runs on the main thread, where AppKit requires panels to be shown.

#include "file-dialog.hpp"

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <filesystem>

namespace {

/// Translates the extension filters into the content types the panel takes.
/// Returns nil when nothing usable came through, which leaves the panel
/// accepting every file -- better than a dialog that shows nothing.
NSArray<UTType *> *ContentTypes(
    const std::vector<pacer::FileDialogFilter> &filters) {
  NSMutableArray<UTType *> *types = [NSMutableArray array];
  for (const auto &filter : filters) {
    for (const auto &extension : filter.extensions) {
      UTType *type = [UTType typeWithFilenameExtension:
                          @(extension.c_str())];
      if (type != nil && ![types containsObject:type]) {
        [types addObject:type];
      }
    }
  }
  return types.count > 0 ? types : nil;
}

/// AppKit needs an NSApplication before a panel can be shown. A GUI host
/// has made one long before this runs; a plain command-line host has not,
/// and would otherwise put up a panel nobody can see.
void EnsureApplication() {
  [NSApplication sharedApplication];
}

/// Points the panel at `start_path`: its directory, plus its filename when
/// it names one. A path that does not exist yet still seeds the name, which
/// is what a "save as" wants.
void SeedLocation(NSSavePanel *panel, const std::string &start_path) {
  if (start_path.empty()) {
    return;
  }
  std::filesystem::path path = std::filesystem::absolute(start_path);
  std::error_code ec;
  bool is_directory = std::filesystem::is_directory(path, ec);
  std::filesystem::path directory = is_directory ? path : path.parent_path();
  if (!directory.empty() && std::filesystem::exists(directory, ec)) {
    panel.directoryURL =
        [NSURL fileURLWithPath:@(directory.c_str()) isDirectory:YES];
  }
  if (!is_directory && path.has_filename()) {
    panel.nameFieldStringValue = @(path.filename().c_str());
  }
}

std::vector<std::string> RunOpenPanel(
    const std::string &title,
    const std::vector<pacer::FileDialogFilter> &filters,
    const std::string &start_path, bool allow_multiple) {
  @autoreleasepool {
    EnsureApplication();
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.message = @(title.c_str());
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = allow_multiple;
    if (NSArray<UTType *> *types = ContentTypes(filters)) {
      panel.allowedContentTypes = types;
    }
    SeedLocation(panel, start_path);

    if ([panel runModal] != NSModalResponseOK) {
      return {};
    }
    std::vector<std::string> paths;
    for (NSURL *url in panel.URLs) {
      if (url.fileSystemRepresentation != nullptr) {
        paths.emplace_back(url.fileSystemRepresentation);
      }
    }
    return paths;
  }
}

} // namespace

namespace pacer {

bool HasNativeFileDialog() { return true; }

std::string OpenFileDialog(const std::string &title,
                           const std::vector<FileDialogFilter> &filters,
                           const std::string &start_path) {
  std::vector<std::string> paths =
      RunOpenPanel(title, filters, start_path, /*allow_multiple=*/false);
  return paths.empty() ? std::string{} : paths.front();
}

std::vector<std::string>
OpenFilesDialog(const std::string &title,
                const std::vector<FileDialogFilter> &filters,
                const std::string &start_path) {
  return RunOpenPanel(title, filters, start_path, /*allow_multiple=*/true);
}

std::string SaveFileDialog(const std::string &title,
                           const std::vector<FileDialogFilter> &filters,
                           const std::string &start_path) {
  @autoreleasepool {
    EnsureApplication();
    NSSavePanel *panel = [NSSavePanel savePanel];
    panel.message = @(title.c_str());
    if (NSArray<UTType *> *types = ContentTypes(filters)) {
      panel.allowedContentTypes = types;
    }
    SeedLocation(panel, start_path);

    if ([panel runModal] != NSModalResponseOK || panel.URL == nil) {
      return {};
    }
    const char *path = panel.URL.fileSystemRepresentation;
    return path != nullptr ? std::string(path) : std::string{};
  }
}

} // namespace pacer
