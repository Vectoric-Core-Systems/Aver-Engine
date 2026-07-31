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

} // namespace aver::editor
