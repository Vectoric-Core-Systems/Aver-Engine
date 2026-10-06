#pragma once
// The process-wide state UiAbi.cpp owns (draw list, viewport, font, pointer), for the widget ABI in
// UiWidgetAbi.cpp to draw into and read. Not installed; the two files are one library.
#include "aver/ui/UiDrawList.hpp"

#include <cstdint>

namespace aver::ui::abi {

UiDrawList& drawList();
// The borrowed font, or null when none is set or it has no glyphs.
const UiFont* font();
// The rectangle aver_ui_begin_frame was given.
void viewport(float out[4]);
// The pointer and held buttons the host set this frame.
void pointer(float& x, float& y, std::uint32_t& buttons);
bool frameStarted();

} // namespace aver::ui::abi
