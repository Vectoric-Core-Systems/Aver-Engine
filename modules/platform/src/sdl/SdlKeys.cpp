#include "SdlKeys.hpp"

#include <SDL3/SDL_scancode.h>

#include <array>

namespace aver::detail {
namespace {

// Win32 virtual-key codes, by value. Spelled out here because <windows.h> is exactly what this file
// exists to do without; the names match winuser.h so a row can be checked against MSDN by eye.
enum : i32 {
    VK_BACK = 0x08, VK_TAB = 0x09, VK_RETURN = 0x0D, VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_MENU = 0x12,
    VK_PAUSE = 0x13, VK_CAPITAL = 0x14, VK_ESCAPE = 0x1B, VK_SPACE = 0x20, VK_PRIOR = 0x21, VK_NEXT = 0x22,
    VK_END = 0x23, VK_HOME = 0x24, VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28,
    VK_SNAPSHOT = 0x2C, VK_INSERT = 0x2D, VK_DELETE = 0x2E, VK_LWIN = 0x5B, VK_RWIN = 0x5C, VK_APPS = 0x5D,
    VK_NUMPAD0 = 0x60, VK_MULTIPLY = 0x6A, VK_ADD = 0x6B, VK_SUBTRACT = 0x6D, VK_DECIMAL = 0x6E,
    VK_DIVIDE = 0x6F, VK_F1 = 0x70, VK_F13 = 0x7C, VK_NUMLOCK = 0x90, VK_SCROLL = 0x91,
    VK_OEM_1 = 0xBA, VK_OEM_PLUS = 0xBB, VK_OEM_COMMA = 0xBC, VK_OEM_MINUS = 0xBD, VK_OEM_PERIOD = 0xBE,
    VK_OEM_2 = 0xBF, VK_OEM_3 = 0xC0, VK_OEM_4 = 0xDB, VK_OEM_5 = 0xDC, VK_OEM_6 = 0xDD, VK_OEM_7 = 0xDE,
    VK_OEM_102 = 0xE2,
};

// Built once: SDL_SCANCODE_COUNT entries, 0 for every scancode Windows has no code for.
std::array<i32, SDL_SCANCODE_COUNT> buildTable() {
    std::array<i32, SDL_SCANCODE_COUNT> t{};
    for (int i = 0; i < 26; ++i) t[SDL_SCANCODE_A + i] = 'A' + i;
    for (int i = 0; i < 9; ++i) t[SDL_SCANCODE_1 + i] = '1' + i;
    t[SDL_SCANCODE_0] = '0';

    t[SDL_SCANCODE_RETURN] = VK_RETURN;
    t[SDL_SCANCODE_ESCAPE] = VK_ESCAPE;
    t[SDL_SCANCODE_BACKSPACE] = VK_BACK;
    t[SDL_SCANCODE_TAB] = VK_TAB;
    t[SDL_SCANCODE_SPACE] = VK_SPACE;

    t[SDL_SCANCODE_MINUS] = VK_OEM_MINUS;
    t[SDL_SCANCODE_EQUALS] = VK_OEM_PLUS;
    t[SDL_SCANCODE_LEFTBRACKET] = VK_OEM_4;
    t[SDL_SCANCODE_RIGHTBRACKET] = VK_OEM_6;
    t[SDL_SCANCODE_BACKSLASH] = VK_OEM_5;
    t[SDL_SCANCODE_NONUSHASH] = VK_OEM_5;      // the ISO key left of Enter; Windows reports OEM_5 there too
    t[SDL_SCANCODE_SEMICOLON] = VK_OEM_1;
    t[SDL_SCANCODE_APOSTROPHE] = VK_OEM_7;
    t[SDL_SCANCODE_GRAVE] = VK_OEM_3;
    t[SDL_SCANCODE_COMMA] = VK_OEM_COMMA;
    t[SDL_SCANCODE_PERIOD] = VK_OEM_PERIOD;
    t[SDL_SCANCODE_SLASH] = VK_OEM_2;
    t[SDL_SCANCODE_NONUSBACKSLASH] = VK_OEM_102;
    t[SDL_SCANCODE_CAPSLOCK] = VK_CAPITAL;

    for (int i = 0; i < 12; ++i) t[SDL_SCANCODE_F1 + i] = VK_F1 + i;
    for (int i = 0; i < 12; ++i) t[SDL_SCANCODE_F13 + i] = VK_F13 + i;

    t[SDL_SCANCODE_PRINTSCREEN] = VK_SNAPSHOT;
    t[SDL_SCANCODE_SCROLLLOCK] = VK_SCROLL;
    t[SDL_SCANCODE_PAUSE] = VK_PAUSE;
    t[SDL_SCANCODE_INSERT] = VK_INSERT;
    t[SDL_SCANCODE_HOME] = VK_HOME;
    t[SDL_SCANCODE_PAGEUP] = VK_PRIOR;
    t[SDL_SCANCODE_DELETE] = VK_DELETE;
    t[SDL_SCANCODE_END] = VK_END;
    t[SDL_SCANCODE_PAGEDOWN] = VK_NEXT;
    t[SDL_SCANCODE_RIGHT] = VK_RIGHT;
    t[SDL_SCANCODE_LEFT] = VK_LEFT;
    t[SDL_SCANCODE_DOWN] = VK_DOWN;
    t[SDL_SCANCODE_UP] = VK_UP;

    t[SDL_SCANCODE_NUMLOCKCLEAR] = VK_NUMLOCK;
    t[SDL_SCANCODE_KP_DIVIDE] = VK_DIVIDE;
    t[SDL_SCANCODE_KP_MULTIPLY] = VK_MULTIPLY;
    t[SDL_SCANCODE_KP_MINUS] = VK_SUBTRACT;
    t[SDL_SCANCODE_KP_PLUS] = VK_ADD;
    t[SDL_SCANCODE_KP_ENTER] = VK_RETURN;      // Windows sets only the extended-key bit to tell them apart
    t[SDL_SCANCODE_KP_0] = VK_NUMPAD0;
    for (int i = 0; i < 9; ++i) t[SDL_SCANCODE_KP_1 + i] = VK_NUMPAD0 + 1 + i;
    t[SDL_SCANCODE_KP_PERIOD] = VK_DECIMAL;
    t[SDL_SCANCODE_APPLICATION] = VK_APPS;

    // The generic codes, both sides alike: WM_KEYDOWN/WM_SYSKEYDOWN carry VK_SHIFT, VK_CONTROL and
    // VK_MENU in wParam, never VK_LSHIFT and friends, and every binding in the tree reads those.
    t[SDL_SCANCODE_LSHIFT] = VK_SHIFT;
    t[SDL_SCANCODE_RSHIFT] = VK_SHIFT;
    t[SDL_SCANCODE_LCTRL] = VK_CONTROL;
    t[SDL_SCANCODE_RCTRL] = VK_CONTROL;
    t[SDL_SCANCODE_LALT] = VK_MENU;
    t[SDL_SCANCODE_RALT] = VK_MENU;
    t[SDL_SCANCODE_LGUI] = VK_LWIN;
    t[SDL_SCANCODE_RGUI] = VK_RWIN;
    return t;
}

const std::array<i32, SDL_SCANCODE_COUNT>& table() {
    static const std::array<i32, SDL_SCANCODE_COUNT> t = buildTable();
    return t;
}

}  // namespace

i32 vkFromSdlScancode(int scancode) {
    if (scancode <= 0 || scancode >= SDL_SCANCODE_COUNT) return 0;
    return table()[static_cast<usize>(scancode)];
}

} // namespace aver::detail
