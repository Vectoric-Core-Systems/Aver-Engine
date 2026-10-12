#pragma once
#include "aver/core/Types.hpp"

namespace aver::detail {

// The Win32 virtual-key code for an SDL_Scancode, so every key binding in the engine -- InputState,
// framework/InputKeys.hpp, the editor's shortcuts -- reads the same numbers on every platform. 0 for a
// scancode with no Win32 equivalent, or one out of range; InputState ignores 0.
//
// By SCANCODE, a physical key position, not by keycode: VK_OEM_* codes are positions on a US layout
// too, and a game's WASD must stay WASD on an AZERTY keyboard exactly as it does on Windows.
//
// Takes an int, not an SDL_Scancode, so its one test needs SDL's headers and nothing else.
i32 vkFromSdlScancode(int scancode);

} // namespace aver::detail
