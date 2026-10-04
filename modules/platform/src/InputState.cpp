#include "aver/platform/InputState.hpp"

namespace aver {

void InputState::newFrame() {
    pressed_.fill(false);
    released_.fill(false);
    mPressed_.fill(false);
    mReleased_.fill(false);
    mouseDX_ = 0;
    mouseDY_ = 0;
    wheel_   = 0.0f;
}

void InputState::clear() {
    held_.fill(false);
    pressed_.fill(false);
    released_.fill(false);
    mHeld_.fill(false);
    mPressed_.fill(false);
    mReleased_.fill(false);
    mouseDX_ = 0;
    mouseDY_ = 0;
    wheel_   = 0.0f;
    // mouseX_/mouseY_ and hasMouse_ survive deliberately: the cursor is still wherever it is, and
    // forgetting that would manufacture a large bogus delta on the next move after refocus.
}

void InputState::onEvent(const Event& e) {
    switch (e.type) {
        case EventType::Key: {
            if (!inRange(e.key)) return;
            const usize k = static_cast<usize>(e.key);
            if (e.pressed) {
                // Windows auto-repeats a held key: WM_KEYDOWN arrives over and over with no
                // intervening WM_KEYUP. Only the 0->1 transition is a press, or every repeat would
                // read as a fresh tap and a "press to jump" would machine-gun.
                if (!held_[k]) pressed_[k] = true;
                held_[k] = true;
            } else {
                if (held_[k]) released_[k] = true;
                held_[k] = false;
            }
            return;
        }
        case EventType::MouseButton: {
            if (!inButton(e.button)) return;
            const usize b = static_cast<usize>(e.button);
            if (e.pressed) {
                if (!mHeld_[b]) mPressed_[b] = true;
                mHeld_[b] = true;
            } else {
                if (mHeld_[b]) mReleased_[b] = true;
                mHeld_[b] = false;
            }
            // A button event carries a position too, and it can be the first one seen.
            mouseX_ = e.mouseX;
            mouseY_ = e.mouseY;
            hasMouse_ = true;
            return;
        }
        case EventType::MouseMove: {
            if (hasMouse_) {
                mouseDX_ += e.mouseX - mouseX_;
                mouseDY_ += e.mouseY - mouseY_;
            }
            mouseX_ = e.mouseX;
            mouseY_ = e.mouseY;
            hasMouse_ = true;
            return;
        }
        case EventType::MouseWheel:
            wheel_ += e.wheel;
            return;
        case EventType::FocusLost:
            clear();
            return;
        default:
            return;   // window close/resize are not this class's business
    }
}

} // namespace aver
