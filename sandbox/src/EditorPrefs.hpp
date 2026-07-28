#pragma once
// Editor UI state that should outlive a session.
//
// THE EDITOR HAD NOWHERE TO PUT THIS. ImGui's own persistence is deliberately off -- the backend sets
// `io.IniFilename = nullptr` so nothing writes an imgui.ini into the working directory, and the dock
// layout is rebuilt in code every run. That is the right call for the dock layout, which is a
// designed default rather than something to inherit from whatever state a previous session happened
// to end in. It left no home at all for the smaller things a user adjusts and expects to stay put.
//
// WHAT BELONGS HERE: preferences and UI geometry. A column width, a panel's last size, a toggle the
// user set. Small, per-user, and harmless to lose -- every read takes a fallback, so a missing or
// corrupt file is a fresh-looking editor rather than a broken one.
//
// WHAT DOES NOT: anything about a PROJECT. A project's settings live in the project, or two people
// opening the same project would see different engines. And anything the editor cannot rebuild from
// scratch -- this file is cache-grade by design and is not backed up, not versioned, and not
// migrated.
//
// FORMAT: `key=value`, one per line, `#` for comments. Chosen over JSON because the whole content is
// a handful of scalars, because a human debugging a layout should be able to read and edit it, and
// because a parser that cannot fail in interesting ways is one less thing to get wrong.
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>

namespace aver::editor {

// Reads the file if it has not been read yet. Every accessor does this itself, so there is no
// initialisation order to get right and no "did somebody call load()" bug to have.
f32  prefFloat(std::string_view key, f32 fallback);
void setPrefFloat(std::string_view key, f32 value);

bool prefBool(std::string_view key, bool fallback);
void setPrefBool(std::string_view key, bool value);

i32  prefInt(std::string_view key, i32 fallback);
void setPrefInt(std::string_view key, i32 value);

// Strings are stored verbatim and therefore must not contain a newline -- one would split the entry
// into two lines and the second would be discarded as having no '='. Callers here store names and
// short identifiers, so the restriction costs nothing; a setter that had to escape would need a
// parser that unescapes, and neither is worth it for the content this file actually holds.
std::string prefString(std::string_view key, std::string_view fallback);
void setPrefString(std::string_view key, std::string_view value);

// Write, if anything changed since the last write. Cheap to call when nothing has.
//
// EXPLICIT rather than written on every set, because a caller that sets a value per frame -- which a
// dragged splitter would, if it saved while dragging rather than when the drag ends -- would
// otherwise write the file sixty times a second. Callers save on a settling edge; the app also
// flushes at shutdown so a value changed and never settled is still kept.
void flushEditorPrefs();

// Where the file is, for a log line or a diagnostic. Empty before the first access.
const std::string& editorPrefsPath();

} // namespace aver::editor
