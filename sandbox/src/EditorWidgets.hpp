#pragma once
// Small ImGui widget helpers shared across the editor's mode panels and asset tabs.
//
// panelFloat/panelInt started as static, private members of SandboxApp -- built for the landscape
// sculpt and foliage mode panels -- and so were reachable only from SandboxApp's own member
// functions. Every NEW editor that needed the same narrow-dock label fix had to reinvent it, or
// reintroduce the exact bug it exists to prevent (see panelFloat's own comment below). Moved here
// with NO BEHAVIOUR CHANGE: identical signatures, identical bodies, identical call sites -- this is
// a home, not a rewrite.
//
// THE SPLIT-PANE HELPERS BELOW (SplitPane, splitPaneWidth, resetSplitPane, splitterHandle,
// drawSplitHandle) generalise ActorEditor's own column-splitter -- see ActorEditor.cpp's file-local
// columnSplitter() -- so BtEditor/SoundEditor/ParticleEditor/AnimEditor/GraphEditor's own panel
// splits can share ONE resizable, persisted convention instead of each hand-rolling a fixed fraction
// of the content region every frame (avail.x * 0.45f and friends -- see each editor's own draw()
// before this). ActorEditor ITSELF IS NOT TOUCHED: it is the reference this was generalised from,
// kept exactly as it was, with its own file-local columnSplitter left in place.
//
// TWO PERSISTED REPRESENTATIONS, because the editors that had this problem already disagreed on
// which one fits: a FRACTION (SplitPane, below) for a pane that should always be some proportion of
// whatever room there is -- BtEditor's tree/details, SoundEditor's list/details, ParticleEditor's
// preview/params, and the inner half of AnimEditor's view/tracks split all started life as exactly
// that, as `avail.x * <constant>`. GraphEditor's details column is different by original design (see
// its own "Gap B" comment in GraphEditor.cpp): a fixed WIDTH that only narrows under real pressure,
// matching ActorEditor's own DPI-independent-pixel convention -- so it is NOT migrated onto SplitPane
// and instead calls splitterHandle()/clampSplitWidth() directly, the same two primitives ActorEditor's
// own splitter and SplitPane are both built from, persisting a width through EditorPrefs the way
// ActorEditor's g_leftColW/g_rightColW already do. Forcing either family into the other's convention
// would have changed what "reset" looks like for it, hence both.
//
// PURE ARITHMETIC IS TESTABLE; DRAWING IS NOT. clampSplitWidth/splitFractionOf/splitWidthOf/
// loadSplitFraction/storeSplitFraction/splitPaneWidth/resetSplitPane touch no ImGui symbol and are
// declared OUTSIDE the `#if AVER_WITH_IMGUI` guard below -- deliberately, so EditorWidgetsTest
// (tests/editor) can exercise the clamp, the pixel<->fraction conversion across a resize, and the
// ACTUAL EditorPrefs round trip with no window and no ImGui context, matching every other headless
// slice this codebase pulls out of a `draw()` for the same reason (BtEditor's structural edits,
// GraphEditorGeometry, AnimCurveGeometry, ...). What is left inside the guard -- splitterHandle and
// drawSplitHandle -- is the actual mouse-driven divider, and CORRECTNESS THERE IS VISUAL ONLY: verify
// it by eye, in a narrow dock, at high DPI, the same standing rule panelFloat/panelInt's own comment
// states below.
#include "EditorPrefs.hpp"

#include "aver/core/Types.hpp"

#include <string_view>

