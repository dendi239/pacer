#pragma once

namespace pacer {

/// Browser builds: forwards the page's paste events (Cmd+V / Ctrl+V) into
/// ImGui text fields, which otherwise only ever see ImGui's own in-memory
/// clipboard -- a wasm app cannot read the system clipboard synchronously.
/// Call once per frame from PreNewFrame, i.e. before ImGui::NewFrame
/// handles that frame's key presses. No-op on desktop, where the backend
/// reads the system clipboard itself.
void ForwardBrowserPaste();

} // namespace pacer
