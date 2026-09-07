// Desktop file transfer: the app owns the filesystem, so "offer a file to
// the user" is just a write, and there is nothing to receive -- the path
// field in the UI already names any file the user wants opened. See
// file-transfer-web.cpp for the browser, where neither is true.

#include "file-transfer.hpp"

#include <fstream>

namespace pacer {

bool NeedsHostFileTransfer() { return false; }

bool OfferFileToUser(const std::string &suggested_name,
                     const std::string &contents) {
  std::ofstream file(suggested_name);
  if (!file.is_open()) {
    return false;
  }
  file << contents;
  return file.good();
}

void RequestFileFromUser(const char * /*accept*/) {}

std::string TakeReceivedFile() { return {}; }

} // namespace pacer
