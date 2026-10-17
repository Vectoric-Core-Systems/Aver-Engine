#pragma once
// The process-wide state UiAbi.cpp owns (draw list, viewport, font, pointer), for the widget ABI in
// UiWidgetAbi.cpp to draw into and read. Not installed; the two files are one library.
#include "aver/ui/UiDrawList.hpp"
#include "aver/ui/UiTextEngine.hpp"

#include <cstdint>
#include <string>

namespace aver::ui::abi {

UiDrawList& drawList();
// The borrowed font, or null when none is set or it has no glyphs.
const UiFont* font();
// The process-wide text engine (docs/GAME_UI_TEXT.md); the widget frame paints through it.
UiTextEngine& textEngine();
// The rectangle aver_ui_begin_frame was given.
void viewport(float out[4]);
// The pointer and held buttons the host set this frame.
void pointer(float& x, float& y, std::uint32_t& buttons);
bool frameStarted();
// The host's text resolver (aver_ui_set_text_resolver), shared with aver_ui_text / aver_ui_text_width.
using TextResolveFn = std::int32_t (*)(const char* raw, char* out, std::int32_t cap);
void setTextResolver(TextResolveFn fn);
// True with the resolved text when `raw` is a reference the resolver handles; grows its buffer when truncated.
bool resolveUiText(const char* raw, std::string& out);

} // namespace aver::ui::abi
