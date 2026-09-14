// Win32 virtual key <-> AVER_FW_KEY_*, in ONE place.
//
// WHY THIS IS A HEADER AND NOT A STATIC IN SOMEBODY'S .cpp. This table lived in an anonymous
// namespace inside modules/runtime.game/src/GameInput.cpp, where exactly one host could reach it.
// The editor -- which is the only way a project actually runs (there is no standalone game
// executable; see sandbox/CMakeLists.txt) -- could not, so when the editor started publishing input
// from the same OS event stream it would have had to write the same mapping out a second time.
// Sandbox's own CMakeLists.txt already complains about precisely this shape for the world runtime:
// "turning a .ocworld placement into an entity used to be written out twice, and every future change
// to it would have had to be made twice and stay agreeing." A key mapping that disagrees between two
// hosts is worse than most: it does not fail to build, it makes one key work in the editor and not
// in a shipped game, and nothing logs.
//
// Header-only and inline because it is a pure function of its argument -- no state, no allocation,
// nothing to link. It belongs beside the enum it maps onto rather than beside either caller.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/framework/framework_abi.h"

namespace aver::fw {

// Win32 virtual key -> framework key. Returns -1 for keys the framework has no slot for.
//
// VK_SHIFT/VK_CONTROL/VK_MENU and NOT VK_LSHIFT/VK_LCONTROL/VK_LMENU: WM_KEYDOWN delivers the
// UNSIDED code unless the receiver does the extended-key dance, so mapping the sided ones would
// produce a Shift key that never registers. The framework's slot is named LSHIFT but means "shift".
//
// MOST OF THE VK RANGE FALLS THROUGH TO -1, and that is a real limitation rather than an oversight:
// the framework enum has 46 slots and Win32 has 256 codes, so F-keys, the numpad and every OEM key
// are simply unreachable by gameplay today. The enum cannot be renumbered to fix it -- the InputKey
// graph node takes a literal integer, so saved graphs depend on the current numbering -- which is
// why the raw-VK ABI exists alongside this rather than replacing it.
inline i32 frameworkKeyFromVk(i32 vk) {
    if (vk >= 'A' && vk <= 'Z') return AVER_FW_KEY_A + (vk - 'A');
    if (vk >= '0' && vk <= '9') return AVER_FW_KEY_0 + (vk - '0');
    switch (vk) {
        case 0x20: return AVER_FW_KEY_SPACE;    // VK_SPACE
        case 0x10: return AVER_FW_KEY_LSHIFT;   // VK_SHIFT
        case 0x11: return AVER_FW_KEY_LCTRL;    // VK_CONTROL
        case 0x12: return AVER_FW_KEY_LALT;     // VK_MENU
        case 0x0D: return AVER_FW_KEY_ENTER;    // VK_RETURN
        case 0x1B: return AVER_FW_KEY_ESCAPE;   // VK_ESCAPE
        case 0x09: return AVER_FW_KEY_TAB;      // VK_TAB
        case 0x25: return AVER_FW_KEY_LEFT;
        case 0x26: return AVER_FW_KEY_UP;
        case 0x27: return AVER_FW_KEY_RIGHT;
        case 0x28: return AVER_FW_KEY_DOWN;
        default:   return -1;
    }
}

// The framework key a mouse button publishes as. Buttons live at the end of the same enum, which is
// why InputKey can read a mouse button with no new node -- see GraphNodeDefs' own comment.
inline i32 frameworkKeyFromMouseButton(i32 b) {
    switch (b) {
        case 0:  return AVER_FW_KEY_MOUSE_LEFT;
        case 1:  return AVER_FW_KEY_MOUSE_RIGHT;
        case 2:  return AVER_FW_KEY_MOUSE_MIDDLE;
        default: return -1;
    }
}

// Human-readable name for a framework key, for --input-echo and for any diagnostic that has to name
// the offending slot rather than just its index.
inline const char* frameworkKeyName(i32 k) {
    static const char* kNames[] = {
        "A","B","C","D","E","F","G","H","I","J","K","L","M",
        "N","O","P","Q","R","S","T","U","V","W","X","Y","Z",
        "0","1","2","3","4","5","6","7","8","9",
        "SPACE","LSHIFT","LCTRL","LALT","ENTER","ESCAPE","TAB",
        "LEFT","RIGHT","UP","DOWN","MOUSE_LEFT","MOUSE_RIGHT","MOUSE_MIDDLE",
    };
    if (k < 0 || k >= AVER_FW_KEY_COUNT) return "?";
    return kNames[k];
}

} // namespace aver::fw
