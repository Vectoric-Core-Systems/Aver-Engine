// The widget system's C ABI: one process-wide UiTree, plus the models the ready-made screens edit.
#include "aver/ui/ui_widget_abi.h"

#include "UiAbiState.hpp"
#include "aver/ui/UiHostInput.hpp"
#include "aver/ui/UiLayoutAsset.hpp"
#include "aver/ui/UiProps.hpp"
#include "aver/ui/UiScreens.hpp"
#include "aver/ui/UiTree.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::ui;

static_assert(static_cast<int>(UiWidgetKind::Count) == 12, "AVER_UI_KIND_* mirrors UiWidgetKind");
static_assert(static_cast<int>(UiEventType::Command) == AVER_UI_EVENT_COMMAND, "AVER_UI_EVENT_* mirrors UiEventType");
static_assert(static_cast<int>(UiNav::PageDown) == AVER_UI_NAV_PAGE_DOWN, "AVER_UI_NAV_* mirrors UiNav");
static_assert(static_cast<int>(UiEditKey::Enter) == AVER_UI_EDIT_ENTER, "AVER_UI_EDIT_* mirrors UiEditKey");

namespace {

UiTree g_tree;
UiInputFrame g_in;
UiHostInputMapper g_mapper;
bool g_pointerValid = true;
UiSettingsModel g_settings = uiDefaultSettingsModel();
UiRebindModel g_rebind;
std::unique_ptr<UiSettingsScreen> g_screen;   // after g_tree: destroyed first, it unhooks from the tree
std::string g_str;
std::string g_error;

const char* keep(std::string s) {
    g_str = std::move(s);
    return g_str.c_str();
}

std::string str(const char* s) { return s ? std::string(s) : std::string(); }

std::vector<std::string> lines(const char* text) {
    std::vector<std::string> out;
    if (!text || !*text) return out;
    std::string cur;
    for (const char* p = text; *p; ++p) {
        if (*p == '\n') { out.push_back(cur); cur.clear(); }
        else if (*p != '\r') cur.push_back(*p);
    }
    out.push_back(cur);
    return out;
}

UiWidget* W(uint32_t id) { return g_tree.get(id); }

} // namespace

