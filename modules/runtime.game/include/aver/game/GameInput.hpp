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
// the last state it saw. `echo`, when non-null, receives a human-readable list of the keys held, so
// --input-echo can prove the path end to end without a debugger.
void publishInput(const InputState& in, bool focused, std::string* echo = nullptr);
#endif

} // namespace aver::game
