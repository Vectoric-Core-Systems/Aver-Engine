// The InputState -> aver_fw_* bridge.
#pragma once
#include "aver/core/Types.hpp"

#include <string>

namespace aver {
class InputState;
}

namespace aver::game {

#if AVER_MODULE_FRAMEWORK
// Publishes this frame's accumulated input into the framework's input state.
//
// `focused` gates publication: an unfocused window publishes nothing at all, rather than publishing
// the last state it saw.
//
// `captured`, when true, feeds `capturedDx`/`capturedDy` into the framework's mouse delta INSTEAD OF
// `in`'s own window-accumulated deltas -- see GameApp::pollCapturedMouse for where those come from
// and why: `in.mouseDX()`/`mouseDY()` accumulate WM_MOUSEMOVE, which stops moving the instant the OS
// cursor (invisible, but still real) reaches the clip rect's edge, so a player looking hard in one
// direction would run out of travel after a few hundred pixels of window width. The captured deltas
// instead come from re-centring the (hidden, confined) cursor every frame, which has no edge to hit.
// Meaningless, and ignored, when `focused` is false -- the unfocused branch already publishes an
// explicit 0,0 for the same reason every other slot does.
//
// `echo`, when non-null, receives a human-readable list of the keys held, so --input-echo can prove
// the path end to end without a debugger.
void publishInput(const InputState& in, bool focused, bool captured, f32 capturedDx, f32 capturedDy,
                   std::string* echo = nullptr);
#endif

} // namespace aver::game
