#pragma once

#include <string>

namespace pacer {

// Moving files in and out of the app, on platforms that disagree about
// whether the app may touch the filesystem at all.
//
// On desktop a path is a path: the app writes and reads it directly. In the
// browser there is no user-visible filesystem to write to, so a "save" is a
// download and an "open" is a file picker the page puts up -- and the pick
// is asynchronous, so it cannot be a blocking call that returns the file.
// These four functions are the smallest interface that both worlds fit.

/// True where the app must go through the host to exchange files with the
/// user (the browser), rather than reading and writing paths itself. Callers
/// use it to decide whether an explicit "Open file..." affordance is needed
/// alongside a path field.
bool NeedsHostFileTransfer();

/// Hands `contents` to the user as a file named `suggested_name`. On desktop
/// this writes to that path; in the browser it starts a download of the
/// basename, wherever the browser has been told to put downloads.
///
/// Returns false only if the app could tell the transfer failed. A browser
/// download leaves the app's hands the moment it starts, so it always
/// reports success.
bool OfferFileToUser(const std::string &suggested_name,
                     const std::string &contents);

/// Asks the host to let the user pick a file; `accept` is an HTML accept
/// list, e.g. ".json". Returns immediately -- the chosen file turns up at
/// TakeReceivedFile() a few frames later. No-op where
/// NeedsHostFileTransfer() is false.
///
/// Call this from a frame that is handling a click: browsers only open a
/// file picker while the page still holds a fresh user activation.
void RequestFileFromUser(const char *accept);

/// Path of a file the user has handed over since the last call, or empty if
/// there is none. The file lives in the app's own filesystem and reads back
/// with the ordinary file APIs. Poll once a frame.
std::string TakeReceivedFile();

} // namespace pacer
