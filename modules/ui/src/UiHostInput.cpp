// Device state to UI input: navigation, editing, typed characters and repeat.
#include "aver/ui/UiHostInput.hpp"

namespace aver::ui {

namespace {

constexpr int kVkBack = 0x08, kVkTab = 0x09, kVkEnter = 0x0D, kVkShift = 0x10, kVkCtrl = 0x11, kVkEscape = 0x1B,
              kVkSpace = 0x20, kVkPageUp = 0x21, kVkPageDown = 0x22, kVkEnd = 0x23, kVkHome = 0x24, kVkLeft = 0x25,
              kVkUp = 0x26, kVkRight = 0x27, kVkDown = 0x28, kVkDelete = 0x2E;

// AVER_FW_GAMEPAD_* buttons used for navigation.
constexpr int kPadUp = 0, kPadDown = 1, kPadLeft = 2, kPadRight = 3, kPadLShoulder = 8, kPadRShoulder = 9,
              kPadA = 10, kPadB = 11;

bool isTypingKey(int vk) {
    return (vk >= 0x30 && vk <= 0x39) || (vk >= 0x41 && vk <= 0x5A) || (vk >= 0x60 && vk <= 0x69) ||
           vk == kVkSpace || (vk >= 0xBA && vk <= 0xC0) || (vk >= 0xDB && vk <= 0xDE);
}

} // namespace

u32 uiVkToChar(i32 vk, bool shift) {
    if (vk >= 0x41 && vk <= 0x5A) return static_cast<u32>(shift ? vk : vk + 32);
    if (vk >= 0x30 && vk <= 0x39) {
        static const char kShifted[] = ")!@#$%^&*(";
        return static_cast<u32>(shift ? kShifted[vk - 0x30] : vk);
    }
    if (vk >= 0x60 && vk <= 0x69) return static_cast<u32>('0' + (vk - 0x60));
    if (vk == kVkSpace) return ' ';
    struct Oem { int vk; char plain, shifted; };
    static const Oem kOem[] = {
        {0xBA, ';', ':'}, {0xBB, '=', '+'}, {0xBC, ',', '<'}, {0xBD, '-', '_'}, {0xBE, '.', '>'},
        {0xBF, '/', '?'}, {0xC0, '`', '~'}, {0xDB, '[', '{'}, {0xDC, '\\', '|'}, {0xDD, ']', '}'},
        {0xDE, '\'', '"'},
    };
    for (const Oem& o : kOem)
        if (o.vk == vk) return static_cast<u32>(shift ? o.shifted : o.plain);
    return 0;
}

bool UiHostInputMapper::Repeater::step(bool pressedEdge, bool held, f32 dt) {
    const bool pressed = pressedEdge || (held && !prev);
    prev = held;
    if (!held && !pressedEdge) {
        t = 0.0f;
        next = kRepeatDelay;
        return false;
    }
    if (pressed) {
        t = 0.0f;
        next = kRepeatDelay;
        return true;
    }
    t += dt;
    if (t >= next) {
        next += kRepeatInterval;
        return true;
    }
    return false;
}

bool UiHostInputMapper::Repeater::press(bool held) {
    const bool fire = held && !prev;
    prev = held;
    return fire;
}

i32 UiHostInputMapper::map(const UiHostSnapshot& s, f32 dt, UiInputFrame& out) {
    const auto rep = [&](int vk) { return key_[vk].step(s.keyPressed[vk], s.keyHeld[vk], dt); };
    const bool shift = s.keyHeld[kVkShift];
    const bool ctrl = s.keyHeld[kVkCtrl];

    bool up = rep(kVkUp), down = rep(kVkDown), left = rep(kVkLeft), right = rep(kVkRight);
    if (rep(kVkPageUp)) out.nav |= uiNavBit(UiNav::PageUp);
    if (rep(kVkPageDown)) out.nav |= uiNavBit(UiNav::PageDown);

    if (left) out.editKeys.push_back(UiEditKey::Left);
    if (right) out.editKeys.push_back(UiEditKey::Right);
    if (rep(kVkHome)) out.editKeys.push_back(UiEditKey::Home);
    if (rep(kVkEnd)) out.editKeys.push_back(UiEditKey::End);
    if (rep(kVkBack)) out.editKeys.push_back(UiEditKey::Backspace);
    if (rep(kVkDelete)) out.editKeys.push_back(UiEditKey::Delete);

    if (s.keyPressed[kVkEnter]) {
        out.nav |= uiNavBit(UiNav::Accept);
        out.editKeys.push_back(UiEditKey::Enter);
    }
    if (s.keyPressed[kVkSpace]) out.nav |= uiNavBit(UiNav::Accept);
    if (s.keyPressed[kVkEscape]) out.nav |= uiNavBit(UiNav::Cancel);
    if (s.keyPressed[kVkTab]) out.nav |= uiNavBit(shift ? UiNav::PrevTab : UiNav::NextTab);

    for (int vk = 0; vk < 256; ++vk) {
        if (!isTypingKey(vk)) continue;
        if (!rep(vk) || ctrl) continue;
        if (const u32 c = uiVkToChar(vk, shift)) out.chars.push_back(c);
    }

    if (s.padConnected) {
        const auto padRep = [&](int b) { return pad_[b].step(false, s.padButton[b], dt); };
        up = padRep(kPadUp) || up;
        down = padRep(kPadDown) || down;
        left = padRep(kPadLeft) || left;
        right = padRep(kPadRight) || right;

        const f32 thr = kStickThreshold;
        const bool sl = stick_[0].step(false, s.padAxis[0] < -thr, dt);
        const bool sr = stick_[1].step(false, s.padAxis[0] > thr, dt);
        const bool su = stick_[2].step(false, s.padAxis[1] > thr, dt);
        const bool sd = stick_[3].step(false, s.padAxis[1] < -thr, dt);
        up = up || su;
        down = down || sd;
        left = left || sl;
        right = right || sr;

        // Face and shoulder buttons act once per press.
        if (pad_[kPadA].press(s.padButton[kPadA])) out.nav |= uiNavBit(UiNav::Accept);
        if (pad_[kPadB].press(s.padButton[kPadB])) out.nav |= uiNavBit(UiNav::Cancel);
        if (pad_[kPadLShoulder].press(s.padButton[kPadLShoulder])) out.nav |= uiNavBit(UiNav::PrevTab);
        if (pad_[kPadRShoulder].press(s.padButton[kPadRShoulder])) out.nav |= uiNavBit(UiNav::NextTab);
    } else {
        for (Repeater& r : pad_) r = Repeater{};
        for (Repeater& r : stick_) r = Repeater{};
    }

    if (up) out.nav |= uiNavBit(UiNav::Up);
    if (down) out.nav |= uiNavBit(UiNav::Down);
    if (left) out.nav |= uiNavBit(UiNav::Left);
    if (right) out.nav |= uiNavBit(UiNav::Right);

    for (int vk = 1; vk < 256; ++vk)
        if (s.keyPressed[vk]) return vk;
    return -1;
}

} // namespace aver::ui
