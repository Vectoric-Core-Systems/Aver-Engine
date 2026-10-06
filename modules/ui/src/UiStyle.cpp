// Built-in themes and style resolution.
#include "aver/ui/UiStyle.hpp"

namespace aver::ui {

namespace {

constexpr usize K(UiWidgetKind k) { return static_cast<usize>(k); }

struct Palette {
    u32 panel, panelBorder, text, textDim, accent, focus;
    u32 ctl, ctlHover, ctlPressed, ctlOff, ctlBorder;
    u32 track, field, dim;
};

UiStyle control(const Palette& p, UiInsets pad) {
    UiStyle s;
    s.background[0] = p.ctl;
    s.background[1] = p.ctlHover;
    s.background[2] = p.ctlPressed;
    s.background[3] = p.ctlOff;
    s.border = p.ctlBorder;
    s.borderWidth = 1.0f;
    s.text = p.text;
    s.textDisabled = p.textDim;
    s.accent = p.accent;
    s.track = p.track;
    s.placeholder = p.textDim;
    s.padding = pad;
    return s;
}

UiTheme build(const char* name, const Palette& p) {
    UiTheme t;
    t.name = name;
    t.focusColor = p.focus;
    t.dim = p.dim;

    UiStyle panel;
    panel.background[0] = panel.background[1] = panel.background[2] = panel.background[3] = p.panel;
    panel.border = p.panelBorder;
    panel.borderWidth = 1.0f;
    panel.text = p.text;
    panel.textDisabled = p.textDim;
    panel.accent = p.accent;
    t.kinds[K(UiWidgetKind::Panel)] = panel;

    UiStyle text;
    text.text = p.text;
    text.textDisabled = p.textDim;
    text.accent = p.accent;
    t.kinds[K(UiWidgetKind::Text)] = text;
    t.kinds[K(UiWidgetKind::Scroll)] = text;
    t.kinds[K(UiWidgetKind::Image)] = text;

    const UiInsets btnPad{14, 8, 14, 8};
    t.kinds[K(UiWidgetKind::Button)] = control(p, btnPad);
    t.kinds[K(UiWidgetKind::KeyBind)] = control(p, btnPad);
    t.kinds[K(UiWidgetKind::Choice)] = control(p, {28, 8, 28, 8});
    t.kinds[K(UiWidgetKind::Toggle)] = control(p, {0, 4, 0, 4});

    UiStyle slider = control(p, {0, 0, 0, 0});
    t.kinds[K(UiWidgetKind::Slider)] = slider;

    UiStyle bar = control(p, {6, 2, 6, 2});
    bar.border = 0;
    t.kinds[K(UiWidgetKind::ProgressBar)] = bar;

    UiStyle field = control(p, {10, 6, 10, 6});
    field.background[0] = field.background[1] = field.background[2] = p.field;
    t.kinds[K(UiWidgetKind::TextInput)] = field;

    UiStyle list = control(p, {2, 2, 2, 2});
    list.background[0] = list.background[1] = list.background[2] = p.field;
    t.kinds[K(UiWidgetKind::List)] = list;

    // Named variants: only the fields that differ.
    UiStyle title;
    title.fontSize = 32.0f;
    title.text = p.text;
    t.named.emplace_back("title", title);

    UiStyle heading;
    heading.fontSize = 22.0f;
    heading.text = p.accent;
    t.named.emplace_back("heading", heading);

    UiStyle muted;
    muted.text = p.textDim;
    t.named.emplace_back("muted", muted);

    UiStyle danger;
    danger.background[0] = uiRgba(150, 48, 48);
    danger.background[1] = uiRgba(180, 62, 62);
    danger.background[2] = uiRgba(120, 36, 36);
    t.named.emplace_back("danger", danger);

    UiStyle tab;
    tab.background[0] = uiShade(p.ctl, -0.25f);
    tab.border = 0;
    t.named.emplace_back("tab", tab);

    return t;
}

} // namespace

UiStyle UiTheme::resolve(UiWidgetKind kind, std::string_view namedStyle) const {
    UiStyle s = kinds[K(kind)];
    if (namedStyle.empty()) return s;
    if (namedStyle == "clear") {   // a container with no background or border
        for (u32& b : s.background) b = 0;
        s.border = 0;
        s.borderWidth = 0.0f;
        return s;
    }
    for (const auto& [n, o] : named) {
        if (n != namedStyle) continue;
        for (int i = 0; i < 4; ++i) if (o.background[i]) s.background[i] = o.background[i];
        if (o.border) s.border = o.border;
        if (o.borderWidth > 0.0f) s.borderWidth = o.borderWidth;
        if (o.text) s.text = o.text;
        if (o.textDisabled) s.textDisabled = o.textDisabled;
        if (o.accent) s.accent = o.accent;
        if (o.track) s.track = o.track;
        if (o.placeholder) s.placeholder = o.placeholder;
        if (o.fontSize > 0.0f) s.fontSize = o.fontSize;
        break;
    }
    return s;
}

UiTheme uiBuiltinTheme(std::string_view name) {
    if (name == "light") {
        Palette p{uiRgba(244, 245, 248, 235), uiRgba(190, 194, 204), uiRgba(28, 30, 38), uiRgba(130, 134, 146),
                  uiRgba(30, 110, 230), uiRgba(230, 130, 0),
                  uiRgba(226, 229, 237), uiRgba(210, 215, 228), uiRgba(190, 196, 212), uiRgba(236, 238, 242),
                  uiRgba(170, 176, 192), uiRgba(205, 209, 220), uiRgba(255, 255, 255), uiRgba(0, 0, 0, 90)};
        return build("light", p);
    }
    if (name == "contrast") {
        Palette p{uiRgba(0, 0, 0, 255), uiRgba(255, 255, 255), uiRgba(255, 255, 255), uiRgba(160, 160, 160),
                  uiRgba(255, 230, 0), uiRgba(0, 255, 255),
                  uiRgba(0, 0, 0), uiRgba(50, 50, 50), uiRgba(90, 90, 0), uiRgba(20, 20, 20),
                  uiRgba(255, 255, 255), uiRgba(70, 70, 70), uiRgba(0, 0, 0), uiRgba(0, 0, 0, 200)};
        return build("contrast", p);
    }
    Palette p{uiRgba(24, 26, 32, 232), uiRgba(64, 68, 80), uiRgba(232, 234, 240), uiRgba(128, 132, 144),
              uiRgba(86, 156, 255), uiRgba(255, 200, 80),
              uiRgba(52, 56, 68), uiRgba(68, 74, 92), uiRgba(38, 42, 54), uiRgba(34, 36, 42),
              uiRgba(86, 92, 108), uiRgba(44, 48, 60), uiRgba(18, 20, 26), uiRgba(0, 0, 0, 150)};
    return build("dark", p);
}

const char* const* uiBuiltinThemeNames() {
    static const char* const names[] = {"dark", "light", "contrast", nullptr};
    return names;
}

} // namespace aver::ui
