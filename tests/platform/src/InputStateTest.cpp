// InputState, exercised by feeding it event streams directly.
//
// No window, no message loop, no device. That is the point of the class living below the UI layer:
// every hard case here is the accumulator's own logic -- edge detection, auto-repeat, focus loss,
// first-move delta -- so a real window would add scheduling noise and test nothing extra.
#include "aver/platform/InputState.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <string>

using namespace aver;

static int g_failures = 0;

// Records one assertion. Counts a failure and logs it when the condition is false.
static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static Event keyEvent(i32 vk, bool down) {
    Event e; e.type = EventType::Key; e.key = vk; e.pressed = down; return e;
}
static Event moveEvent(i32 x, i32 y) {
    Event e; e.type = EventType::MouseMove; e.mouseX = x; e.mouseY = y; return e;
}
static Event buttonEvent(i32 b, bool down, i32 x = 0, i32 y = 0) {
    Event e; e.type = EventType::MouseButton; e.button = b; e.pressed = down;
    e.mouseX = x; e.mouseY = y; return e;
}
static Event wheelEvent(f32 notches) {
    Event e; e.type = EventType::MouseWheel; e.wheel = notches; return e;
}
static Event focusLostEvent() {
    Event e; e.type = EventType::FocusLost; return e;
}

constexpr i32 kW = 'W';
constexpr i32 kA = 'A';

int main() {
    AVER_INFO("InputStateTest");

    // ---- keyboard edges ----
    {
        AVER_INFO("keyboard: press, hold, release");
        InputState in;
        in.newFrame();
        in.onEvent(keyEvent(kW, true));
        check(in.keyHeld(kW),      "held after press");
        check(in.keyPressed(kW),   "pressed edge on the frame of the press");
        check(!in.keyReleased(kW), "not released on the frame of the press");

        // Next frame with no events: still held, but the edge is gone.
        in.newFrame();
        check(in.keyHeld(kW),    "still held a frame later with no further events");
        check(!in.keyPressed(kW), "pressed edge does NOT persist into the next frame");

        in.newFrame();
        in.onEvent(keyEvent(kW, false));
        check(!in.keyHeld(kW),    "not held after release");
        check(in.keyReleased(kW), "released edge on the frame of the release");
    }

    // ---- auto-repeat ----
    {
        AVER_INFO("keyboard: auto-repeat is not a new press");
        InputState in;
        in.newFrame();
        in.onEvent(keyEvent(kW, true));
        check(in.keyPressed(kW), "first WM_KEYDOWN is a press");

        // Windows repeats a held key with no intervening KEYUP, in the SAME frame and in later
        // ones. Neither may read as a fresh tap: "press to jump" would machine-gun.
        in.onEvent(keyEvent(kW, true));
        in.onEvent(keyEvent(kW, true));
        check(in.keyHeld(kW), "still held through repeats");

        in.newFrame();
        in.onEvent(keyEvent(kW, true));   // a repeat arriving in a later frame
        check(!in.keyPressed(kW), "a repeat in a later frame is NOT a press");
        check(in.keyHeld(kW),     "and the key is still held");
    }

    // ---- focus loss ----
    {
        AVER_INFO("focus loss clears held keys");
        InputState in;
        in.newFrame();
        in.onEvent(keyEvent(kW, true));
        in.onEvent(keyEvent(kA, true));
        in.onEvent(buttonEvent(0, true, 10, 10));
        check(in.keyHeld(kW) && in.keyHeld(kA), "two keys held");
        check(in.mouseHeld(0),                  "and a mouse button");

        // No WM_KEYUP is ever sent for a key held when focus leaves. Without this the key stays
        // down for the rest of the process.
        in.onEvent(focusLostEvent());
        check(!in.keyHeld(kW),   "W released by focus loss");
        check(!in.keyHeld(kA),   "A released by focus loss");
        check(!in.mouseHeld(0),  "mouse button released by focus loss");

        // The cursor is still wherever it is; forgetting that manufactures a bogus delta on the
        // next move after refocus.
        check(in.hasMouse(),        "cursor position survives focus loss");
        check(in.mouseX() == 10,    "and keeps its value");
    }

    // ---- mouse motion ----
    {
        AVER_INFO("mouse: first move has no delta");
        InputState in;
        in.newFrame();
        in.onEvent(moveEvent(400, 300));
        check(in.hasMouse(),      "cursor seen");
        check(in.mouseX() == 400 && in.mouseY() == 300, "absolute position recorded");
        check(in.mouseDX() == 0 && in.mouseDY() == 0,
              "FIRST move reports zero delta, not the distance from the origin");

        in.newFrame();
        in.onEvent(moveEvent(410, 290));
        check(in.mouseDX() == 10,  "dx from the previous position");
        check(in.mouseDY() == -10, "dy from the previous position");

        // Several moves in one frame must sum, not overwrite: at high mouse poll rates a frame
        // routinely contains a dozen WM_MOUSEMOVEs and keeping only the last loses most of the
        // motion.
        in.newFrame();
        in.onEvent(moveEvent(415, 290));
        in.onEvent(moveEvent(420, 290));
        check(in.mouseDX() == 10, "two moves in one frame accumulate (5 + 5)");

        in.newFrame();
        check(in.mouseDX() == 0 && in.mouseDY() == 0, "delta resets on the next frame");
        check(in.mouseX() == 420,                     "absolute position does not reset");
    }

    // ---- mouse buttons and wheel ----
    {
        AVER_INFO("mouse: buttons and wheel");
        InputState in;
        in.newFrame();
        in.onEvent(buttonEvent(1, true, 5, 5));
        check(in.mouseHeld(1),     "right button held");
        check(in.mousePressed(1),  "right button press edge");
        check(!in.mouseHeld(0),    "left button unaffected");

        in.newFrame();
        check(in.mouseHeld(1),     "still held next frame");
        check(!in.mousePressed(1), "press edge cleared");

        in.newFrame();
        in.onEvent(buttonEvent(1, false, 5, 5));
        check(in.mouseReleased(1), "release edge");
        check(!in.mouseHeld(1),    "no longer held");

        in.newFrame();
        in.onEvent(wheelEvent(1.0f));
        in.onEvent(wheelEvent(-0.5f));
        check(in.wheel() == 0.5f, "wheel notches accumulate within a frame");
        in.newFrame();
        check(in.wheel() == 0.0f, "wheel resets each frame");
    }

    // ---- bounds ----
    {
        AVER_INFO("out-of-range codes are ignored, not crashes");
        InputState in;
        in.newFrame();
        in.onEvent(keyEvent(-1, true));
        in.onEvent(keyEvent(9999, true));
        in.onEvent(buttonEvent(-1, true));
        in.onEvent(buttonEvent(7, true));
        check(!in.keyHeld(-1) && !in.keyHeld(9999), "out-of-range keys report not-held");
        check(!in.mouseHeld(-1) && !in.mouseHeld(7), "out-of-range buttons report not-held");
        Event junk; junk.type = EventType::WindowResize; junk.width = 100; junk.height = 100;
        in.onEvent(junk);
        check(true, "a non-input event is ignored without incident");
    }

    if (g_failures == 0) { AVER_INFO("InputStateTest: ALL PASS"); return 0; }
    AVER_ERROR("InputStateTest: {} failure(s)", g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
