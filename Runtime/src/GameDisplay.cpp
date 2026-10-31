#include "aver/game/GameDisplay.hpp"

#if AVER_MODULE_FRAMEWORK

#include "aver/framework/framework_abi.h"
#include "aver/platform/Window.hpp"
#include "aver/rhi/RHI.hpp"

namespace aver::game {

PointerPlan resolvePointerPlan(i32 mode, bool autoCapture, bool cursorFreed) {
    PointerPlan p;
    switch (mode) {
        case AVER_FW_POINTER_VISIBLE:         break;
        case AVER_FW_POINTER_HIDDEN:          p.hidden = true; break;
        case AVER_FW_POINTER_CAPTURED:        p.captured = true; break;
        case AVER_FW_POINTER_CONFINED:        p.confined = true; break;
        case AVER_FW_POINTER_CONFINED_HIDDEN: p.confined = true; p.hidden = true; break;
        case AVER_FW_POINTER_AUTO: default:   p.captured = autoCapture; break;
    }
    if (cursorFreed) p = PointerPlan{};
    return p;
}

i32 nearestPresentMode(const rhi::IDevice& dev, i32 wanted) {
    using PM = rhi::IDevice::PresentMode;
    if (wanted < 0 || wanted >= AVER_FW_PRESENT_COUNT) return AVER_FW_PRESENT_FIFO;
    if (dev.presentModeSupported(static_cast<PM>(wanted))) return wanted;
    if (wanted == AVER_FW_PRESENT_MAILBOX && dev.presentModeSupported(PM::Immediate)) return AVER_FW_PRESENT_IMMEDIATE;
    return AVER_FW_PRESENT_FIFO;
}

void applyPresentMode(rhi::IDevice& dev) {
    const i32 want = aver_fw_take_present_mode_request();
    if (want >= 0) dev.setPresentMode(static_cast<rhi::IDevice::PresentMode>(nearestPresentMode(dev, want)));
    aver_fw_publish_present_mode(static_cast<i32>(dev.presentMode()));
}

void applyWindowRequests(Window& w) {
    if (!w.valid()) return;
    i32 r[AVER_FW_WINDOW_STATE_COUNT] = {};
    const i32 mask = aver_fw_window_take_requests(r);
    if (mask & AVER_FW_WINDOW_REQ_FLAGS) {
        const i32 all[3] = {AVER_FW_WINDOW_FLAG_RESIZABLE, AVER_FW_WINDOW_FLAG_BORDERLESS, AVER_FW_WINDOW_FLAG_ALWAYS_ON_TOP};
        const u32 mapped[3] = {Window::kFlagResizable, Window::kFlagBorderless, Window::kFlagAlwaysOnTop};
        for (int i = 0; i < 3; ++i) {
            if (r[AVER_FW_WINDOW_FLAGS] & all[i]) w.setFlag(mapped[i], true);
            if (r[AVER_FW_WINDOW_FLAGS_OFF] & all[i]) w.setFlag(mapped[i], false);
        }
    }
    if (mask & AVER_FW_WINDOW_REQ_MODE) w.setMode(static_cast<Window::Mode>(r[AVER_FW_WINDOW_MODE]));
    if (mask & AVER_FW_WINDOW_REQ_SIZE) w.setClientSize(static_cast<u32>(r[AVER_FW_WINDOW_WIDTH]), static_cast<u32>(r[AVER_FW_WINDOW_HEIGHT]));
    if (mask & AVER_FW_WINDOW_REQ_POSITION) w.setOuterPosition(r[AVER_FW_WINDOW_X], r[AVER_FW_WINDOW_Y]);
    uint8_t title[256];
    if (aver_fw_window_take_title(title, static_cast<i32>(sizeof title)) > 0) w.setTitle(reinterpret_cast<const char*>(title));

    i32 s[AVER_FW_WINDOW_STATE_COUNT] = {};
    s[AVER_FW_WINDOW_MODE] = static_cast<i32>(w.mode());
    s[AVER_FW_WINDOW_WIDTH] = static_cast<i32>(w.width());
    s[AVER_FW_WINDOW_HEIGHT] = static_cast<i32>(w.height());
    i32 x = 0, y = 0;
    if (w.outerPosition(x, y)) { s[AVER_FW_WINDOW_X] = x; s[AVER_FW_WINDOW_Y] = y; }
    const u32 f = w.flags();
    s[AVER_FW_WINDOW_FLAGS] = ((f & Window::kFlagResizable) ? AVER_FW_WINDOW_FLAG_RESIZABLE : 0) |
                              ((f & Window::kFlagBorderless) ? AVER_FW_WINDOW_FLAG_BORDERLESS : 0) |
                              ((f & Window::kFlagAlwaysOnTop) ? AVER_FW_WINDOW_FLAG_ALWAYS_ON_TOP : 0);
    i32 mw = 0, mh = 0;
    if (w.monitorSize(mw, mh)) { s[AVER_FW_WINDOW_MONITOR_W] = mw; s[AVER_FW_WINDOW_MONITOR_H] = mh; }
    s[AVER_FW_WINDOW_DPI_PERCENT] = static_cast<i32>(w.dpiScale() * 100.0f + 0.5f);
    s[AVER_FW_WINDOW_FOCUSED] = w.isForeground() ? 1 : 0;
    aver_fw_window_publish(s);
}

void CursorHost::apply(Window& w, const PointerPlan& plan) {
    if (!w.valid()) return;
    // Images first, so a shape that has just got one shows it.
    if (const i32 serial = aver_fw_cursor_image_serial(); serial != imageSerial_) {
        imageSerial_ = serial;
        for (i32 s = 0; s < AVER_FW_CURSOR_COUNT; ++s) {
            i32 info[4] = {};
            const uint8_t* rgba = aver_fw_cursor_image(s, info);
            w.setCursorImage(static_cast<Window::CursorShape>(s), rgba, info[0], info[1], info[2], info[3]);
        }
    }
    if (const i32 shape = aver_fw_cursor_shape(); shape != shape_) {
        shape_ = shape;
        w.setCursorShape(static_cast<Window::CursorShape>(shape));
    }
    // A captured pointer is hidden and held by MouseCapture; hiding or confining it as well would fight it.
    const bool hide = plan.hidden && !plan.captured;
    if (hide != hidden_) { hidden_ = hide; w.setCursorHidden(hide); }
    const bool confine = plan.confined && !plan.captured;
    if (confine) w.setCursorConfined(true);          // every frame: a window that moved needs the clip moved
    else if (confined_) w.setCursorConfined(false);  // and only on the way out, so MouseCapture's own clip is left alone
    confined_ = confine;
    float warp[2] = {0.0f, 0.0f};
    if (aver_fw_take_pointer_warp(warp)) w.warpCursor(static_cast<i32>(warp[0]), static_cast<i32>(warp[1]));
}

} // namespace aver::game

#endif // AVER_MODULE_FRAMEWORK
