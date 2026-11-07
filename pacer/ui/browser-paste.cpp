#include "browser-paste.hpp"

#if defined(__EMSCRIPTEN__)

#include <cstdlib>

#include <emscripten/em_js.h>
#include <imgui.h>

// Like the file picker in file-transfer-web.cpp, the paste is not a callback
// into C++: JS parks the text on Module and the render loop takes it at the
// frame boundary.
EM_JS_DEPS(pacer_browser_paste, "$stringToNewUTF8");

namespace {

// Installs the listeners on first use. Returns a malloc'd copy of text
// pasted since the last call, or null. Caller frees.
//
// SDL calls preventDefault() on every Ctrl+key keydown, which suppresses
// the browser's paste event for Ctrl+V. So the paste shortcut is caught on
// the window in the capture phase, before it reaches SDL's listener, and
// kept from SDL entirely; the browser then fires its paste event as usual.
// ImGui never sees the shortcut either, so it can't paste a second time
// from its own clipboard.
EM_JS(char *, PacerJsTakePaste, (), {
  if (!Module.pacerPasteInstalled) {
    Module.pacerPasteInstalled = true;
    window.addEventListener('keydown', function(event) {
      if ((event.ctrlKey || event.metaKey) && !event.altKey &&
          (event.key === 'v' || event.key === 'V'))
        event.stopImmediatePropagation();
    }, true);
    document.addEventListener('paste', function(event) {
      var text = event.clipboardData && event.clipboardData.getData('text/plain');
      if (text) {
        Module.pacerPastedText = (Module.pacerPastedText || '') + text;
        event.preventDefault();
      }
    });
  }
  var text = Module.pacerPastedText;
  if (!text)
    return 0;
  Module.pacerPastedText = null;
  return stringToNewUTF8(text);
});

} // namespace

void pacer::ForwardBrowserPaste() {
  char *text = PacerJsTakePaste();
  if (text == nullptr)
    return;
  // Typed into whichever text field is active; ignored when none is.
  // Single-line fields drop any newline.
  ImGui::GetIO().AddInputCharactersUTF8(text);
  std::free(text);
}

#else

void pacer::ForwardBrowserPaste() {}

#endif
