#pragma once
// Editor UI state that outlives a session: per-user preferences and UI geometry, in a `key=value`
// file. Cache-grade — every read takes a fallback, and nothing about a project belongs here.
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>

namespace aver::editor {

// Reads a float setting, or `fallback`. Loads the file on first use.
f32  prefFloat(std::string_view key, f32 fallback);
// Stores a float setting.
void setPrefFloat(std::string_view key, f32 value);

// Reads a bool setting, or `fallback`.
bool prefBool(std::string_view key, bool fallback);
// Stores a bool setting.
void setPrefBool(std::string_view key, bool value);

// Reads an int setting, or `fallback`.
i32  prefInt(std::string_view key, i32 fallback);
// Stores an int setting.
void setPrefInt(std::string_view key, i32 value);

// Reads a string setting, or `fallback`.
std::string prefString(std::string_view key, std::string_view fallback);
// Stores a string setting. A value containing a newline is refused, not escaped.
void setPrefString(std::string_view key, std::string_view value);

// Writes the file if anything changed. Explicit, so a per-frame setter does not write per frame.
void flushEditorPrefs();

// Where the file is. Empty before the first access.
const std::string& editorPrefsPath();

// THE DECISION THAT KEEPS A BAD READ FROM DESTROYING A GOOD FILE, as a pure function so it can be
// tested. `readFileText` returns false for a file that is absent AND for one that exists but could
// not be opened, and those two need opposite answers:
//
//   absent      -- the ordinary first run. The store is legitimately empty and MUST be allowed to
//                  write, or preferences could never be created at all.
//   unreadable  -- a lock, a permissions change, a network share that blinked. The store is empty
//                  only because the read failed, and writing it back would replace a good file with
//                  a header and nothing else, atomically and irreversibly.
//
// Returns true when the store must refuse to write for the rest of the session.
bool prefsShouldRefuseWrite(bool readSucceeded, bool fileExists);

// True when this session latched read-only because the file existed and could not be read. The
// editor surfaces this; nothing else should have to care.
bool editorPrefsReadOnly();

// THE MERGE RULE FOR RECOVERING FROM READ-ONLY, pulled out as its own pure decision for the same
// reason prefsShouldRefuseWrite above is: a read-only store's file can become readable again mid-
// session (see the .cpp's tryRecoverReadOnly), and the moment it does, every key already in memory
// and every key still only on disk have to be reconciled into one map before anything gets written.
//
// SESSION VALUES WIN. `sessionAlreadyHasKey` is true for a key this session set through setPref*
// while the store thought it had nothing on disk to lose -- adopting the file's older value for
// that key instead would silently discard the very edit the user is waiting to see saved, which is
// the read-only bug coming back wearing a recovery feature as a disguise. A key the session never
// touched has no such edit to protect, so the file's copy of it is exactly what recovering means to
// restore.
bool prefsShouldAdoptFromFile(bool sessionAlreadyHasKey);

} // namespace aver::editor