extern "C" {

// ---- the frame ----------------------------------------------------------------------------------

void aver_ui_widgets_frame(float dt) {
    if (!abi::frameStarted()) return;
    float vp[4];
    abi::viewport(vp);
    g_tree.setViewport(UiRect{vp[0], vp[1], vp[2], vp[3]});

    float px = 0, py = 0;
    std::uint32_t buttons = 0;
    abi::pointer(px, py, buttons);
    g_in.pointerX = px;
    g_in.pointerY = py;
    g_in.buttons = buttons;
    g_in.pointerValid = g_pointerValid;

    UiDrawListPainter painter(abi::drawList(), abi::font());
    g_tree.update(dt, g_in, painter);
    g_tree.draw(painter);

    g_in.nav = 0;
    g_in.chars.clear();
    g_in.editKeys.clear();
    g_in.rawKey = -1;
    g_in.wheel = 0.0f;
}

void aver_ui_input_nav(int32_t nav) {
    if (nav >= 0 && nav <= AVER_UI_NAV_PAGE_DOWN) g_in.nav |= 1u << static_cast<unsigned>(nav);
}
void aver_ui_input_char(uint32_t cp) { g_in.chars.push_back(cp); }
void aver_ui_input_edit_key(int32_t key) {
    if (key >= 0 && key <= AVER_UI_EDIT_ENTER) g_in.editKeys.push_back(static_cast<UiEditKey>(key));
}
void aver_ui_input_wheel(float notches) { g_in.wheel += notches; }
void aver_ui_input_raw_key(int32_t slot) { g_in.rawKey = slot; }
void aver_ui_input_pointer_valid(int32_t valid) { g_pointerValid = valid != 0; }

void aver_ui_input_host_frame(float dt, const uint8_t* keyPressed256, const uint8_t* keyHeld256,
                              const int16_t* vkToSlot256, int32_t padConnected, const uint8_t* padButtons14,
                              const float* padAxes6) {
    static UiHostSnapshot snap;
    for (int i = 0; i < 256; ++i) {
        snap.keyPressed[i] = keyPressed256 && keyPressed256[i];
        snap.keyHeld[i] = keyHeld256 && keyHeld256[i];
    }
    snap.padConnected = padConnected != 0;
    for (int i = 0; i < 14; ++i) snap.padButton[i] = padButtons14 && padButtons14[i];
    for (int i = 0; i < 6; ++i) snap.padAxis[i] = padAxes6 ? padAxes6[i] : 0.0f;

    const i32 vk = g_mapper.map(snap, dt, g_in);
    if (vk >= 0 && vkToSlot256 && vkToSlot256[vk] >= 0) g_in.rawKey = vkToSlot256[vk];
}

uint32_t aver_ui_wants_input(void) {
    const UiInputState& s = g_tree.inputState();
    uint32_t bits = 0;
    if (s.wantsPointer()) bits |= AVER_UI_WANTS_POINTER;
    if (s.wantsKeyboard()) bits |= AVER_UI_WANTS_KEYBOARD;
    if (s.pausesGame()) bits |= AVER_UI_WANTS_PAUSE;
    if (s.menuOpen) bits |= AVER_UI_WANTS_CURSOR;
    return bits;
}

// ---- environment ------------------------------------------------------------------------------------

void aver_ui_set_theme(const char* name) { g_tree.setTheme(uiBuiltinTheme(name ? name : "dark")); }

void aver_ui_set_dpi(int32_t mode, float refWidth, float refHeight, float userScale) {
    UiDpi d = g_tree.dpi();
    if (mode >= 0 && mode <= 4) d.mode = static_cast<UiScaleMode>(mode);
    if (refWidth > 0.0f) d.refWidth = refWidth;
    if (refHeight > 0.0f) d.refHeight = refHeight;
    if (userScale > 0.0f) d.userScale = userScale;
    g_tree.setDpi(d);
}

float aver_ui_scale(void) { return g_tree.scale(); }

// ---- structure ----------------------------------------------------------------------------------------

uint32_t aver_ui_widget_create(int32_t kind, uint32_t parent, const char* name) {
    if (kind < 0 || kind >= static_cast<int32_t>(UiWidgetKind::Count)) return 0;
    return g_tree.create(static_cast<UiWidgetKind>(kind), parent, name ? name : "");
}

int32_t aver_ui_widget_destroy(uint32_t id) {
    if (g_screen && g_screen->root() == id) g_screen.reset();
    return g_tree.destroy(id) ? 1 : 0;
}

int32_t aver_ui_widget_reparent(uint32_t id, uint32_t newParent, int32_t index) {
    return g_tree.reparent(id, newParent, index) ? 1 : 0;
}

uint32_t aver_ui_widget_find(const char* name, uint32_t under) { return name ? g_tree.find(name, under) : 0; }
uint32_t aver_ui_widget_parent(uint32_t id) { const UiWidget* w = W(id); return w ? w->parent : 0; }
uint32_t aver_ui_widget_root(uint32_t id) { return g_tree.rootOf(id); }
int32_t aver_ui_widget_child_count(uint32_t id) {
    const UiWidget* w = W(id);
    return w ? static_cast<int32_t>(w->children.size()) : 0;
}
uint32_t aver_ui_widget_child_at(uint32_t id, int32_t index) {
    const UiWidget* w = W(id);
    return (w && index >= 0 && static_cast<usize>(index) < w->children.size()) ? w->children[static_cast<usize>(index)] : 0;
}
int32_t aver_ui_widget_kind(uint32_t id) { const UiWidget* w = W(id); return w ? static_cast<int32_t>(w->kind) : -1; }
int32_t aver_ui_widget_exists(uint32_t id) { return W(id) ? 1 : 0; }

int32_t aver_ui_widget_set_prop(uint32_t id, const char* key, const char* valueText) {
    UiWidget* w = W(id);
    if (!w || !key) return 0;
    return uiSetProp(*w, key, valueText ? valueText : "") ? 1 : 0;
}

const char* aver_ui_widget_get_prop(uint32_t id, const char* key) {
    const UiWidget* w = W(id);
    std::string out;
    if (!w || !key || !uiGetProp(*w, key, out)) return nullptr;
    return keep(std::move(out));
}

void aver_ui_widget_set_text(uint32_t id, const char* utf8) { g_tree.setText(id, utf8 ? utf8 : ""); }
const char* aver_ui_widget_get_text(uint32_t id) {
    const UiWidget* w = W(id);
    return keep(w ? w->text : std::string());
}
void aver_ui_widget_set_value(uint32_t id, float value) { g_tree.setValue(id, value); }
float aver_ui_widget_get_value(uint32_t id) { const UiWidget* w = W(id); return w ? w->value : 0.0f; }
void aver_ui_widget_set_checked(uint32_t id, int32_t checked) { g_tree.setChecked(id, checked != 0); }
int32_t aver_ui_widget_get_checked(uint32_t id) { const UiWidget* w = W(id); return (w && w->checked) ? 1 : 0; }
void aver_ui_widget_set_selected(uint32_t id, int32_t index) { g_tree.setSelected(id, index); }
int32_t aver_ui_widget_get_selected(uint32_t id) { const UiWidget* w = W(id); return w ? w->selected : 0; }
void aver_ui_widget_set_items(uint32_t id, const char* newlineSeparated) {
    if (UiWidget* w = W(id)) {
        w->items = lines(newlineSeparated);
        g_tree.setSelected(id, w->selected);
    }
}
void aver_ui_widget_set_visible(uint32_t id, int32_t visible) { g_tree.setVisible(id, visible != 0); }
int32_t aver_ui_widget_get_visible(uint32_t id) { return g_tree.isVisible(id) ? 1 : 0; }
void aver_ui_widget_set_enabled(uint32_t id, int32_t enabled) { if (UiWidget* w = W(id)) w->enabled = enabled != 0; }
void aver_ui_widget_set_texture(uint32_t id, uint64_t texture) { if (UiWidget* w = W(id)) w->texture = texture; }

void aver_ui_widget_rect(uint32_t id, float* outXYWH) {
    if (!outXYWH) return;
    const UiWidget* w = W(id);
    const UiRect r = w ? w->rect : UiRect{};
    outXYWH[0] = r.x; outXYWH[1] = r.y; outXYWH[2] = r.w; outXYWH[3] = r.h;
}

int32_t aver_ui_set_focus(uint32_t id) { return g_tree.setFocus(id) ? 1 : 0; }
uint32_t aver_ui_focused(void) { return g_tree.focused(); }
uint32_t aver_ui_hit_widget(float x, float y) { return g_tree.hitTest(x, y); }

// ---- layouts ----------------------------------------------------------------------------------------------

uint32_t aver_ui_layout_open(const char* utf8Text, int32_t applyEnvironment) {
    UiLayoutDoc doc;
    std::string err;
    if (!uiParseLayout(utf8Text ? utf8Text : "", doc, &err)) {
        g_error = err;
        return 0;
    }
    const UiWidgetId root = uiInstantiateLayout(doc, g_tree, 0);
    if (!root) {
        g_error = "the layout's widgets could not be created";
        return 0;
    }
    if (applyEnvironment) {
        g_tree.setTheme(uiBuiltinTheme(doc.theme));
        UiDpi d = doc.dpi;
        d.userScale = g_tree.dpi().userScale;
        d.platformScale = g_tree.dpi().platformScale;
        g_tree.setDpi(d);
    }
    g_error.clear();
    return root;
}

const char* aver_ui_layout_error(void) { return keep(g_error); }

const char* aver_ui_layout_capture(uint32_t root) {
    UiLayoutDoc doc;
    if (!uiCaptureLayout(g_tree, root, doc)) return nullptr;
    return keep(uiWriteLayout(doc));
}

// ---- events -------------------------------------------------------------------------------------------------

int32_t aver_ui_event_poll(int32_t* outType, uint32_t* outWidget, float* outValue, int32_t* outIndex,
                           char* textBuf, int32_t textCap) {
    UiEvent e;
    if (!g_tree.pollEvent(e)) return 0;
    if (outType) *outType = static_cast<int32_t>(e.type);
    if (outWidget) *outWidget = e.widget;
    if (outValue) *outValue = e.value;
    if (outIndex) *outIndex = e.index;
    if (textBuf && textCap > 0) {
        const usize n = std::min(e.text.size(), static_cast<usize>(textCap - 1));
        std::memcpy(textBuf, e.text.data(), n);
        textBuf[n] = '\0';
    }
    return 1;
}

// ---- ready-made screens ---------------------------------------------------------------------------------------

uint32_t aver_ui_menu_create(const char* rootName, const char* title, const char* entries, int32_t modal,
                             const char* cancelCommand) {
    std::vector<UiMenuEntry> list;
    for (const std::string& line : lines(entries)) {
        if (line.empty()) continue;
        UiMenuEntry e;
        const usize bar = line.find('|');
        e.label = line.substr(0, bar);
        if (bar != std::string::npos) e.command = line.substr(bar + 1);
        if (!e.label.empty() && e.label[0] == '!') { e.danger = true; e.label.erase(0, 1); }
        list.push_back(std::move(e));
    }
    UiMenuOptions o;
    o.modal = modal != 0;
    o.cancelCommand = str(cancelCommand);
    return uiBuildMenu(g_tree, rootName ? rootName : "Menu", str(title), list, o);
}

void aver_ui_settings_reset_default(void) { g_settings = uiDefaultSettingsModel(); }
void aver_ui_settings_clear(void) { g_settings.items.clear(); }

void aver_ui_settings_add(int32_t type, const char* key, const char* label, const char* tab, float value,
                          float minValue, float maxValue, float step, const char* choices) {
    if (!key || !*key) return;
    UiSettingItem i;
    i.type = type == 0 ? UiSettingItem::Type::Toggle : (type == 2 ? UiSettingItem::Type::Choice : UiSettingItem::Type::Slider);
    i.key = key;
    i.label = label ? label : key;
    i.tab = tab ? tab : "General";
    i.value = value;
    i.minValue = minValue;
    i.maxValue = maxValue;
    i.step = step;
    i.choices = lines(choices);
    if (UiSettingItem* existing = g_settings.find(key)) *existing = std::move(i);
    else g_settings.items.push_back(std::move(i));
}

void aver_ui_settings_set_value(const char* key, float value) {
    if (UiSettingItem* i = key ? g_settings.find(key) : nullptr) {
        i->value = value;
        if (g_screen) g_screen->refresh();
    }
}

float aver_ui_settings_value(const char* key) {
    const UiSettingItem* i = key ? g_settings.find(key) : nullptr;
    return i ? i->value : 0.0f;
}

void aver_ui_rebind_clear(void) { g_rebind.rows.clear(); }

void aver_ui_rebind_add(const char* action, int32_t slot, const char* label, const char* bindingText,
                        int32_t rebindable) {
    UiRebindRow r;
    r.action = str(action);
    r.slot = slot;
    r.label = label ? label : r.action;
    r.binding = str(bindingText);
    r.rebindable = rebindable != 0;
    g_rebind.rows.push_back(std::move(r));
}

void aver_ui_rebind_set_binding(int32_t row, const char* bindingText) {
    if (row < 0 || static_cast<usize>(row) >= g_rebind.rows.size()) return;
    if (g_screen) g_screen->setBinding(static_cast<usize>(row), str(bindingText));
    else g_rebind.rows[static_cast<usize>(row)].binding = str(bindingText);
}

uint32_t aver_ui_settings_build(const char* rootName) {
    if (g_screen) {
        const UiWidgetId old = g_screen->root();
        g_screen.reset();
        if (old) g_tree.destroy(old);
    }
    g_screen = std::make_unique<UiSettingsScreen>(g_tree, g_settings, g_rebind.rows.empty() ? nullptr : &g_rebind);
    return g_screen->build(rootName ? rootName : "SettingsScreen");
}

} // extern "C"