namespace aver::editor {

// ---- pure arithmetic: no ImGui, no window, safe for a headless test -----------------------------

// Clamps a pixel width to [minSelf, avail-minOther] without letting a small `avail` invert the
// range: if avail is too small to honour both minimums, minSelf wins so the pane keeps a usable size
// instead of the range going empty or negative. Lifted out of ActorEditor's own file-local
// columnSplitter (ActorEditor.cpp), which has the identical two lines inline; this is that clamp,
// generalised so it can be shared AND tested without a window.
inline f32 clampSplitWidth(f32 value, f32 avail, f32 minSelf, f32 minOther) {
    if (value < minSelf) value = minSelf;
    const f32 maxSelf = avail - minOther;
    if (maxSelf > minSelf && value > maxSelf) value = maxSelf;
    return value;
}

// A pixel width, as the fraction of `avail` it represents. 0 when avail<=0, so a window that has not
// laid out yet (the very first frame) cannot divide by zero.
inline f32 splitFractionOf(f32 widthPx, f32 avail) {
    return avail > 0.0f ? widthPx / avail : 0.0f;
}
// The inverse: a fraction back to a pixel width against THIS frame's `avail`. Round-tripping a value
// through splitFractionOf then splitWidthOf, computed at a DIFFERENT avail than it was captured at,
// is how a fraction-persisted pane tracks a resized window instead of staying visually put in
// pixels -- which is the entire reason SplitPane below persists a fraction rather than a raw width.
inline f32 splitWidthOf(f32 fraction, f32 avail) {
    return fraction * avail;
}

// Seeds a persisted split fraction from `prefKey`, or `defaultFraction` when it is absent -- a thin,
// named wrapper around prefFloat so a caller reads "load the split" rather than a bare pref lookup,
// and so EditorWidgetsTest exercises the SAME call EditorPrefsTest already proves against the real
// store, not a parallel mock of it.
inline f32 loadSplitFraction(std::string_view prefKey, f32 defaultFraction) {
    return prefFloat(prefKey, defaultFraction);
}
// Persists a split fraction and flushes immediately -- matching ActorEditor's own "write on release,
// not on every frame of a drag" rule (see drawSplitHandle below and ActorEditor.cpp's columnSplitter
// call sites), so a crash or an alt-tab mid-drag loses at most the drag in progress, never leaves the
// store dirty with nothing to flush it.
inline void storeSplitFraction(std::string_view prefKey, f32 fraction) {
    setPrefFloat(prefKey, fraction);
    flushEditorPrefs();
}

// Persisted state for ONE resizable divider between two panes. A PLAIN FLOAT, not a class with an
// ImGui-only constructor, so it can sit as an ordinary member on an editor tab whose OWN class must
// keep compiling with AVER_WITH_IMGUI undefined (tests/editor's headless build of these tabs -- see
// e.g. ParticleEditor.hpp's identical reasoning for its own plain-POD preview-resize bookkeeping).
// `fraction` is the FIRST pane's share of `avail`, 0..1; negative means "not yet seeded from its
// preference", the same sentinel meaning ActorEditor's own g_leftColW/g_rightColW give to <= 0.
struct SplitPane {
    f32 fraction = -1.0f;
};

// The first pane's width in pixels for THIS frame's `avail`: seeds `pane.fraction` from `prefKey`
// the first time this pane is used, then clamps against `avail` so neither pane can start below its
// floor even before any drag happens. `minSelfPx`/`minOtherPx` should already be DPI-scaled by the
// caller, matching ActorEditor's own convention of scaling at the call site rather than inside the
// shared helper. Callable with no ImGui context -- this and the seed/persist wrappers above are what
// EditorWidgetsTest exercises directly.
inline f32 splitPaneWidth(SplitPane& pane, std::string_view prefKey, f32 defaultFraction, f32 avail,
                           f32 minSelfPx, f32 minOtherPx) {
    if (pane.fraction < 0.0f) pane.fraction = loadSplitFraction(prefKey, defaultFraction);
    return clampSplitWidth(splitWidthOf(pane.fraction, avail), avail, minSelfPx, minOtherPx);
}

// Re-seeds `pane` to `defaultFraction` and persists it immediately, so "Reset Tab Layout" (routed
// through AssetEditor::resetLayout -- see AssetEditor.hpp, and AssetEditorHost::resetFocusedLayout's
// own comment for the bug that routing replaces) takes effect the moment it is clicked rather than
// waiting for a seed check to notice a stale value already sitting in memory.
inline void resetSplitPane(SplitPane& pane, std::string_view prefKey, f32 defaultFraction) {
    pane.fraction = defaultFraction;
    storeSplitFraction(prefKey, defaultFraction);
}

} // namespace aver::editor

#if AVER_WITH_IMGUI
#include "imgui.h"

#include <cstdio>

namespace aver::editor {

// Caption ABOVE the control, control full width below.
// ImGui's default puts the label to the RIGHT of a slider and does not clip it -- it just runs out
// of panel and disappears. In a 20%-width dock at 300% DPI that turned "Radius (cm)" into "Radiu".
// Every panel control that can appear in a narrow dock should go through here so none can regress.
inline bool panelFloat(const char* label, f32* v, f32 lo, f32 hi, const char* fmt,
                        ImGuiSliderFlags flags = 0) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1);
    char id[96];
    std::snprintf(id, sizeof id, "##%s", label);
    return ImGui::SliderFloat(id, v, lo, hi, fmt, flags);
}

