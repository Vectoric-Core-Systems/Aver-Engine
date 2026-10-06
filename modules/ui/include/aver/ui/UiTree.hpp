#pragma once
// The retained widget tree: owns widgets, lays them out, routes input to them, raises events and
// draws through a UiPainter. Core-only like the rest of Aver.UI: testable with no GPU and no window.
// See docs/GAME_UI.md for the model (anchors vs stacks, focus scopes, input routing).
#include "aver/ui/UiFocus.hpp"
#include "aver/ui/UiLayout.hpp"
#include "aver/ui/UiStyle.hpp"
#include "aver/ui/UiText.hpp"
#include "aver/ui/UiWidget.hpp"

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace aver::ui {

// Navigation actions, as bits in UiInputFrame::nav. The host maps keys and gamepad onto these.
enum class UiNav : u8 { Up, Down, Left, Right, Accept, Cancel, NextTab, PrevTab, PageUp, PageDown };
constexpr u32 uiNavBit(UiNav n) { return 1u << static_cast<u32>(n); }

enum class UiEditKey : u8 { Backspace, Delete, Left, Right, Home, End, Enter };

// Everything the host feeds the tree for one frame. Edges (nav, chars, edit keys, rawKey) are
// "happened this frame"; buttons are "held".
struct UiInputFrame {
    f32 pointerX = 0, pointerY = 0;
    bool pointerValid = true;          // false while the cursor is captured by the game (mouse-look)
    u32 buttons = 0;                   // bit 0 left, 1 right, 2 middle
    f32 wheel = 0;                     // notches this frame, positive = away from the user (scroll up)
    u32 nav = 0;                       // uiNavBit(...) mask
    std::vector<u32> chars;            // typed code points
    std::vector<UiEditKey> editKeys;
    i32 rawKey = -1;                   // a key slot pressed this frame, for rebinding capture; -1 none
};

enum class UiEventType : u8 {
    Clicked,            // button/keybind activated, list row activated (index)
    Toggled,            // value = checked
    ValueChanged,       // slider value; choice (index = value = selected)
    SelectionChanged,   // list row selected (index)
    TextChanged,        // text = new contents
    TextSubmitted,      // Enter in a text input; text = contents
    FocusGained,
    FocusLost,
    KeyCaptured,        // a KeyBind heard a key; index = raw key slot
    Cancel,             // Cancel reached the menu root (widget = root)
    Command             // a widget's `command` fired, or a screen raised one; text = command
};

struct UiEvent {
    UiEventType type = UiEventType::Clicked;
    UiWidgetId widget = 0;
    f32 value = 0;
    i32 index = 0;
    std::string text;
};

// What the host should do with its own input this frame.
struct UiInputState {
    bool pointerOverUi = false;    // the pointer is on a widget that takes clicks
    bool menuOpen = false;         // a Menu-mode (or modal) root is visible
    bool textEditing = false;      // a text input has focus
    bool dragging = false;         // a widget has captured the pointer
    // Gameplay should not see mouse/keyboard/gamepad while this is true, and the cursor is shown.
    bool pausesGame() const { return menuOpen; }
    bool wantsPointer() const { return pointerOverUi || menuOpen || dragging; }
    bool wantsKeyboard() const { return menuOpen || textEditing; }
};

class UiTree {
public:
    UiTree();

    // ---- structure --------------------------------------------------------------------------
    // parent == 0 creates a root. Roots default to stretching over the viewport.
    UiWidgetId create(UiWidgetKind kind, UiWidgetId parent = 0, std::string_view name = {});
    // Creates from a full property set (a layout asset record).
    UiWidgetId createFrom(const UiWidgetProps& props, UiWidgetId parent = 0);
    // Destroys the widget and its subtree. False for an unknown id.
    bool destroy(UiWidgetId id);
    // Moves under `newParent` (0 = make it a root) at `index` among its siblings (-1 = last).
    // False for an unknown id or a move into its own subtree.
    bool reparent(UiWidgetId id, UiWidgetId newParent, i32 index = -1);
    void clear();

    UiWidget* get(UiWidgetId id);
    const UiWidget* get(UiWidgetId id) const;
    // First widget named `name` in depth-first order under `under` (0 = anywhere, roots in order).
    UiWidgetId find(std::string_view name, UiWidgetId under = 0) const;
    UiWidgetId rootOf(UiWidgetId id) const;
    const std::vector<UiWidgetId>& roots() const { return roots_; }
    usize size() const { return widgets_.size(); }

    // Depth-first pre-order visit of a subtree (0 = every root).
    void visit(UiWidgetId under, const std::function<void(UiWidget&)>& fn);

    bool isVisible(UiWidgetId id) const;    // this widget and every ancestor visible
    bool isEnabled(UiWidgetId id) const;    // this widget and every ancestor enabled

    // ---- environment ------------------------------------------------------------------------
    void setViewport(const UiRect& r) { viewport_ = r; }
    const UiRect& viewport() const { return viewport_; }
    void setDpi(const UiDpi& d) { dpi_ = d; }
    const UiDpi& dpi() const { return dpi_; }
    f32 scale() const { return scale_; }         // as of the last layout
    void setTheme(UiTheme t) { theme_ = std::move(t); }
    const UiTheme& theme() const { return theme_; }

    // ---- per frame --------------------------------------------------------------------------
    // Layout, input, events, layout again. Events reach listeners at the end of the call.
    void update(f32 dt, const UiInputFrame& in, const UiTextMetrics& metrics);
    // Layout only; what a preview or a test uses.
    void layout(const UiTextMetrics& metrics);
    void draw(UiPainter& painter);
    const UiInputState& inputState() const { return state_; }
    // A widget's rectangle inside its padding, as of the last layout: where its children are placed.
    UiRect contentRect(UiWidgetId id) const;

    // ---- queries ----------------------------------------------------------------------------
    // The topmost widget under a point (Passive roots are ignored, a modal root blocks the rest).
    UiWidgetId hitTest(f32 x, f32 y) const;
    UiWidgetId focused() const { return focus_; }
    bool setFocus(UiWidgetId id);
    void clearFocus();
    // The root whose subtree owns keyboard/gamepad navigation: the topmost visible modal root,
    // else the topmost visible Menu root, else 0.
    UiWidgetId navScope() const;
    // Focusable widgets under `root` in document order (what Tab and arrows choose between).
    std::vector<UiFocusItem> focusItems(UiWidgetId root) const;
    // Roots back to front: layer, then zOrder, then creation order.
    std::vector<UiWidgetId> sortedRoots() const;

    // ---- events -----------------------------------------------------------------------------
    // Pops the oldest queued event. The queue is capped; the oldest are dropped past 1024.
    bool pollEvent(UiEvent& out);
    // Listeners are called at the end of update() for every event, in order. Returns a token.
    usize addListener(std::function<void(const UiEvent&)> fn);
    void removeListener(usize token);
    // Raises an event as if a widget had (screens use this for their own commands).
    void emit(UiEvent ev);
    // Runs the built-in commands (ui.close, ui.open:Name, ui.toggle:Name) for `text`; true if it was one.
    bool runBuiltinCommand(std::string_view text, UiWidgetId source);

    // ---- programmatic value changes (no event is raised) ------------------------------------
    void setVisible(UiWidgetId id, bool visible);
    void setText(UiWidgetId id, std::string_view text);
    void setValue(UiWidgetId id, f32 value);          // slider/progress, clamped to its range
    void setChecked(UiWidgetId id, bool checked);
    void setSelected(UiWidgetId id, i32 index);       // choice/list
    // Scrolls a Scroll/List ancestor of `id` (or itself) so `id` is visible next layout.
    void scrollIntoView(UiWidgetId id);

private:
    // layout (UiTreeLayout.cpp)
    UiStyle styleOf(const UiWidget& w) const;
    f32 fontPx(const UiWidget& w, const UiStyle& st) const;
    UiInsets paddingOf(const UiWidget& w, const UiStyle& st) const;
    UiVec2 measure(UiWidget& w);
    void arrange(UiWidget& w, const UiRect& rect);
    void arrangeStack(UiWidget& w, const UiRect& inner, bool vertical, const std::vector<UiWidget*>& kids);
    void arrangeGrid(UiWidget& w, const UiRect& inner, const std::vector<UiWidget*>& kids);
    UiRect placeAnchored(const UiWidget& child, const UiRect& parent) const;
    void finishScroll(UiWidget& w, const UiRect& inner, const std::vector<UiWidget*>& kids);
    void translate(UiWidget& w, f32 dx, f32 dy);
    std::vector<UiWidget*> visibleChildren(UiWidget& w);
    f32 rowHeightOf(const UiWidget& w) const;
    UiRect sliderTrack(const UiWidget& w) const;

    // input (UiTreeInput.cpp)
    void processPointer(const UiInputFrame& in);
    void processNav(const UiInputFrame& in);
    void processText(const UiInputFrame& in);
    void updateScope();
    void activate(UiWidget& w, bool fromPointer, f32 pointerX);
    void dragTo(UiWidget& w, f32 x, f32 y);
    void cycleChoice(UiWidget& w, int delta);
    void adjustSlider(UiWidget& w, int dir);
    bool setSliderFromX(UiWidget& w, f32 x);
    void moveFocus(UiNavDir dir);
    void setFocusInternal(UiWidgetId id, bool visibleRing);
    bool hittable(const UiWidget& w) const;
    UiWidgetId hitIn(const UiWidget& w, f32 x, f32 y) const;
    bool focusCandidate(const UiWidget& w) const;
    void collectFocus(const UiWidget& w, std::vector<UiFocusItem>& out) const;
    UiWidgetId nearestFocusable(UiWidgetId id) const;
    UiWidgetId nearestScrollable(UiWidgetId id) const;
    void scrollBy(UiWidget& w, f32 dy);
    i32 caretFromX(const UiWidget& w, f32 x) const;
    f32 textShift(const UiWidget& w, const UiTextMetrics& m) const;
    std::string displayText(const UiWidget& w) const;
    void ensureRowVisible(UiWidget& list);
    bool focusableNow(UiWidgetId id) const;
    UiRect textArea(const UiWidget& w) const;
    void dispatch();

    // draw (UiTreeDraw.cpp)
    void drawWidget(UiPainter& p, const UiWidget& w, f32 opacity, bool enabled);
    void drawScrollbar(UiPainter& p, const UiWidget& w, const UiStyle& st, f32 opacity);

    std::unordered_map<UiWidgetId, std::unique_ptr<UiWidget>> widgets_;
    std::vector<UiWidgetId> roots_;
    UiWidgetId nextId_ = 1;
    u32 nextSeq_ = 1;

    UiRect viewport_{0, 0, 1920, 1080};
    UiDpi dpi_{};
    f32 scale_ = 1.0f;
    UiTheme theme_;
    const UiTextMetrics* metrics_ = nullptr;

    UiWidgetId focus_ = 0;
    UiWidgetId scope_ = 0;
    UiWidgetId captured_ = 0;       // widget holding the pointer for a drag
    UiWidgetId pressed_ = 0;
    bool focusVisible_ = false;
    u32 prevButtons_ = 0;
    f32 pointerX_ = 0, pointerY_ = 0;
    f32 time_ = 0;
    u64 frame_ = 0;
    u64 listenFrame_ = 0;
    bool scrollDrag_ = false;       // captured_ is dragging its scrollbar rather than its value

    std::vector<UiEvent> pending_;
    std::deque<UiEvent> queue_;
    std::vector<std::pair<usize, std::function<void(const UiEvent&)>>> listeners_;
    usize nextListener_ = 1;
    UiInputState state_{};
};

} // namespace aver::ui
