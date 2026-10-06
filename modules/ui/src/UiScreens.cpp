// The settings screen, the controls (rebinding) page and the command menu.
#include "aver/ui/UiScreens.hpp"

#include <algorithm>
#include <cstdlib>

namespace aver::ui {

namespace {

UiWidget& make(UiTree& t, UiWidgetId parent, UiWidgetKind kind, std::string_view name) {
    return *t.get(t.create(kind, parent, name));
}

// A container with no look of its own.
UiWidget& makeBox(UiTree& t, UiWidgetId parent, std::string_view name, UiLayoutMode layout, f32 spacing) {
    UiWidget& w = make(t, parent, UiWidgetKind::Panel, name);
    w.style = "clear";
    w.layout = layout;
    w.spacing = spacing;
    return w;
}

UiSettingItem slider(const char* key, const char* label, const char* tab, f32 v, f32 lo, f32 hi, f32 step) {
    UiSettingItem i;
    i.type = UiSettingItem::Type::Slider;
    i.key = key;
    i.label = label;
    i.tab = tab;
    i.value = v;
    i.minValue = lo;
    i.maxValue = hi;
    i.step = step;
    return i;
}

UiSettingItem toggle(const char* key, const char* label, const char* tab, bool on) {
    UiSettingItem i;
    i.type = UiSettingItem::Type::Toggle;
    i.key = key;
    i.label = label;
    i.tab = tab;
    i.value = on ? 1.0f : 0.0f;
    return i;
}

} // namespace

// ---- model ----------------------------------------------------------------------------------------

UiSettingItem* UiSettingsModel::find(std::string_view key) {
    for (UiSettingItem& i : items) if (i.key == key) return &i;
    return nullptr;
}

const UiSettingItem* UiSettingsModel::find(std::string_view key) const {
    for (const UiSettingItem& i : items) if (i.key == key) return &i;
    return nullptr;
}

std::vector<std::string> UiSettingsModel::tabs() const {
    std::vector<std::string> out;
    for (const UiSettingItem& i : items)
        if (std::find(out.begin(), out.end(), i.tab) == out.end()) out.push_back(i.tab);
    return out;
}

UiSettingsModel uiDefaultSettingsModel() {
    UiSettingsModel m;
    UiSettingItem quality;
    quality.type = UiSettingItem::Type::Choice;
    quality.key = "graphics.quality";
    quality.label = "Quality";
    quality.tab = "Graphics";
    quality.choices = {"Low", "Medium", "High", "Ultra"};
    quality.value = 2.0f;
    m.items.push_back(quality);
    m.items.push_back(slider("graphics.renderScale", "Render scale", "Graphics", 1.0f, 0.5f, 1.0f, 0.05f));
    m.items.push_back(toggle("graphics.taa", "Anti-aliasing (TAA)", "Graphics", true));
    m.items.push_back(toggle("graphics.vsync", "VSync", "Graphics", true));
    m.items.push_back(toggle("graphics.fullscreen", "Fullscreen", "Graphics", false));
    m.items.push_back(slider("graphics.uiScale", "UI scale", "Graphics", 1.0f, 0.75f, 1.5f, 0.05f));
    m.items.push_back(slider("audio.master", "Master volume", "Audio", 1.0f, 0.0f, 1.0f, 0.05f));
    m.items.push_back(slider("audio.music", "Music volume", "Audio", 0.8f, 0.0f, 1.0f, 0.05f));
    m.items.push_back(slider("audio.sfx", "Effects volume", "Audio", 1.0f, 0.0f, 1.0f, 0.05f));
    m.items.push_back(slider("audio.voice", "Voice volume", "Audio", 1.0f, 0.0f, 1.0f, 0.05f));
    return m;
}

// ---- settings screen ----------------------------------------------------------------------------------

UiSettingsScreen::UiSettingsScreen(UiTree& tree, UiSettingsModel& settings, UiRebindModel* rebind)
    : tree_(tree), settings_(settings), rebind_(rebind) {}

UiSettingsScreen::~UiSettingsScreen() {
    if (listener_) tree_.removeListener(listener_);
}

UiWidgetId UiSettingsScreen::build(std::string_view rootName) {
    if (root_) tree_.destroy(root_);
    if (listener_) { tree_.removeListener(listener_); listener_ = 0; }

    tabNames_ = settings_.tabs();
    if (rebind_) tabNames_.push_back("Controls");

    UiWidget& root = make(tree_, 0, UiWidgetKind::Panel, rootName);
    root_ = root.id;
    root.style = "clear";
    root.inputMode = UiInputMode::Menu;
    root.modal = true;
    root.zOrder = 10;
    root.cancelCommand = "ui.close";

    UiWidget& win = make(tree_, root_, UiWidgetKind::Panel, "Window");
    win.anchors = UiAnchors{0.5f, 0.5f, 0.5f, 0.5f};
    win.pivot = {0.5f, 0.5f};
    win.width = 780;
    win.height = 640;
    win.layout = UiLayoutMode::VStack;
    win.spacing = 12;
    win.padding = UiInsets{20, 20, 20, 20};
    const UiWidgetId winId = win.id;

    UiWidget& title = make(tree_, winId, UiWidgetKind::Text, "Title");
    title.text = "Settings";
    title.style = "title";

    UiWidget& bar = makeBox(tree_, winId, "TabBar", UiLayoutMode::HStack, 8);
    const UiWidgetId barId = bar.id;
    for (usize i = 0; i < tabNames_.size(); ++i) {
        UiWidget& b = make(tree_, barId, UiWidgetKind::Button, "tab." + std::to_string(i));
        b.text = tabNames_[i];
        b.fillW = 1.0f;
    }

    UiWidget& pages = makeBox(tree_, winId, "Pages", UiLayoutMode::None, 0);
    pages.fillH = 1.0f;
    const UiWidgetId pagesId = pages.id;

    std::string firstControl;
    for (usize p = 0; p < tabNames_.size(); ++p) {
        UiWidget& page = make(tree_, pagesId, UiWidgetKind::Scroll, "page." + std::to_string(p));
        page.anchors = UiAnchors{0, 0, 1, 1};
        page.spacing = 10;
        page.padding = UiInsets{8, 8, 16, 8};
        const UiWidgetId pageId = page.id;

        const bool controls = rebind_ && p + 1 == tabNames_.size();
        if (!controls) {
            for (const UiSettingItem& item : settings_.items) {
                if (item.tab != tabNames_[p]) continue;
                UiWidget& row = makeBox(tree_, pageId, "row." + item.key, UiLayoutMode::HStack, 16);
                row.minHeight = 36;
                const UiWidgetId rowId = row.id;
                UiWidget& label = make(tree_, rowId, UiWidgetKind::Text, "label." + item.key);
                label.text = item.label;
                label.fillW = 1.0f;
                label.alignV = UiAlign::Center;

                const std::string name = "setting." + item.key;
                if (item.type == UiSettingItem::Type::Toggle) {
                    UiWidget& c = make(tree_, rowId, UiWidgetKind::Toggle, name);
                    c.width = 300;
                    c.alignV = UiAlign::Center;
                } else if (item.type == UiSettingItem::Type::Slider) {
                    UiWidget& c = make(tree_, rowId, UiWidgetKind::Slider, name);
                    c.width = 300;
                    c.height = 28;
                    c.alignV = UiAlign::Center;
                    c.minValue = item.minValue;
                    c.maxValue = item.maxValue;
                    c.step = item.step;
                    c.showValue = true;
                } else {
                    UiWidget& c = make(tree_, rowId, UiWidgetKind::Choice, name);
                    c.width = 300;
                    c.alignV = UiAlign::Center;
                    c.items = item.choices;
                }
                if (p == 0 && firstControl.empty()) firstControl = name;
            }
        } else {
            for (usize i = 0; i < rebind_->rows.size(); ++i) {
                const UiRebindRow& r = rebind_->rows[i];
                UiWidget& row = makeBox(tree_, pageId, "bindrow." + std::to_string(i), UiLayoutMode::HStack, 16);
                row.minHeight = 36;
                const UiWidgetId rowId = row.id;
                UiWidget& label = make(tree_, rowId, UiWidgetKind::Text, "bindlabel." + std::to_string(i));
                label.text = r.label.empty() ? r.action : r.label;
                label.fillW = 1.0f;
                label.alignV = UiAlign::Center;
                UiWidget& k = make(tree_, rowId, UiWidgetKind::KeyBind, "bind." + std::to_string(i));
                k.width = 300;
                k.alignV = UiAlign::Center;
                k.placeholder = rebind_->hint;
                k.enabled = r.rebindable;
                if (p == 0 && firstControl.empty()) firstControl = k.name;
            }
        }
    }

    UiWidget& footer = makeBox(tree_, winId, "Footer", UiLayoutMode::HStack, 12);
    footer.justify = UiJustify::End;
    const UiWidgetId footerId = footer.id;
    if (rebind_) {
        UiWidget& reset = make(tree_, footerId, UiWidgetKind::Button, "ResetControls");
        reset.text = "Reset controls";
        reset.command = "rebind.reset";
    }
    UiWidget& back = make(tree_, footerId, UiWidgetKind::Button, "Back");
    back.text = "Back";
    back.command = "ui.close";
    back.width = 160;

    if (UiWidget* r = tree_.get(root_)) r->defaultFocus = firstControl.empty() ? "Back" : firstControl;

    listener_ = tree_.addListener([this](const UiEvent& e) { onEvent(e); });
    refresh();
    showTab(0);
    return root_;
}

void UiSettingsScreen::refresh() {
    if (!root_) return;
    for (const UiSettingItem& item : settings_.items) {
        UiWidget* w = tree_.get(tree_.find("setting." + item.key, root_));
        if (!w) continue;
        if (item.type == UiSettingItem::Type::Toggle) w->checked = item.value > 0.5f;
        else if (item.type == UiSettingItem::Type::Slider) w->value = std::clamp(item.value, item.minValue, item.maxValue);
        else w->selected = static_cast<i32>(item.value);
    }
    if (rebind_) {
        for (usize i = 0; i < rebind_->rows.size(); ++i)
            if (UiWidget* w = tree_.get(tree_.find("bind." + std::to_string(i), root_))) w->text = rebind_->rows[i].binding;
    }
}

void UiSettingsScreen::showTab(usize index) {
    if (!root_ || tabNames_.empty()) return;
    tab_ = std::min(index, tabNames_.size() - 1);
    for (usize i = 0; i < tabNames_.size(); ++i) {
        if (UiWidget* page = tree_.get(tree_.find("page." + std::to_string(i), root_))) page->visible = i == tab_;
        if (UiWidget* b = tree_.get(tree_.find("tab." + std::to_string(i), root_))) b->style = i == tab_ ? "" : "tab";
    }
    // Focus may have been on a control of the page that just hid.
    if (!tree_.focused() || !tree_.isVisible(tree_.focused())) {
        UiWidgetId first = 0;
        const std::vector<UiWidgetId> order = uiTabOrder(tree_.focusItems(root_));
        if (!order.empty()) first = order.front();
        if (first) tree_.setFocus(first);
    }
}

void UiSettingsScreen::setBinding(usize row, std::string_view text) {
    if (!rebind_ || row >= rebind_->rows.size()) return;
    rebind_->rows[row].binding = std::string(text);
    if (UiWidget* w = tree_.get(tree_.find("bind." + std::to_string(row), root_))) w->text = std::string(text);
}

void UiSettingsScreen::applyItem(UiSettingItem& item, const UiEvent& e) {
    if (item.type == UiSettingItem::Type::Choice) item.value = static_cast<f32>(e.index);
    else item.value = e.value;

    if (item.key == "graphics.uiScale") {
        UiDpi d = tree_.dpi();
        d.userScale = item.value;
        tree_.setDpi(d);
    }
    if (settings_.onChange) settings_.onChange(item);

    UiEvent c;
    c.type = UiEventType::Command;
    c.widget = e.widget;
    c.text = "setting:" + item.key;
    c.value = item.value;
    c.index = static_cast<i32>(item.value);
    tree_.emit(std::move(c));
}

void UiSettingsScreen::onEvent(const UiEvent& e) {
    const UiWidget* w = tree_.get(e.widget);
    if (!w || tree_.rootOf(e.widget) != root_) return;
    const std::string& n = w->name;

    if (e.type == UiEventType::Clicked && n.rfind("tab.", 0) == 0) {
        showTab(static_cast<usize>(std::max(0, std::atoi(n.c_str() + 4))));
    } else if ((e.type == UiEventType::Toggled || e.type == UiEventType::ValueChanged) && n.rfind("setting.", 0) == 0) {
        if (UiSettingItem* item = settings_.find(std::string_view(n).substr(8))) applyItem(*item, e);
    } else if (e.type == UiEventType::KeyCaptured && n.rfind("bind.", 0) == 0 && rebind_) {
        const usize row = static_cast<usize>(std::max(0, std::atoi(n.c_str() + 5)));
        if (row >= rebind_->rows.size()) return;
        const UiRebindRow& r = rebind_->rows[row];
        UiEvent c;
        c.type = UiEventType::Command;
        c.widget = e.widget;
        c.text = "rebind:" + r.action + ":" + std::to_string(r.slot);
        c.index = e.index;
        c.value = static_cast<f32>(e.index);
        tree_.emit(std::move(c));
    }
}

// ---- command menu -------------------------------------------------------------------------------------

UiWidgetId uiBuildMenu(UiTree& tree, std::string_view rootName, std::string_view title,
                       const std::vector<UiMenuEntry>& entries, const UiMenuOptions& options) {
    UiWidget& root = make(tree, 0, UiWidgetKind::Panel, rootName);
    const UiWidgetId rootId = root.id;
    root.style = "clear";
    root.inputMode = UiInputMode::Menu;
    root.modal = options.modal;
    root.cancelCommand = options.cancelCommand;
    root.zOrder = 5;

    UiWidget& win = make(tree, rootId, UiWidgetKind::Panel, "Window");
    win.anchors = UiAnchors{0.5f, 0.5f, 0.5f, 0.5f};
    win.pivot = {0.5f, 0.5f};
    win.width = options.width;
    win.layout = UiLayoutMode::VStack;
    win.spacing = 12;
    win.padding = UiInsets{24, 24, 24, 24};
    const UiWidgetId winId = win.id;

    if (!title.empty()) {
        UiWidget& t = make(tree, winId, UiWidgetKind::Text, "Title");
        t.text = std::string(title);
        t.style = "title";
        t.textAlign = UiTextAlign::Center;
    }
    std::string first;
    for (usize i = 0; i < entries.size(); ++i) {
        UiWidget& b = make(tree, winId, UiWidgetKind::Button, "entry." + std::to_string(i));
        b.text = entries[i].label;
        b.command = entries[i].command;
        if (entries[i].danger) b.style = "danger";
        if (first.empty()) first = b.name;
    }
    if (UiWidget* r = tree.get(rootId)) r->defaultFocus = first;
    return rootId;
}

} // namespace aver::ui
