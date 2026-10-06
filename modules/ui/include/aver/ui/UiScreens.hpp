#pragma once
// Ready-made general screens built from the widget set: a settings screen (graphics, audio, and a
// controls page that rebinds keys) and a plain command menu. They know nothing about the renderer,
// the mixer or the input system: a host fills a model, the screen edits it, and changes leave as
// Command events ("setting:<key>", "rebind:<action>:<slot>") the host applies and persists.
// See docs/GAME_UI.md for how Aver.Framework wires them to Settings, Audio and EnhancedInput.
#include "aver/ui/UiTree.hpp"

#include <functional>
#include <string>
#include <vector>

namespace aver::ui {

struct UiSettingItem {
    enum class Type : u8 { Toggle, Slider, Choice };
    Type type = Type::Slider;
    std::string key;          // stable id and persistence key, e.g. "audio.master"
    std::string label;
    std::string tab;          // the page it sits on
    f32 value = 0;            // toggle 0/1, slider value, choice index
    f32 minValue = 0, maxValue = 1, step = 0;
    std::vector<std::string> choices;
};

struct UiSettingsModel {
    std::vector<UiSettingItem> items;
    std::function<void(const UiSettingItem&)> onChange;   // optional; Command events are raised too

    UiSettingItem* find(std::string_view key);
    const UiSettingItem* find(std::string_view key) const;
    // Distinct tab names in order of first use.
    std::vector<std::string> tabs() const;
};

// Graphics (quality, render scale, TAA, vsync, fullscreen, UI scale) and Audio (master, music, sfx,
// voice). "graphics.uiScale" is applied by the screen itself: it drives the tree's DPI user scale.
UiSettingsModel uiDefaultSettingsModel();

struct UiRebindRow {
    std::string action;       // the input action's name
    i32 slot = 0;             // which binding of that action
    std::string label;        // what the player reads
    std::string binding;      // the current key as text
    bool rebindable = true;
};

struct UiRebindModel {
    std::vector<UiRebindRow> rows;
    std::string hint = "Press a key...";   // shown while a row listens
};

// Builds and drives a settings screen in a tree. Must not outlive the tree or the models.
class UiSettingsScreen {
public:
    UiSettingsScreen(UiTree& tree, UiSettingsModel& settings, UiRebindModel* rebind = nullptr);
    ~UiSettingsScreen();
    UiSettingsScreen(const UiSettingsScreen&) = delete;
    UiSettingsScreen& operator=(const UiSettingsScreen&) = delete;

    // Creates the widgets (a Menu-mode root named `rootName`) and starts listening to the tree.
    // Esc / Cancel and the Back button close it ("ui.close"). Returns the root id.
    UiWidgetId build(std::string_view rootName = "SettingsScreen");
    UiWidgetId root() const { return root_; }

    // Copies the models into the widgets (after the host changed a value or a binding).
    void refresh();
    void showTab(usize index);
    usize tab() const { return tab_; }
    // After the host applied a rebind: updates the row and its widget.
    void setBinding(usize row, std::string_view bindingText);

private:
    void onEvent(const UiEvent& e);
    void applyItem(UiSettingItem& item, const UiEvent& e);

    UiTree& tree_;
    UiSettingsModel& settings_;
    UiRebindModel* rebind_;
    UiWidgetId root_ = 0;
    usize listener_ = 0;
    usize tab_ = 0;
    std::vector<std::string> tabNames_;
};

struct UiMenuEntry {
    std::string label;
    std::string command;      // raised as a Command event when chosen
    bool danger = false;      // drawn with the "danger" style
};

struct UiMenuOptions {
    bool modal = true;            // dim the world behind it
    std::string cancelCommand;    // raised when Cancel is pressed; empty = nothing
    f32 width = 420;
};

// A centred titled panel of buttons. Returns the root (a Menu-mode root named `rootName`).
UiWidgetId uiBuildMenu(UiTree& tree, std::string_view rootName, std::string_view title,
                       const std::vector<UiMenuEntry>& entries, const UiMenuOptions& options = {});

} // namespace aver::ui
