// Accessibility for the retained UI: the spoken form of a widget and the caption overlay. See UiAccess.hpp.
#include "aver/ui/UiAccess.hpp"

#include "aver/ui/UiTree.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace aver::ui {

namespace {

// Role and state words are references with English defaults, so a culture switch localizes them.
constexpr const char* kButton = "@Access:role.button=button";
constexpr const char* kCheckbox = "@Access:role.checkbox=checkbox";
constexpr const char* kChecked = "@Access:state.checked=checked";
constexpr const char* kUnchecked = "@Access:state.unchecked=unchecked";
constexpr const char* kSlider = "@Access:role.slider=slider";
constexpr const char* kChoice = "@Access:role.choice=choice";
constexpr const char* kOf = "@Access:choice.of=of";
constexpr const char* kKeyBind = "@Access:role.keybind=key binding";
constexpr const char* kListening = "@Access:state.listening=press a key";
constexpr const char* kEdit = "@Access:role.edit=edit field";
constexpr const char* kHidden = "@Access:state.hidden=hidden";
constexpr const char* kList = "@Access:role.list=list";
constexpr const char* kProgress = "@Access:role.progress=progress";
constexpr const char* kDisabled = "@Access:state.disabled=unavailable";

std::string say(const UiTree& t, const std::string& raw) { return raw.empty() ? std::string() : t.shown(raw); }

// The same number the slider draws: a percent for 0..1, else a whole number or two decimals.
std::string valueText(const UiWidget& w) {
    char buf[32];
    const f32 range = w.maxValue - w.minValue;
    if (w.showValue) {
        if (w.minValue == 0.0f && w.maxValue <= 1.0001f) std::snprintf(buf, sizeof buf, "%d%%", static_cast<int>(std::lround(w.value * 100.0f)));
        else if (w.step >= 1.0f || range >= 20.0f) std::snprintf(buf, sizeof buf, "%.0f", static_cast<double>(w.value));
        else std::snprintf(buf, sizeof buf, "%.2f", static_cast<double>(w.value));
    } else {
        const f32 k = range > 0.0f ? (w.value - w.minValue) / range : 0.0f;
        std::snprintf(buf, sizeof buf, "%d%%", static_cast<int>(std::lround(std::clamp(k, 0.0f, 1.0f) * 100.0f)));
    }
    return buf;
}

// The nearest Text sibling before the widget: the label of a settings row (Text + control in one HStack).
std::string siblingLabel(const UiTree& t, const UiWidget& w) {
    const UiWidget* p = t.get(w.parent);
    if (!p) return {};
    std::string found;
    for (const UiWidgetId c : p->children) {
        if (c == w.id) break;
        const UiWidget* k = t.get(c);
        if (k && k->kind == UiWidgetKind::Text && !k->text.empty()) found = say(t, k->text);
    }
    return found;
}

void add(std::string& out, const std::string& part) {
    if (part.empty()) return;
    if (!out.empty()) out += ", ";
    out += part;
}

} // namespace

std::string uiAccessibleText(const UiTree& tree, UiWidgetId id) {
    const UiWidget* w = tree.get(id);
    if (!w) return {};

    std::string name = say(tree, w->accessName);
    if (name.empty()) {
        switch (w->kind) {
            case UiWidgetKind::Button:
            case UiWidgetKind::Text:
            case UiWidgetKind::ProgressBar:
                name = say(tree, w->text);
                break;
            case UiWidgetKind::Toggle:
                name = w->text.empty() ? siblingLabel(tree, *w) : say(tree, w->text);
                break;
            case UiWidgetKind::KeyBind:   // the text is the bound key (the value), the label is the sibling
                name = siblingLabel(tree, *w);
                if (name.empty()) name = say(tree, w->placeholder);
                break;
            case UiWidgetKind::Slider:
            case UiWidgetKind::Choice:
            case UiWidgetKind::List:
            case UiWidgetKind::TextInput:
                name = siblingLabel(tree, *w);
                if (name.empty()) name = say(tree, w->placeholder);
                break;
            default:
                break;
        }
    }

    std::string out;
    add(out, name);
    switch (w->kind) {
        case UiWidgetKind::Button:
            add(out, say(tree, kButton));
            break;
        case UiWidgetKind::Toggle:
            add(out, say(tree, kCheckbox));
            add(out, say(tree, w->checked ? kChecked : kUnchecked));
            break;
        case UiWidgetKind::Slider:
            add(out, say(tree, kSlider));
            add(out, valueText(*w));
            break;
        case UiWidgetKind::Choice: {
            add(out, say(tree, kChoice));
            const usize n = w->items.size();
            if (w->selected >= 0 && static_cast<usize>(w->selected) < n) {
                add(out, say(tree, w->items[static_cast<usize>(w->selected)]));
                add(out, std::to_string(w->selected + 1) + " " + say(tree, kOf) + " " + std::to_string(n));
            }
            break;
        }
        case UiWidgetKind::KeyBind:
            add(out, say(tree, kKeyBind));
            add(out, w->listening ? say(tree, kListening) : say(tree, w->text));
            break;
        case UiWidgetKind::TextInput:
            add(out, say(tree, kEdit));
            if (w->masked) { if (!w->text.empty()) add(out, say(tree, kHidden)); }   // never the content
            else add(out, w->text);                                                   // typed text is not a reference
            break;
        case UiWidgetKind::List:
            add(out, say(tree, kList));
            if (w->selected >= 0 && static_cast<usize>(w->selected) < w->items.size())
                add(out, say(tree, w->items[static_cast<usize>(w->selected)]));
            break;
        case UiWidgetKind::ProgressBar:
            add(out, say(tree, kProgress));
            add(out, valueText(*w));
            break;
        default:
            break;
    }
    if (!tree.isEnabled(id)) add(out, say(tree, kDisabled));
    return out;
}

} // namespace aver::ui
