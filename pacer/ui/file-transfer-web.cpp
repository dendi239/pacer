// Browser file transfer. A wasm app has no filesystem the user can see, so
// saving means handing the bytes to the page as a download, and opening
// means asking the page for a file picker and waiting.
//
// The picker is deliberately not a callback into C++. JS drops the chosen
// file into the in-memory filesystem and parks its path on Module; the
// render loop picks it up on a later frame via TakeReceivedFile(). That
// keeps every mutation of app state on the frame boundary, where the rest
// of the UI code already expects it, instead of re-entering mid-frame from
// a FileReader event.

#include "file-transfer.hpp"

#include <cstdlib>

#include <emscripten.h>

// EM_JS bodies below use these JS-side helpers, which are only linked in if
// something declares a dependency on them.
EM_JS_DEPS(pacer_file_transfer, "$UTF8ToString,$stringToNewUTF8");

namespace {

EM_JS(void, PacerJsDownloadFile,
      (const char *name, const char *data, int length), {
        // Copy out of the wasm heap: the Blob must not alias memory that
        // keeps changing (or moves, if the heap grows) while the browser
        // writes the download.
        var bytes = new Uint8Array(HEAPU8.subarray(data, data + length));
        var url = URL.createObjectURL(new Blob([bytes], {type : 'application/json'}));
        var link = document.createElement('a');
        link.href = url;
        link.download = UTF8ToString(name);
        document.body.appendChild(link);
        link.click();
        document.body.removeChild(link);
        URL.revokeObjectURL(url);
      });

EM_JS(void, PacerJsRequestFile, (const char *accept), {
  var input = document.createElement('input');
  input.type = 'file';
  input.accept = UTF8ToString(accept);
  input.style.display = 'none';
  input.onchange = function(event) {
    var file = event.target.files[0];
    document.body.removeChild(input);
    if (!file) {
      return;
    }
    var reader = new FileReader();
    reader.onload = function() {
      try {
        FS.mkdir('/uploads');
      } catch (err) {
        // Already there from an earlier pick; that is the normal case.
      }
      var path = '/uploads/' + file.name;
      FS.writeFile(path, new Uint8Array(reader.result));
      Module.pacerReceivedFile = path;
    };
    reader.readAsArrayBuffer(file);
  };
  document.body.appendChild(input);
  input.click();
});

// Returns a malloc'd copy of the pending path, or null when nothing has
// been picked since the last call. Caller frees.
EM_JS(char *, PacerJsTakeReceivedFile, (), {
  var path = Module.pacerReceivedFile;
  if (!path) {
    return 0;
  }
  Module.pacerReceivedFile = null;
  return stringToNewUTF8(path);
});

// The browser only shows the basename in its download UI anyway, and a
// suggested name carrying a directory ("tracks/llandow.json") would be
// mangled differently by different browsers. Strip it ourselves.
std::string Basename(const std::string &path) {
  size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

namespace pacer {

bool NeedsHostFileTransfer() { return true; }

bool OfferFileToUser(const std::string &suggested_name,
                     const std::string &contents) {
  std::string name = Basename(suggested_name);
  if (name.empty()) {
    name = "track.json";
  }
  PacerJsDownloadFile(name.c_str(), contents.data(),
                      static_cast<int>(contents.size()));
  // Once the download starts it belongs to the browser; whether the user
  // keeps it is not something the app gets told.
  return true;
}

void RequestFileFromUser(const char *accept) { PacerJsRequestFile(accept); }

std::string TakeReceivedFile() {
  char *path = PacerJsTakeReceivedFile();
  if (path == nullptr) {
    return {};
  }
  std::string result(path);
  std::free(path);
  return result;
}

} // namespace pacer
