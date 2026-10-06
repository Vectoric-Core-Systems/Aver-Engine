// UiTree input: hit testing, pointer, navigation, text editing, focus scopes.
#include "aver/ui/UiTree.hpp"

#include <algorithm>
#include <cmath>

namespace aver::ui {

namespace {

constexpr f32 kThumbDp = 14.0f;       // slider thumb width
constexpr f32 kScrollGripDp = 14.0f;  // width of the draggable scrollbar strip
constexpr f32 kWheelStepDp = 60.0f;   // Scroll pixels per wheel notch

bool isScrollable(const UiWidget& w) { return w.kind == UiWidgetKind::Scroll || w.kind == UiWidgetKind::List; }

} // namespace

// ---- hit testing ----------------------------------------------------------------------------------

bool UiTree::hittable(const UiWidget& w) const {
    if (w.hit == UiHitMode::Never) return false;
    if (w.hit == UiHitMode::Always) return true;
    switch (w.kind) {
        case UiWidgetKind::Button:
        case UiWidgetKind::Toggle:
        case UiWidgetKind::Slider:
        case UiWidgetKind::Choice:
        case UiWidgetKind::List:
        case UiWidgetKind::Scroll:
        case UiWidgetKind::TextInput:
        case UiWidgetKind::KeyBind:
            return true;
        case UiWidgetKind::Panel: {
            const u32 bg = w.bgColor ? w.bgColor : styleOf(w).background[0];
            return uiAlphaOf(bg) > 0;
        }
        default:
            return false;
    }
}

UiWidgetId UiTree::hitIn(const UiWidget& w, f32 x, f32 y) const {
    if (!w.visible) return 0;
    const bool inside = w.rect.contains(x, y);
    if ((w.clip || isScrollable(w)) && !inside) return 0;
    for (usize i = w.children.size(); i-- > 0;) {
        if (const UiWidget* k = get(w.children[i]))
            if (const UiWidgetId h = hitIn(*k, x, y)) return h;
    }
    return (inside && hittable(w)) ? w.id : 0;
}

UiWidgetId UiTree::hitTest(f32 x, f32 y) const {
    const std::vector<UiWidgetId> order = sortedRoots();
    UiWidgetId modal = 0;
    for (usize i = order.size(); i-- > 0;) {
        const UiWidget* r = get(order[i]);
        if (r->visible && r->modal) { modal = r->id; break; }
    }
    for (usize i = order.size(); i-- > 0;) {
        const UiWidget* r = get(order[i]);
        if (r->visible && (r->inputMode != UiInputMode::Passive || r->modal))
            if (const UiWidgetId h = hitIn(*r, x, y)) return h;
        if (order[i] == modal) break;
    }
    return 0;
}

// ---- focus ----------------------------------------------------------------------------------------

bool UiTree::focusableNow(UiWidgetId id) const {
    const UiWidget* w = get(id);
    return w && w->focusable && isVisible(id) && isEnabled(id);
}

void UiTree::collectFocus(const UiWidget& w, std::vector<UiFocusItem>& out) const {
    if (!w.visible || !w.enabled) return;
    if (w.focusable && !w.rect.empty()) out.push_back(UiFocusItem{w.id, w.rect, w.tabIndex});
    for (const UiWidgetId c : w.children) if (const UiWidget* k = get(c)) collectFocus(*k, out);
}

std::vector<UiFocusItem> UiTree::focusItems(UiWidgetId root) const {
    std::vector<UiFocusItem> out;
    if (const UiWidget* r = get(root)) collectFocus(*r, out);
    return out;
}

UiWidgetId UiTree::nearestFocusable(UiWidgetId id) const {
    for (const UiWidget* w = get(id); w; w = w->parent ? get(w->parent) : nullptr)
        if (w->focusable) return w->id;
    return 0;
}

UiWidgetId UiTree::nearestScrollable(UiWidgetId id) const {
    for (const UiWidget* w = get(id); w; w = w->parent ? get(w->parent) : nullptr) {
        if (!isScrollable(*w)) continue;
        const UiRect inner = w->rect.deflate(paddingOf(*w, styleOf(*w)));
        if (w->contentSize.y > inner.h + 0.5f) return w->id;
    }
    return 0;
}

void UiTree::setFocusInternal(UiWidgetId id, bool visibleRing) {
    if (id == focus_) {
        if (visibleRing && id != 0) focusVisible_ = true;
        return;
    }
    if (UiWidget* old = get(focus_)) {
        old->focused = false;
        old->listening = false;
        UiEvent e;
        e.type = UiEventType::FocusLost;
        e.widget = old->id;
        emit(std::move(e));
    }
    focus_ = id;
    if (UiWidget* w = get(id)) {
        w->focused = true;
        if (w->kind == UiWidgetKind::TextInput) w->caret = static_cast<i32>(uiUtf8Length(w->text));
        UiEvent e;
        e.type = UiEventType::FocusGained;
        e.widget = id;
        emit(std::move(e));
    }
    focusVisible_ = visibleRing && id != 0;
}

bool UiTree::setFocus(UiWidgetId id) {
    if (id == 0) { clearFocus(); return true; }
    if (!focusableNow(id)) return false;
    setFocusInternal(id, true);
    return true;
}

void UiTree::clearFocus() { setFocusInternal(0, false); }

void UiTree::updateScope() {
    const UiWidgetId scope = navScope();
    if (scope != scope_) {
        scope_ = scope;
        if (scope) {
            const UiWidget* root = get(scope);
            UiWidgetId target = 0;
            if (!root->defaultFocus.empty()) {
                const UiWidgetId named = find(root->defaultFocus, scope);
                if (focusableNow(named)) target = named;
            }
            if (!target) {
                const std::vector<UiWidgetId> order = uiTabOrder(focusItems(scope));
                if (!order.empty()) target = order.front();
            }
            setFocusInternal(target, true);
        } else {
            clearFocus();
        }
        return;
    }
    if (scope_) {
        if (!(focusableNow(focus_) && rootOf(focus_) == scope_)) {
            const std::vector<UiWidgetId> order = uiTabOrder(focusItems(scope_));
            setFocusInternal(order.empty() ? 0 : order.front(), true);
        }
    } else if (focus_ != 0 && !focusableNow(focus_)) {
        clearFocus();
    }
}

void UiTree::moveFocus(UiNavDir dir) {
    const UiWidget* f = get(focus_);
    if (f) {
        const std::string& over = dir == UiNavDir::Up ? f->navUp : dir == UiNavDir::Down ? f->navDown
                                : dir == UiNavDir::Left ? f->navLeft : f->navRight;
        if (!over.empty()) {
            const UiWidgetId t = find(over, scope_);
            if (focusableNow(t)) { setFocusInternal(t, true); scrollIntoView(t); return; }
        }
    }
    const UiWidgetId next = uiNavigate(focusItems(scope_), focus_, dir, true);
    if (next) { setFocusInternal(next, true); scrollIntoView(next); }
}

// ---- value widgets -----------------------------------------------------------------------------------

void UiTree::cycleChoice(UiWidget& w, int delta) {
    const i32 n = static_cast<i32>(w.items.size());
    if (n == 0) return;
    w.selected = ((w.selected + delta) % n + n) % n;
    UiEvent e;
    e.type = UiEventType::ValueChanged;
    e.widget = w.id;
    e.index = w.selected;
    e.value = static_cast<f32>(w.selected);
    e.text = w.items[static_cast<usize>(w.selected)];
    emit(std::move(e));
}

void UiTree::adjustSlider(UiWidget& w, int dir) {
    const f32 range = w.maxValue - w.minValue;
    if (range <= 0.0f) return;
    const f32 step = w.step > 0.0f ? w.step : range * 0.05f;
    const f32 v = std::clamp(w.value + static_cast<f32>(dir) * step, w.minValue, w.maxValue);
    if (v == w.value) return;
    w.value = v;
    UiEvent e;
    e.type = UiEventType::ValueChanged;
    e.widget = w.id;
    e.value = v;
    emit(std::move(e));
}

bool UiTree::setSliderFromX(UiWidget& w, f32 x) {
    const UiRect track = sliderTrack(w);
    const f32 half = kThumbDp * scale_ * 0.5f;
    const f32 usable = track.w - 2.0f * half;
    if (usable <= 0.0f || w.maxValue <= w.minValue) return false;
    const f32 t = std::clamp((x - (track.x + half)) / usable, 0.0f, 1.0f);
    f32 v = w.minValue + t * (w.maxValue - w.minValue);
    if (w.step > 0.0f) v = w.minValue + std::round((v - w.minValue) / w.step) * w.step;
    v = std::clamp(v, w.minValue, w.maxValue);
    if (v == w.value) return false;
    w.value = v;
    UiEvent e;
    e.type = UiEventType::ValueChanged;
    e.widget = w.id;
    e.value = v;
    emit(std::move(e));
    return true;
}

void UiTree::scrollBy(UiWidget& w, f32 dy) {
    const UiRect inner = w.rect.deflate(paddingOf(w, styleOf(w)));
    w.scrollY = uiClampScroll(w.scrollY + dy, w.contentSize.y, inner.h);
}

void UiTree::ensureRowVisible(UiWidget& list) {
    const f32 rh = rowHeightOf(list);
    const UiRect inner = list.rect.deflate(paddingOf(list, styleOf(list)));
    list.scrollY = uiScrollIntoView(list.scrollY, inner.h, static_cast<f32>(list.selected) * rh, rh);
    list.scrollY = uiClampScroll(list.scrollY, rh * static_cast<f32>(list.items.size()), inner.h);
}

void UiTree::dragTo(UiWidget& w, f32 x, f32 y) {
    if (scrollDrag_ && isScrollable(w)) {
        const UiRect inner = w.rect.deflate(paddingOf(w, styleOf(w)));
        const f32 span = std::max(0.0f, w.contentSize.y - inner.h);
        const f32 t = inner.h > 0.0f ? std::clamp((y - inner.y) / inner.h, 0.0f, 1.0f) : 0.0f;
        w.scrollY = t * span;
        return;
    }
    if (w.kind == UiWidgetKind::Slider) setSliderFromX(w, x);
}

void UiTree::activate(UiWidget& w, bool fromPointer, f32 pointerX) {
    const auto command = [&]() {
        if (w.command.empty()) return;
        UiEvent c;
        c.type = UiEventType::Command;
        c.widget = w.id;
        c.text = w.command;
        emit(std::move(c));
    };
    switch (w.kind) {
        case UiWidgetKind::Button: {
            UiEvent e;
            e.type = UiEventType::Clicked;
            e.widget = w.id;
            emit(std::move(e));
            command();
            break;
        }
        case UiWidgetKind::KeyBind: {
            w.listening = true;
            listenFrame_ = frame_;
            UiEvent e;
            e.type = UiEventType::Clicked;
            e.widget = w.id;
            emit(std::move(e));
            command();
            break;
        }
        case UiWidgetKind::Toggle: {
            w.checked = !w.checked;
            UiEvent e;
            e.type = UiEventType::Toggled;
            e.widget = w.id;
            e.value = w.checked ? 1.0f : 0.0f;
            emit(std::move(e));
            command();
            break;
        }
        case UiWidgetKind::Choice:
            cycleChoice(w, (fromPointer && pointerX < w.rect.center().x) ? -1 : 1);
            command();
            break;
        case UiWidgetKind::List: {
            if (w.items.empty()) break;
            UiEvent e;
            e.type = UiEventType::Clicked;
            e.widget = w.id;
            e.index = w.selected;
            e.text = w.items[static_cast<usize>(std::clamp<i32>(w.selected, 0, static_cast<i32>(w.items.size()) - 1))];
            emit(std::move(e));
            command();
            break;
        }
        default:
            break;
    }
}

// ---- text editing -------------------------------------------------------------------------------------

UiRect UiTree::textArea(const UiWidget& w) const { return w.rect.deflate(paddingOf(w, styleOf(w))); }

std::string UiTree::displayText(const UiWidget& w) const {
    return w.masked ? std::string(uiUtf8Length(w.text), '*') : w.text;
}

f32 UiTree::textShift(const UiWidget& w, const UiTextMetrics& m) const {
    const UiStyle st = styleOf(w);
    const std::string text = displayText(w);
    const std::string_view prefix = std::string_view(text).substr(0, uiUtf8Offset(text, static_cast<usize>(std::max(0, w.caret))));
    const f32 caretX = m.textWidth(prefix, fontPx(w, st));
    return std::max(0.0f, caretX - (textArea(w).w - 2.0f * scale_));
}

i32 UiTree::caretFromX(const UiWidget& w, f32 x) const {
    if (!metrics_) return w.caret;
    const UiStyle st = styleOf(w);
    const f32 px = fontPx(w, st);
    const std::string text = displayText(w);
    const f32 rel = x - textArea(w).x + textShift(w, *metrics_);
    const usize n = uiUtf8Length(text);
    for (usize i = 0; i < n; ++i) {
        const f32 a = metrics_->textWidth(std::string_view(text).substr(0, uiUtf8Offset(text, i)), px);
        const f32 b = metrics_->textWidth(std::string_view(text).substr(0, uiUtf8Offset(text, i + 1)), px);
        if (rel < (a + b) * 0.5f) return static_cast<i32>(i);
    }
    return static_cast<i32>(n);
}

void UiTree::processText(const UiInputFrame& in) {
    UiWidget* w = get(focus_);
    if (!w || w->kind != UiWidgetKind::TextInput || !isVisible(w->id) || !isEnabled(w->id)) return;

    bool changed = false;
    bool submitted = false;
    usize len = uiUtf8Length(w->text);
    w->caret = std::clamp<i32>(w->caret, 0, static_cast<i32>(len));

    for (const u32 cp : in.chars) {
        if (cp < 32 || cp == 127) continue;
        if (w->maxLength > 0 && len >= static_cast<usize>(w->maxLength)) break;
        std::string ins;
        uiAppendUtf8(ins, cp);
        w->text.insert(uiUtf8Offset(w->text, static_cast<usize>(w->caret)), ins);
        ++w->caret;
        ++len;
        changed = true;
    }
    for (const UiEditKey k : in.editKeys) {
        const usize at = static_cast<usize>(w->caret);
        switch (k) {
            case UiEditKey::Backspace:
                if (at > 0) {
                    const usize a = uiUtf8Offset(w->text, at - 1), b = uiUtf8Offset(w->text, at);
                    w->text.erase(a, b - a);
                    --w->caret;
                    --len;
                    changed = true;
                }
                break;
            case UiEditKey::Delete:
                if (at < len) {
                    const usize a = uiUtf8Offset(w->text, at), b = uiUtf8Offset(w->text, at + 1);
                    w->text.erase(a, b - a);
                    --len;
                    changed = true;
                }
                break;
            case UiEditKey::Left: if (w->caret > 0) --w->caret; break;
            case UiEditKey::Right: if (at < len) ++w->caret; break;
            case UiEditKey::Home: w->caret = 0; break;
            case UiEditKey::End: w->caret = static_cast<i32>(len); break;
            case UiEditKey::Enter: submitted = true; break;
        }
    }
    if (changed) {
        UiEvent e;
        e.type = UiEventType::TextChanged;
        e.widget = w->id;
        e.text = w->text;
        emit(std::move(e));
    }
    if (submitted) {
        UiEvent e;
        e.type = UiEventType::TextSubmitted;
        e.widget = w->id;
        e.text = w->text;
        emit(std::move(e));
        if (!w->command.empty()) {
            UiEvent c;
            c.type = UiEventType::Command;
            c.widget = w->id;
            c.text = w->command;
            emit(std::move(c));
        }
    }
}

// ---- pointer ------------------------------------------------------------------------------------------

void UiTree::processPointer(const UiInputFrame& in) {
    visit(0, [](UiWidget& w) { w.hovered = false; w.hotItem = -1; });

    const bool left = (in.buttons & 1u) != 0;
    const bool leftDown = left && !(prevButtons_ & 1u);
    const bool leftUp = !left && (prevButtons_ & 1u);
    prevButtons_ = in.buttons;

    state_.pointerOverUi = false;
    UiWidgetId hit = 0;
    if (in.pointerValid) {
        pointerX_ = in.pointerX;
        pointerY_ = in.pointerY;
        hit = hitTest(pointerX_, pointerY_);
    }

    if (UiWidget* h = get(hit)) {
        h->hovered = true;
        state_.pointerOverUi = true;
        if (h->kind == UiWidgetKind::List) {
            const UiRect inner = textArea(*h);
            const f32 rh = rowHeightOf(*h);
            if (inner.contains(pointerX_, pointerY_) && rh > 0.0f) {
                const i32 row = static_cast<i32>(std::floor((pointerY_ - inner.y + h->scrollY) / rh));
                if (row >= 0 && row < static_cast<i32>(h->items.size())) h->hotItem = row;
            }
        }
    }

    if (captured_) {
        UiWidget* c = get(captured_);
        if (!c || !left) { captured_ = 0; scrollDrag_ = false; }
        else if (in.pointerValid) dragTo(*c, pointerX_, pointerY_);
    }

    if (leftDown) {
        UiWidget* h = get(hit);
        // A click anywhere but the capturing key-bind abandons the capture.
        if (UiWidget* f = get(focus_)) if (f->listening && f != h) f->listening = false;

        if (h) {
            const UiWidgetId f = nearestFocusable(hit);
            if (f && isEnabled(f) && isVisible(f)) setFocusInternal(f, false);
            else if (!scope_) clearFocus();

            if (isEnabled(hit)) {
                pressed_ = hit;
                h->pressed = true;
                const UiRect inner = textArea(*h);
                const bool overflow = h->contentSize.y > inner.h + 0.5f;
                const bool onGrip = pointerX_ >= h->rect.right() - kScrollGripDp * scale_;
                if (isScrollable(*h) && overflow && onGrip) {
                    captured_ = hit;
                    scrollDrag_ = true;
                    pressed_ = 0;
                    h->pressed = false;
                    dragTo(*h, pointerX_, pointerY_);
                } else if (h->kind == UiWidgetKind::Slider) {
                    captured_ = hit;
                    setSliderFromX(*h, pointerX_);
                } else if (h->kind == UiWidgetKind::TextInput) {
                    h->caret = caretFromX(*h, pointerX_);
                } else if (h->kind == UiWidgetKind::List && h->hotItem >= 0 && h->hotItem != h->selected) {
                    h->selected = h->hotItem;
                    UiEvent e;
                    e.type = UiEventType::SelectionChanged;
                    e.widget = h->id;
                    e.index = h->selected;
                    e.text = h->items[static_cast<usize>(h->selected)];
                    emit(std::move(e));
                }
            }
        } else if (!scope_) {
            clearFocus();
        }
    }

    if (leftUp) {
        if (UiWidget* p = get(pressed_)) {
            p->pressed = false;
            const bool sameTarget = hit == pressed_;
            const bool rowOk = p->kind != UiWidgetKind::List || (p->hotItem >= 0 && p->hotItem == p->selected);
            if (sameTarget && rowOk && isEnabled(pressed_)) activate(*p, true, pointerX_);
        }
        pressed_ = 0;
        captured_ = 0;
        scrollDrag_ = false;
    }

    if (in.wheel != 0.0f && hit) {
        if (UiWidget* s = get(nearestScrollable(hit))) {
            const f32 step = s->kind == UiWidgetKind::List ? rowHeightOf(*s) * 3.0f : kWheelStepDp * scale_;
            scrollBy(*s, -in.wheel * step);
        }
    }
}

// ---- navigation (keyboard and gamepad) ----------------------------------------------------------------------

void UiTree::processNav(const UiInputFrame& in) {
    if (!in.nav || !scope_) return;
    UiWidget* f = get(focus_);
    const auto has = [&](UiNav n) { return (in.nav & uiNavBit(n)) != 0; };

    if (f && f->listening) {   // a key-bind waiting for a key: only Cancel gets through
        if (has(UiNav::Cancel)) f->listening = false;
        return;
    }
    focusVisible_ = true;
    const bool editing = f && f->kind == UiWidgetKind::TextInput;

    if (has(UiNav::Cancel)) {
        UiEvent e;
        e.type = UiEventType::Cancel;
        e.widget = scope_;
        emit(std::move(e));
        if (const UiWidget* root = get(scope_)) {
            if (!root->cancelCommand.empty()) {
                UiEvent c;
                c.type = UiEventType::Command;
                c.widget = scope_;
                c.text = root->cancelCommand;
                emit(std::move(c));
            }
        }
        if (!get(scope_) || !isVisible(scope_)) return;   // a built-in command closed it
    }
    if (has(UiNav::Accept) && f && !editing && f->enabled) activate(*f, false, 0.0f);

    if (has(UiNav::NextTab) || has(UiNav::PrevTab)) {
        const UiWidgetId n = uiNextTab(focusItems(scope_), focus_, has(UiNav::PrevTab), true);
        if (n) { setFocusInternal(n, true); scrollIntoView(n); f = get(focus_); }
    }

    const auto horizontal = [&](int dir) {
        UiWidget* cur = get(focus_);
        if (cur && cur->kind == UiWidgetKind::Slider) adjustSlider(*cur, dir);
        else if (cur && cur->kind == UiWidgetKind::Choice) cycleChoice(*cur, dir);
        else if (!(cur && cur->kind == UiWidgetKind::TextInput)) moveFocus(dir < 0 ? UiNavDir::Left : UiNavDir::Right);
    };
    const auto vertical = [&](int dir) {
        UiWidget* cur = get(focus_);
        if (cur && cur->kind == UiWidgetKind::List && !cur->items.empty()) {
            const i32 next = cur->selected + dir;
            if (next >= 0 && next < static_cast<i32>(cur->items.size())) {
                cur->selected = next;
                ensureRowVisible(*cur);
                UiEvent e;
                e.type = UiEventType::SelectionChanged;
                e.widget = cur->id;
                e.index = next;
                e.text = cur->items[static_cast<usize>(next)];
                emit(std::move(e));
                return;
            }
        }
        moveFocus(dir < 0 ? UiNavDir::Up : UiNavDir::Down);
    };
    if (has(UiNav::Left)) horizontal(-1);
    if (has(UiNav::Right)) horizontal(1);
    if (has(UiNav::Up)) vertical(-1);
    if (has(UiNav::Down)) vertical(1);

    if (has(UiNav::PageUp) || has(UiNav::PageDown)) {
        const int dir = has(UiNav::PageUp) ? -1 : 1;
        UiWidget* cur = get(focus_);
        if (cur && cur->kind == UiWidgetKind::List && !cur->items.empty()) {
            const i32 n = static_cast<i32>(cur->items.size());
            cur->selected = std::clamp(cur->selected + dir * 5, 0, n - 1);
            ensureRowVisible(*cur);
            UiEvent e;
            e.type = UiEventType::SelectionChanged;
            e.widget = cur->id;
            e.index = cur->selected;
            e.text = cur->items[static_cast<usize>(cur->selected)];
            emit(std::move(e));
        } else if (UiWidget* s = get(nearestScrollable(focus_))) {
            scrollBy(*s, static_cast<f32>(dir) * textArea(*s).h * 0.9f);
        }
    }
}

// ---- the frame ----------------------------------------------------------------------------------------------

void UiTree::update(f32 dt, const UiInputFrame& in, const UiTextMetrics& metrics) {
    ++frame_;
    time_ += dt;
    metrics_ = &metrics;

    layout(metrics);
    updateScope();
    processPointer(in);
    processNav(in);
    processText(in);

    // Rebinding capture: a key pressed after the frame the key-bind started listening.
    if (UiWidget* f = get(focus_)) {
        if (f->listening && in.rawKey >= 0 && frame_ > listenFrame_) {
            f->listening = false;
            UiEvent e;
            e.type = UiEventType::KeyCaptured;
            e.widget = f->id;
            e.index = in.rawKey;
            emit(std::move(e));
        }
    }

    updateScope();   // a command above may have opened or closed a menu
    layout(metrics);

    const UiWidget* f = get(focus_);
    state_.menuOpen = navScope() != 0;
    state_.textEditing = f && f->kind == UiWidgetKind::TextInput;
    state_.dragging = captured_ != 0;

    metrics_ = nullptr;
    dispatch();
}

} // namespace aver::ui
