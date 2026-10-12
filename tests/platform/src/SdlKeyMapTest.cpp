// SDL scancode -> Win32 virtual-key code, the one table the SDL window backend translates every key
// through. Exit code = failure count.
//
// WHY A WHOLE TEST FOR A TABLE. Every key binding in this engine is a Win32 VK code: InputState is
// indexed by them, framework/InputKeys.hpp maps them to the gameplay ABI, the editor's shortcuts and
// a game's scripts name them. Off Windows the codes come from this table and nowhere else, so a wrong
// row is a key that silently does nothing on Linux and macOS while working on Windows -- and nothing
// at run time would ever say so. Checked against the Win32 values written out by hand, not against
// the table itself.
#include "sdl/SdlKeys.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <SDL3/SDL_scancode.h>

#include <string>

using namespace aver;

static int g_checks = 0;
static int g_failures = 0;

static void expect(SDL_Scancode sc, i32 vk, const char* what) {
    ++g_checks;
    const i32 got = detail::vkFromSdlScancode(static_cast<int>(sc));
    if (got == vk) return;
    AVER_ERROR("   FAIL  {}: scancode {} -> {:#04x}, expected {:#04x}", what, static_cast<int>(sc), got, vk);
    ++g_failures;
}

int main() {
    // Letters and digits: VK codes are the ASCII capitals and digits.
    for (int i = 0; i < 26; ++i)
        expect(static_cast<SDL_Scancode>(SDL_SCANCODE_A + i), 'A' + i, "letter");
    expect(SDL_SCANCODE_0, '0', "digit 0");
    for (int i = 0; i < 9; ++i)
        expect(static_cast<SDL_Scancode>(SDL_SCANCODE_1 + i), '1' + i, "digit");

    // Every key framework/InputKeys.hpp hands to the gameplay ABI.
    expect(SDL_SCANCODE_SPACE,  0x20, "VK_SPACE");
    expect(SDL_SCANCODE_RETURN, 0x0D, "VK_RETURN");
    expect(SDL_SCANCODE_ESCAPE, 0x1B, "VK_ESCAPE");
    expect(SDL_SCANCODE_TAB,    0x09, "VK_TAB");
    expect(SDL_SCANCODE_LEFT,   0x25, "VK_LEFT");
    expect(SDL_SCANCODE_UP,     0x26, "VK_UP");
    expect(SDL_SCANCODE_RIGHT,  0x27, "VK_RIGHT");
    expect(SDL_SCANCODE_DOWN,   0x28, "VK_DOWN");

    // Modifiers arrive as the GENERIC codes, both sides alike, because that is what WM_KEYDOWN and
    // WM_SYSKEYDOWN carry in wParam -- VK_LSHIFT and friends only ever come from GetKeyState.
    expect(SDL_SCANCODE_LSHIFT, 0x10, "left shift -> VK_SHIFT");
    expect(SDL_SCANCODE_RSHIFT, 0x10, "right shift -> VK_SHIFT");
    expect(SDL_SCANCODE_LCTRL,  0x11, "left ctrl -> VK_CONTROL");
    expect(SDL_SCANCODE_RCTRL,  0x11, "right ctrl -> VK_CONTROL");
    expect(SDL_SCANCODE_LALT,   0x12, "left alt -> VK_MENU");
    expect(SDL_SCANCODE_RALT,   0x12, "right alt -> VK_MENU");
    expect(SDL_SCANCODE_LGUI,   0x5B, "VK_LWIN");
    expect(SDL_SCANCODE_RGUI,   0x5C, "VK_RWIN");

    // Function keys, editing and navigation -- the editor binds F1-F12, Delete, Home/End and friends.
    for (int i = 0; i < 12; ++i)
        expect(static_cast<SDL_Scancode>(SDL_SCANCODE_F1 + i), 0x70 + i, "F1-F12");
    expect(SDL_SCANCODE_BACKSPACE, 0x08, "VK_BACK");
    expect(SDL_SCANCODE_DELETE,    0x2E, "VK_DELETE");
    expect(SDL_SCANCODE_INSERT,    0x2D, "VK_INSERT");
    expect(SDL_SCANCODE_HOME,      0x24, "VK_HOME");
    expect(SDL_SCANCODE_END,       0x23, "VK_END");
    expect(SDL_SCANCODE_PAGEUP,    0x21, "VK_PRIOR");
    expect(SDL_SCANCODE_PAGEDOWN,  0x22, "VK_NEXT");

    // US-layout punctuation, by physical position (a scancode is a position, as a VK_OEM_* code is).
    expect(SDL_SCANCODE_GRAVE,        0xC0, "VK_OEM_3 (the console key, `)");
    expect(SDL_SCANCODE_MINUS,        0xBD, "VK_OEM_MINUS");
    expect(SDL_SCANCODE_EQUALS,       0xBB, "VK_OEM_PLUS");
    expect(SDL_SCANCODE_LEFTBRACKET,  0xDB, "VK_OEM_4");
    expect(SDL_SCANCODE_RIGHTBRACKET, 0xDD, "VK_OEM_6");
    expect(SDL_SCANCODE_BACKSLASH,    0xDC, "VK_OEM_5");
    expect(SDL_SCANCODE_SEMICOLON,    0xBA, "VK_OEM_1");
    expect(SDL_SCANCODE_APOSTROPHE,   0xDE, "VK_OEM_7");
    expect(SDL_SCANCODE_COMMA,        0xBC, "VK_OEM_COMMA");
    expect(SDL_SCANCODE_PERIOD,       0xBE, "VK_OEM_PERIOD");
    expect(SDL_SCANCODE_SLASH,        0xBF, "VK_OEM_2");

    // Keypad: digits are VK_NUMPAD0-9, and Enter is VK_RETURN as Windows reports it.
    expect(SDL_SCANCODE_KP_0, 0x60, "VK_NUMPAD0");
    for (int i = 0; i < 9; ++i)
        expect(static_cast<SDL_Scancode>(SDL_SCANCODE_KP_1 + i), 0x61 + i, "VK_NUMPAD1-9");
    expect(SDL_SCANCODE_KP_ENTER,    0x0D, "keypad Enter -> VK_RETURN");
    expect(SDL_SCANCODE_KP_PLUS,     0x6B, "VK_ADD");
    expect(SDL_SCANCODE_KP_MINUS,    0x6D, "VK_SUBTRACT");
    expect(SDL_SCANCODE_KP_MULTIPLY, 0x6A, "VK_MULTIPLY");
    expect(SDL_SCANCODE_KP_DIVIDE,   0x6F, "VK_DIVIDE");
    expect(SDL_SCANCODE_KP_PERIOD,   0x6E, "VK_DECIMAL");

    // Out of range, and a scancode with no Win32 equivalent, are 0 -- which InputState ignores.
    expect(SDL_SCANCODE_UNKNOWN, 0, "SDL_SCANCODE_UNKNOWN");
    ++g_checks;
    if (detail::vkFromSdlScancode(-1) != 0 || detail::vkFromSdlScancode(100000) != 0) {
        AVER_ERROR("   FAIL  an out-of-range scancode must map to 0");
        ++g_failures;
    }

    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
