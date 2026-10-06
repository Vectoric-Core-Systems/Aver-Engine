// Per-kind default properties and kind names.
#include "aver/ui/UiWidget.hpp"

#include <cctype>

namespace aver::ui {

UiWidgetProps uiDefaultProps(UiWidgetKind kind) {
    UiWidgetProps p;
    p.kind = kind;
    switch (kind) {
        case UiWidgetKind::Button:
        case UiWidgetKind::KeyBind:
            p.focusable = true;
            p.textAlign = UiTextAlign::Center;
            break;
        case UiWidgetKind::Toggle:
        case UiWidgetKind::Choice:
        case UiWidgetKind::TextInput:
            p.focusable = true;
            break;
        case UiWidgetKind::Slider:
            p.focusable = true;
            p.minValue = 0.0f;
            p.maxValue = 1.0f;
            break;
        case UiWidgetKind::List:
            p.focusable = true;
            p.clip = true;
            break;
        case UiWidgetKind::Scroll:
            p.clip = true;
            p.layout = UiLayoutMode::VStack;
            break;
        case UiWidgetKind::ProgressBar:
            p.minValue = 0.0f;
            p.maxValue = 1.0f;
            break;
        default:
            break;
    }
    return p;
}

namespace {
const char* const kKindNames[] = {"panel", "text", "image", "button", "toggle", "slider",
                                  "choice", "list", "scroll", "textinput", "progress", "keybind"};
static_assert(sizeof(kKindNames) / sizeof(kKindNames[0]) == static_cast<usize>(UiWidgetKind::Count),
              "kind names must match UiWidgetKind");
} // namespace

const char* uiKindName(UiWidgetKind kind) {
    const usize i = static_cast<usize>(kind);
    return i < static_cast<usize>(UiWidgetKind::Count) ? kKindNames[i] : "panel";
}

bool uiKindFromName(std::string_view name, UiWidgetKind& out) {
    for (usize i = 0; i < static_cast<usize>(UiWidgetKind::Count); ++i) {
        const std::string_view k = kKindNames[i];
        if (k.size() != name.size()) continue;
        bool same = true;
        for (usize c = 0; c < k.size() && same; ++c)
            same = std::tolower(static_cast<unsigned char>(name[c])) == k[c];
        if (same) { out = static_cast<UiWidgetKind>(i); return true; }
    }
    return false;
}

} // namespace aver::ui