// Same fix, for an int slider.
inline bool panelInt(const char* label, int* v, int lo, int hi) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1);
    char id[96];
    std::snprintf(id, sizeof id, "##%s", label);
    return ImGui::SliderInt(id, v, lo, hi);
}

// One draggable divider between two side-by-side ImGui children. Generalised, with NO BEHAVIOUR
// CHANGE, from ActorEditor's own file-local columnSplitter (ActorEditor.cpp) -- see this header's
// own top comment for why ActorEditor itself was left calling its own copy rather than migrated onto
// this. Assumes the FIRST pane's BeginChild/EndChild has already drawn on this line (SameLine, like
// ActorEditor's own splitter, depends on it). `*widthPx` is the first pane's width; the caller
// decides what unit that gets PERSISTED as -- drawSplitHandle below persists a fraction through
// SplitPane, while GraphEditor and ActorEditor's own splitter persist a DPI-independent pixel width
// instead (see their own call sites). Returns true the frame the handle moves; `*released` (when
// given) is true the frame the mouse button comes back up, which is the ONE frame a caller should
// write its preference -- persisting on every frame of a drag would rewrite the store dozens of
// times a second for no benefit, exactly what flushEditorPrefs' own dirty-bit exists to avoid.
inline bool splitterHandle(const char* id, f32 thickness, f32* widthPx, f32 avail, f32 minSelf,
                            f32 minOther, bool* released = nullptr) {
    ImGui::SameLine(0.0f, 0.0f);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const f32 h = ImGui::GetContentRegionAvail().y;
    ImGui::InvisibleButton(id, ImVec2(thickness, h > 8.0f ? h : 8.0f));

    const bool hot = ImGui::IsItemActive() || ImGui::IsItemHovered();
    if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    bool moved = false;
    if (ImGui::IsItemActive() && ImGui::GetIO().MouseDelta.x != 0.0f) {
        *widthPx += ImGui::GetIO().MouseDelta.x;
        moved = true;
    }
    if (released) *released = ImGui::IsItemDeactivated();
    *widthPx = clampSplitWidth(*widthPx, avail, minSelf, minOther);

    if (hot) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const f32 x = at.x + thickness * 0.5f;
        dl->AddLine(ImVec2(x, at.y), ImVec2(x, at.y + (h > 8.0f ? h : 8.0f)),
                    ImGui::GetColorU32(ImGuiCol_SeparatorActive), 2.0f);
    }
    ImGui::SameLine(0.0f, 0.0f);
    return moved;
}

// Draws the drag handle for a pane whose width the caller just obtained from splitPaneWidth() above
// (and has already drawn that pane's BeginChild/EndChild at that width -- see splitterHandle's own
// comment on why the cursor position matters). Updates `pane.fraction` for the NEXT frame and, the
// instant the drag ends, persists it through the SAME EditorPrefs round trip EditorWidgetsTest
// exercises directly against loadSplitFraction/storeSplitFraction. `thickness` and the two minimums
// should already be DPI-scaled by the caller, matching splitPaneWidth's own convention.
// ONLY A DRAG WRITES THE FRACTION. This used to store the CLAMPED width back every frame, so any
// frame the tab was briefly narrow (opening, docking, a window resize) clamped the fraction down to
// honour the other pane's minimum -- and it never grew back once the tab widened, so the first pane
// ratcheted smaller over a session and the next drag persisted it ("the mesh viewer's viewport keeps
// getting tinier"). The clamp belongs to this frame's drawn width (splitPaneWidth), not the setting.
inline void drawSplitHandle(SplitPane& pane, const char* id, std::string_view prefKey, f32 avail,
                             f32 minSelfPx, f32 minOtherPx, f32 thickness) {
    f32 widthPx = clampSplitWidth(splitWidthOf(pane.fraction, avail), avail, minSelfPx, minOtherPx);
    bool released = false;
    if (splitterHandle(id, thickness, &widthPx, avail, minSelfPx, minOtherPx, &released))
        pane.fraction = splitFractionOf(widthPx, avail);
    if (released) storeSplitFraction(prefKey, pane.fraction);
}

} // namespace aver::editor
#endif // AVER_WITH_IMGUI
