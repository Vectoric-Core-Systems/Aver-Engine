// The ImGui-free input path, end to end: OS event -> InputState -> aver_fw_* -> read back.
//
// WHY THIS TEST EXISTS. The defect it guards is not hypothetical. Until this series, the only code
// in the tree that turned OS input into gameplay input was SandboxApp::pushInput, whose entire body
// is inside `#if AVER_WITH_IMGUI` and reads ImGui::IsKeyDown. A build with AVER_ENABLE_UI=OFF --
// which is exactly what a shipped game is -- had NO INPUT AT ALL, and nothing failed to compile to
// say so. A game that starts, renders and ignores the keyboard is the worst kind of regression:
// everything looks fine.
//
// So the assertion is deliberately the whole chain rather than the mapping function alone. It feeds
// synthetic window events, publishes, and reads the framework's own state back through the same C
// ABI gameplay uses. No window, no device, no ImGui, no human.
#include "aver/game/GameInput.hpp"
#include "aver/platform/InputState.hpp"
#include "aver/framework/framework_abi.h"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static Event key(i32 vk, bool down) {
    Event e; e.type = EventType::Key; e.key = vk; e.pressed = down; return e;
}

// Publishes and returns the echo string, the same one --input-echo logs.
static std::string publish(const InputState& in, bool focused = true) {
    std::string echo;
    game::publishInput(in, focused, &echo);
    return echo;
}

int main() {
    AVER_INFO("InputBridgeTest");

    constexpr i32 kVkW = 'W';
    constexpr i32 kVkShift = 0x10;   // VK_SHIFT, which is what WM_KEYDOWN actually delivers
    constexpr i32 kVkA = 'A';

    // ---- the sequence the lift plan names as the proof ----
    {
        AVER_INFO("W, then W+Shift, then W, then nothing");
        InputState in;

        in.newFrame(); in.onEvent(key(kVkW, true));
        check(publish(in) == "W", "holding W publishes exactly W");
        check(aver_fw_input_key(AVER_FW_KEY_W) != 0, "the framework reads W as held");
        check(aver_fw_input_key(AVER_FW_KEY_A) == 0, "and reads A as not held");

        in.newFrame(); in.onEvent(key(kVkShift, true));
        check(publish(in) == "W+LSHIFT", "adding Shift publishes W+LSHIFT");
        check(aver_fw_input_key(AVER_FW_KEY_LSHIFT) != 0, "the framework reads LSHIFT as held");
        check(aver_fw_input_key(AVER_FW_KEY_W) != 0,      "and W is still held");

        in.newFrame(); in.onEvent(key(kVkShift, false));
        check(publish(in) == "W", "releasing Shift returns to W");
        check(aver_fw_input_key(AVER_FW_KEY_LSHIFT) == 0, "the framework reads LSHIFT as released");

        in.newFrame(); in.onEvent(key(kVkW, false));
        check(publish(in) == "(none)", "releasing W leaves nothing held");
        check(aver_fw_input_key(AVER_FW_KEY_W) == 0, "the framework reads W as released");
    }

    // ---- VK_SHIFT and not VK_LSHIFT ----
    {
        AVER_INFO("the UNSIDED modifier codes are the ones that arrive");
        InputState in;
        in.newFrame();
        // WM_KEYDOWN delivers VK_SHIFT (0x10), never VK_LSHIFT (0xA0), unless the receiver does the
        // extended-key dance. Mapping the sided code would produce a Shift that never registers --
        // and would look like a gameplay bug rather than a mapping one.
        in.onEvent(key(0xA0, true));   // VK_LSHIFT
        check(publish(in) == "(none)", "VK_LSHIFT alone publishes nothing, which is correct");
        in.newFrame();
        in.onEvent(key(kVkShift, true));
        check(publish(in) == "LSHIFT", "VK_SHIFT is what maps to the framework's LSHIFT slot");
    }

    // ---- focus ----
    {
        AVER_INFO("an unfocused window publishes nothing");
        InputState in;
        in.newFrame(); in.onEvent(key(kVkW, true));
        check(publish(in, /*focused=*/true) == "W", "focused: W is published");
        check(publish(in, /*focused=*/false).empty(), "unfocused: nothing is published");
        check(aver_fw_input_key(AVER_FW_KEY_W) == 0,
              "and the framework no longer sees W held - a key held while the player alt-tabs "
              "must not keep driving the pawn");
    }

    // ---- mouse buttons ----
    {
        AVER_INFO("mouse buttons reach the framework");
        InputState in;
        in.newFrame();
        Event mb; mb.type = EventType::MouseButton; mb.button = 0; mb.pressed = true;
        in.onEvent(mb);
        check(publish(in) == "MOUSE_LEFT", "left button publishes MOUSE_LEFT");
        check(aver_fw_input_key(AVER_FW_KEY_MOUSE_LEFT) != 0, "the framework reads it as held");
    }

    if (g_failures == 0) { AVER_INFO("InputBridgeTest: ALL PASS"); return 0; }
    AVER_ERROR("InputBridgeTest: {} failure(s)", g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
