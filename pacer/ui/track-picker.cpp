#include "track-picker.hpp"

#include <algorithm>
#include <filesystem>

#include <imgui.h>
#include <imgui_stdlib.h>

void pacer::TrackFilePicker::Refresh() {
  entries_.clear();
  std::error_code ec;
  for (const auto &entry :
       std::filesystem::directory_iterator(directory, ec)) {
    if (entry.is_regular_file() && entry.path().extension() == ".json") {
      entries_.push_back(entry.path().string());
    }
  }
  std::sort(entries_.begin(), entries_.end());
  scanned_ = true;
}

bool pacer::TrackFilePicker::Draw(const char *id) {
  if (!scanned_)
    Refresh();

  ImGui::PushID(id);

  std::string preview = "Pick a track...";
  for (const auto &entry : entries_) {
    if (entry == path) {
      preview = std::filesystem::path(entry).stem().string();
      break;
    }
  }

  bool picked = false;
  ImGui::SetNextItemWidth(-70.0f);
  if (ImGui::BeginCombo("##tracks", preview.c_str())) {
    for (int i = 0; i < (int)entries_.size(); ++i) {
      ImGui::PushID(i);
      std::string label = std::filesystem::path(entries_[i]).stem().string();
      if (ImGui::Selectable(label.c_str(), entries_[i] == path)) {
        path = entries_[i];
        picked = true;
      }
      ImGui::PopID();
    }
    if (entries_.empty()) {
      ImGui::TextDisabled("No .json files in '%s'", directory.c_str());
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  if (ImGui::Button("Refresh")) {
    Refresh();
  }
  ImGui::SetNextItemWidth(-1);
  ImGui::InputText("##path", &path);

  ImGui::PopID();
  return picked;
}
