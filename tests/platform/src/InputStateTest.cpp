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

    // ---- the mouse's side buttons, the sideways wheel, touch ----
    {
        AVER_INFO("side buttons are buttons 3 and 4");
        InputState in;
        in.newFrame();
        in.onEvent(buttonEvent(3, true, 10, 10));
        check(in.mouseHeld(3) && in.mousePressed(3), "X1 held and pressed on the frame of the press");
        check(!in.mouseHeld(4), "X2 is not X1");
        in.newFrame();
        in.onEvent(buttonEvent(3, false));
        check(!in.mouseHeld(3) && in.mouseReleased(3), "X1 released");
        in.onEvent(buttonEvent(4, true));
        check(in.mouseHeld(4), "X2 held");
        check(!in.mouseHeld(5), "a sixth button does not exist");
    }
    {
        AVER_INFO("horizontal wheel accumulates apart from the vertical one");
        InputState in;
        in.newFrame();
        Event h; h.type = EventType::MouseWheel; h.wheelX = 2.0f;
        in.onEvent(h);
        in.onEvent(wheelEvent(1.0f));
        check(in.wheelX() == 2.0f && in.wheel() == 1.0f, "sideways and vertical notches stay apart");
        in.newFrame();
        check(in.wheelX() == 0.0f, "sideways wheel resets each frame");
    }
    {
        AVER_INFO("touch: down, move, up, edges, focus loss");
        InputState in;
        check(!in.touchSeen(), "no touch seen at first");
        in.newFrame();
        Event t; t.type = EventType::Touch; t.finger = 2; t.pressed = true; t.mouseX = 40; t.mouseY = 50; t.pressure = 0.5f;
        in.onEvent(t);
        check(in.touchHeld(2) && in.touchPressed(2) && !in.touchHeld(0), "finger 2 down, finger 0 not");
        check(in.touchX(2) == 40.0f && in.touchY(2) == 50.0f && in.touchPressure(2) == 0.5f, "its position and pressure are kept");
        check(in.touchSeen(), "a touch screen is now known");
        in.newFrame();
        check(in.touchHeld(2) && !in.touchPressed(2), "still held next frame, without the press edge");
        t.mouseX = 60; in.onEvent(t);
        check(in.touchX(2) == 60.0f && !in.touchPressed(2), "a move updates the position and is not a new press");
        t.pressed = false; in.onEvent(t);
        check(!in.touchHeld(2) && in.touchReleased(2), "lifted: released edge");
        t.pressed = true; in.onEvent(t);
        in.onEvent(focusLostEvent());
        check(!in.touchHeld(2) && in.touchSeen(), "focus loss lifts every finger but remembers the screen exists");
        t.finger = 10; in.onEvent(t);
        check(!in.touchHeld(10), "finger 10 is out of range");
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

    // ---- typed text (E16) ----
    {
        AVER_INFO("typed characters: in order, per frame, filtered, bounded");
        InputState in;
        in.newFrame();
        const auto ch = [](u32 cp) { Event e; e.type = EventType::Char; e.codepoint = cp; return e; };
        in.onEvent(ch('h'));
        in.onEvent(ch(0x00E9));       // e acute
        in.onEvent(ch(0x1F600));      // outside the BMP
        check(in.typedChars().size() == 3 && in.typedChars()[0] == 'h' && in.typedChars()[1] == 0xE9 && in.typedChars()[2] == 0x1F600, "characters arrive in order, beyond the BMP too");
        in.onEvent(ch(0x08)); in.onEvent(ch(0x0D)); in.onEvent(ch(0x7F)); in.onEvent(ch(0x1B));
        in.onEvent(ch(0xD800)); in.onEvent(ch(0xDFFF)); in.onEvent(ch(0x110000)); in.onEvent(ch(0));
        check(in.typedChars().size() == 3, "control characters, surrogates, out-of-range values and zero are dropped");
        in.onEvent(keyEvent(kW, true));
        check(in.typedChars().size() == 3 && in.keyHeld(kW), "a key event is a key, not text");
        in.newFrame();
        check(in.typedChars().empty(), "the list lasts one frame");
        for (u32 i = 0; i < 500; ++i) in.onEvent(ch('a' + (i % 26)));
        check(in.typedChars().size() == InputState::kMaxTyped, "a frame keeps at most kMaxTyped characters");
        in.onEvent(focusLostEvent());
        check(in.typedChars().empty(), "focus loss drops them");
        Event rep = keyEvent(kA, true); rep.repeat = true;
        in.newFrame();
        in.onEvent(keyEvent(kA, true));
        in.onEvent(rep);
        check(in.keyPressed(kA) && in.keyHeld(kA), "a repeated key is still one press and one held key (the repeat flag does not change that)");
    }

    if (g_failures == 0) { AVER_INFO("InputStateTest: ALL PASS"); return 0; }
    AVER_ERROR("InputStateTest: {} failure(s)", g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
