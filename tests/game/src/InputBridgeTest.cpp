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

#include <cmath>
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

static Event mouseMove(i32 x, i32 y) {
    Event e; e.type = EventType::MouseMove; e.mouseX = x; e.mouseY = y; return e;
}

// Publishes under `policy` and returns the echo string, the same one --input-echo logs.
static std::string publish(const InputState& in, const game::InputPublishPolicy& policy) {
    std::string echo;
    game::publishInput(in, policy, &echo);
    return echo;
}

// THE STANDALONE RUNTIME'S OWN POLICY, which is what every case below that is not specifically
// about device ownership wants. GameApp.cpp hands all three device gates the same foreground
// answer, because a shipped game has no panels to lose a device to -- the editor is the host that
// answers the three questions separately, and the cases at the bottom of this file cover that
// shape instead.
static game::InputPublishPolicy gamePolicy(bool focused) {
    game::InputPublishPolicy p;
    p.focused        = focused;
    p.keyboardToGame = focused;
    p.mouseToGame    = focused;
    p.gamepadActive  = focused;
    return p;
}

static std::string publish(const InputState& in, bool focused = true) {
    return publish(in, gamePolicy(focused));
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

    // ---- the captured mouse ----
    //
    // UNTESTED UNTIL NOW, and it is the branch a player spends the whole game in: publishInput has
    // taken a `captured` argument since 696c3666 gave the standalone runtime mouse capture, and
    // every case above passed false for it. The claim being pinned is the one the policy's own
    // comment makes -- capture replaces the SOURCE of the delta, not the gate on it -- so the
    // assertion is deliberately that the framework reads the re-centred value WHILE InputState's
    // own accumulated delta says something else, rather than merely that some number arrives.
    {
        AVER_INFO("capture replaces the source of the mouse delta");
        InputState in;
        in.newFrame();
        // Two moves, because the first position a cursor is ever seen at is deliberately a ZERO
        // delta (InputState.hpp's own comment on mouseDX: an uninitialised delta is a camera that
        // snaps on the first mouse move). The second move is the one that accumulates.
        in.onEvent(mouseMove(100, 100));
        in.onEvent(mouseMove(107, 100));
        check(in.mouseDX() == 7, "the window-accumulated delta is 7 -- the source capture replaces");

        f32 m3[3] = {};
        game::InputPublishPolicy p = gamePolicy(true);
        p.captured   = true;
        p.capturedDx = 42.0f;
        p.capturedDy = -3.0f;
        publish(in, p);
        aver_fw_input_mouse(m3);
        check(std::fabs(m3[0] - 42.0f) < 1e-4f && std::fabs(m3[1] + 3.0f) < 1e-4f,
              "captured: the framework reads the re-centred delta, not the window's 7 - a player "
              "looking hard in one direction must not run out of travel at the clip rect's edge");

        p.captured = false;
        publish(in, p);
        aver_fw_input_mouse(m3);
        check(std::fabs(m3[0] - 7.0f) < 1e-4f,
              "uncaptured: the SAME frame publishes the window-accumulated 7 instead");
    }

    // ---- a slot a host chord is eating ----
    //
    // The editor's drawer chord claims SPACE, and the key that opens a panel must not also make
    // the pawn jump. THE SECOND HALF OF THIS CASE IS THE POINT: eating a slot must subtract from
    // that slot's VALUE, never from the loop that publishes it. If eating were implemented as a
    // skip, the keys beside the eaten one would keep being published -- so asserting only that
    // SPACE reads 0 would pass either way. Asserting that W beside it is still HELD is what
    // distinguishes "this slot is eaten" from "the publish stopped".
    {
        AVER_INFO("an eaten slot releases while the key beside it stays held");
        constexpr i32 kVkSpace = 0x20;
        InputState in;
        in.newFrame();
        in.onEvent(key(kVkW, true));
        in.onEvent(key(kVkSpace, true));
        check(publish(in) == "W+SPACE", "both keys are held, and nothing is eaten yet");

        game::InputPublishPolicy p = gamePolicy(true);
        p.eaten[AVER_FW_KEY_SPACE] = true;
        check(publish(in, p) == "W", "the eaten slot drops out of the echo, which reports what was "
                                     "published rather than what is physically down");
        check(aver_fw_input_key(AVER_FW_KEY_SPACE) == 0, "the framework reads the eaten SPACE as released");
        check(aver_fw_input_key(AVER_FW_KEY_W) != 0,
              "AND W beside it is still held - eating one slot is not the publish stopping");
        check(aver_fw_input_vk(kVkSpace) != 0,
              "the raw-VK twin is NOT eaten: a chord is authored against a named slot");
    }

    // ---- the three devices are gated separately ----
    //
    // The editor answers "does the game own this" once per device, because the answers genuinely
    // differ: resolveInputOwnership (sandbox/src/InputOwnership.cpp:18-20) lets a captured cursor
    // keep the mouse on a frame an ImGui text field has taken the keyboard, since a hidden,
    // confined cursor has no widget it could be interacting with. The standalone runtime never
    // produces this shape -- gamePolicy above collapses all three gates into one answer -- so
    // without this case the split would ship with nothing exercising the gates apart.
    {
        AVER_INFO("a keyboard taken by a text field does not take the mouse with it");
        InputState in;
        in.newFrame();
        in.onEvent(key(kVkW, true));
        Event mb; mb.type = EventType::MouseButton; mb.button = 0; mb.pressed = true;
        in.onEvent(mb);

        game::InputPublishPolicy p;
        p.focused     = true;
        p.mouseToGame = true;   // keyboardToGame stays false: a text field has the keyboard
        check(publish(in, p) == "MOUSE_LEFT", "only the mouse button is published");
        check(aver_fw_input_key(AVER_FW_KEY_W) == 0, "the keyboard's W does not reach the game");
        check(aver_fw_input_vk(kVkW) == 0,
              "and neither does its raw-VK twin - the two halves of one physical key read the "
              "same, which is the disagreement this whole bridge exists to end");
        check(aver_fw_input_key(AVER_FW_KEY_MOUSE_LEFT) != 0, "the mouse button still does");
    }

    if (g_failures == 0) { AVER_INFO("InputBridgeTest: ALL PASS"); return 0; }
    AVER_ERROR("InputBridgeTest: {} failure(s)", g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
