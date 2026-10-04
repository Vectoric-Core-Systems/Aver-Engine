// The .ocinput editor tab. See the header for the three-line SandboxApp hook, why this tab merges
// onto the file's own text on save, and InputSchemeEditorHooks for how it reaches SandboxApp.
#include "InputSchemeEditor.hpp"

#include "EditorKeybinds.hpp"
#include "InputKeyNames.hpp"
#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>

#if AVER_WITH_IMGUI
#  include "EditorIcons.hpp"
#  include "imgui.h"
#endif

namespace aver::editor {
namespace {
InputSchemeEditorHooks g_hooks;
} // namespace

void setInputSchemeEditorHooks(InputSchemeEditorHooks hooks) { g_hooks = std::move(hooks); }

InputSchemeEditor::InputSchemeEditor(std::string path) : path_(std::move(path)) { loadFromDisk(); }

void InputSchemeEditor::loadFromDisk() {
    std::string text;
    if (!readFileText(path_, text)) {
        loaded_ = false;
        loadError_ = "could not read " + path_;
        return;
    }
    fmt::OcInputData loaded;
    std::string err;
    if (!fmt::parseOcinput(text, loaded, &err)) {
        loaded_ = false;
        loadError_ = err.empty() ? "failed to parse .ocinput" : err;
        return;
    }
    originalText_ = std::move(text);
    data_ = std::move(loaded);
    loaded_ = true;
    loadError_.clear();
    dirty_ = false;
    history_.clear();
}

std::string InputSchemeEditor::title() const {
    // No manual dirty marker: the host applies ImGuiWindowFlags_UnsavedDocument for every editor
    // whose dirty() is true (AssetEditor.cpp) -- see FoliageTypeEditor::title()'s own comment for
    // the doubled-marker bug this avoids.
    return std::filesystem::path(path_).filename().string() + "###inputscheme:" + path_;
}

bool InputSchemeEditor::save(std::string* why) {
    if (!loaded_) {
        if (why) *why = "cannot save: file failed to load (" + loadError_ + ")";
        return false;
    }
    // Merged against the file's own last-loaded text, not written from scratch -- see this file's
    // header comment and GraphEditor::save()'s identical reason.
    const std::string text = fmt::writeOcinput(data_, originalText_);
    if (!writeFileTextAtomic(path_, text)) {
        if (why) *why = "could not write " + path_;
        return false;
    }
    originalText_ = text;   // the next save merges against what is now actually on disk
    dirty_ = false;
    return true;
}

void InputSchemeEditor::onFileChanged() {
    // A DIRTY TAB KEEPS ITS EDITS -- FoliageTypeEditor/BtEditor/SoundEditor's own rule: reloading
    // here would discard what the author typed because something else touched the file.
    if (dirty_) {
        AVER_WARN("[InputSchemeEditor] '{}' changed on disk, but this tab has unsaved edits -- "
                  "keeping them", path_);
        return;
    }
    loadFromDisk();
}

// ---- undo -----------------------------------------------------------------------------------------

void InputSchemeEditor::pushUndo() { history_.push(data_); }

void InputSchemeEditor::undo() {
    if (!history_.undo(data_)) return;
    dirty_ = true;
}

void InputSchemeEditor::redo() {
    if (!history_.redo(data_)) return;
    dirty_ = true;
}

// ---- actions ----------------------------------------------------------------------------------

void InputSchemeEditor::addAction() {
    if (!loaded_) return;
    pushUndo();
    std::string name = "NewAction";
    for (int n = 1; ; ++n) {
        bool clash = false;
        for (const auto& a : data_.actions) if (a.name == name) { clash = true; break; }
        if (!clash) break;
        name = "NewAction" + std::to_string(n);
    }
    fmt::OcInputAction a;
    a.name = name;
    a.type = fmt::OcInputValueType::Digital;
    data_.actions.push_back(std::move(a));
    dirty_ = true;
}

void InputSchemeEditor::removeAction(usize index) {
    if (!loaded_ || index >= data_.actions.size()) return;
    pushUndo();
    const std::string name = data_.actions[index].name;
    data_.actions.erase(data_.actions.begin() + static_cast<isize>(index));
    // CASCADE -- see this method's own header comment for why an orphaned binding cannot be left.
    data_.bindings.erase(
        std::remove_if(data_.bindings.begin(), data_.bindings.end(),
                       [&](const fmt::OcInputBinding& b) { return b.action == name; }),
        data_.bindings.end());
    dirty_ = true;
}

void InputSchemeEditor::renameAction(usize index, std::string newName) {
    if (!loaded_ || index >= data_.actions.size()) return;
    if (newName.empty() || data_.actions[index].name == newName) return;
    pushUndo();
    const std::string oldName = data_.actions[index].name;
    data_.actions[index].name = newName;
    for (auto& b : data_.bindings) if (b.action == oldName) b.action = newName;
    dirty_ = true;
}

void InputSchemeEditor::setActionType(usize index, fmt::OcInputValueType type) {
    if (!loaded_ || index >= data_.actions.size() || data_.actions[index].type == type) return;
    pushUndo();
    data_.actions[index].type = type;
    dirty_ = true;
}

// ---- bindings ----------------------------------------------------------------------------------

void InputSchemeEditor::addBinding() {
    if (!loaded_ || data_.actions.empty()) return;
    pushUndo();
    fmt::OcInputBinding b;
    b.action = data_.actions.front().name;
    b.source = fmt::OcInputSource::Key;
    b.key = kFwKeyNames[0];   // never empty -- a Key-source binding requires one (OcInput.hpp)
    data_.bindings.push_back(std::move(b));
    dirty_ = true;
}

void InputSchemeEditor::removeBinding(usize index) {
    if (!loaded_ || index >= data_.bindings.size()) return;
    pushUndo();
    data_.bindings.erase(data_.bindings.begin() + static_cast<isize>(index));
    dirty_ = true;
}

void InputSchemeEditor::setBindingAction(usize index, std::string actionName) {
    if (!loaded_ || index >= data_.bindings.size() || data_.bindings[index].action == actionName) return;
    pushUndo();
    data_.bindings[index].action = std::move(actionName);
    dirty_ = true;
}

void InputSchemeEditor::setBindingSource(usize index, fmt::OcInputSource source) {
    if (!loaded_ || index >= data_.bindings.size() || data_.bindings[index].source == source) return;
    pushUndo();
    fmt::OcInputBinding& b = data_.bindings[index];
    b.source = source;
    // See this method's own header comment: a Key/GamepadButton/GamepadAxis binding needs a
    // non-empty `key` the moment it becomes one of those three -- the PREVIOUS source (a mouse one)
    // may have left it empty, which parseOcinput refuses to reload ("BIND key requires a key name",
    // OcInput.cpp). Seed the first name in the NEW source's own vocabulary rather than leave a save
    // this tab could not itself reopen.
    if (b.key.empty()) {
        if (source == fmt::OcInputSource::Key) b.key = kFwKeyNames[0];
        else if (source == fmt::OcInputSource::GamepadButton) b.key = kFwGamepadButtonNames[0];
        else if (source == fmt::OcInputSource::GamepadAxis) b.key = kFwGamepadAxisNames[0];
    }
    dirty_ = true;
}

void InputSchemeEditor::setBindingKey(usize index, std::string key) {
    if (!loaded_ || index >= data_.bindings.size() || data_.bindings[index].key == key) return;
    pushUndo();
    data_.bindings[index].key = std::move(key);
    dirty_ = true;
}

void InputSchemeEditor::setBindingScale(usize index, f32 scale) {
    if (!loaded_ || index >= data_.bindings.size()) return;
    pushUndo();
    data_.bindings[index].scale = scale;
    dirty_ = true;
}

void InputSchemeEditor::setBindingComponent(usize index, i32 component) {
    if (!loaded_ || index >= data_.bindings.size() || data_.bindings[index].component == component) return;
    pushUndo();
    data_.bindings[index].component = component;
    dirty_ = true;
}

// ---- context ------------------------------------------------------------------------------------

void InputSchemeEditor::setContextName(std::string name) {
    if (!loaded_ || data_.contextName == name) return;
    pushUndo();
    data_.contextName = std::move(name);
    dirty_ = true;
}

void InputSchemeEditor::setContextPriority(i32 priority) {
    if (!loaded_ || data_.contextPriority == priority) return;
    pushUndo();
    data_.contextPriority = priority;
    dirty_ = true;
}

// ---- drawing --------------------------------------------------------------------------------------

#if AVER_WITH_IMGUI

namespace {
const char* bindingSourceLabel(fmt::OcInputSource s) {
    switch (s) {
        case fmt::OcInputSource::Key:           return "Key";
        case fmt::OcInputSource::MouseX:        return "Mouse X";
        case fmt::OcInputSource::MouseY:        return "Mouse Y";
        case fmt::OcInputSource::MouseWheel:    return "Mouse Wheel";
        case fmt::OcInputSource::GamepadButton: return "Gamepad Button";
        case fmt::OcInputSource::GamepadAxis:   return "Gamepad Axis";
        default:                                return "Key";
    }
}
const char* componentLabel(i32 c) { return c == 1 ? "Y" : c == 2 ? "Z" : "X"; }

// Every source the bindings table's combo offers, in the order the format's own grammar comment
// lists them (OcInput.cpp's header) plus the two gamepad sources point B of this change adds.
constexpr fmt::OcInputSource kAllSources[] = {
    fmt::OcInputSource::Key,        fmt::OcInputSource::MouseX,        fmt::OcInputSource::MouseY,
    fmt::OcInputSource::MouseWheel, fmt::OcInputSource::GamepadButton, fmt::OcInputSource::GamepadAxis,
};
} // namespace

void InputSchemeEditor::drawContext() {
    ImGui::SeparatorText("Context");
    char nameBuf[96];
    std::snprintf(nameBuf, sizeof nameBuf, "%s", data_.contextName.c_str());
    ImGui::SetNextItemWidth(240.0f);
    ImGui::InputTextWithHint("Context name", "(none -- writes no CONTEXT record)", nameBuf, sizeof nameBuf);
    if (ImGui::IsItemDeactivatedAfterEdit()) setContextName(nameBuf);
    ImGui::TextDisabled("The name EnhancedInput.AddContext registers this scheme under.");

    int priority = data_.contextPriority;
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragInt("Priority", &priority);
    if (ImGui::IsItemDeactivatedAfterEdit()) setContextPriority(priority);
    ImGui::TextDisabled("Higher priority consumes a key before a lower-priority context sees it.");
}

void InputSchemeEditor::drawActions() {
    ImGui::SeparatorText("Actions");
    static const char* kTypeNames[] = {"Digital", "Axis1D", "Axis2D"};
    if (ImGui::BeginTable("inputActions", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                             ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 28.0f);
        ImGui::TableHeadersRow();
        // Direct in-loop removal, size checked fresh each iteration -- GraphEditor's own function-
        // pin list (GraphEditor.cpp) removes the same way: at most one row is skipped for the
        // remainder of THIS frame, which the next frame's redraw fixes, and nothing here holds a
        // reference across the erase.
        for (usize i = 0; i < data_.actions.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            char nameBuf[96];
            std::snprintf(nameBuf, sizeof nameBuf, "%s", data_.actions[i].name.c_str());
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##name", nameBuf, sizeof nameBuf);
            if (ImGui::IsItemDeactivatedAfterEdit()) renameAction(i, nameBuf);

            ImGui::TableNextColumn();
            int typeIdx = static_cast<int>(data_.actions[i].type);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##type", &typeIdx, kTypeNames, IM_ARRAYSIZE(kTypeNames)))
                setActionType(i, static_cast<fmt::OcInputValueType>(typeIdx));

            ImGui::TableNextColumn();
            if (ImGui::SmallButton(ICON_DELETE)) removeAction(i);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button(ICON_ADD " Add Action")) addAction();
}

void InputSchemeEditor::drawBindings() {
    ImGui::SeparatorText("Bindings");
    if (data_.actions.empty()) {
        ImGui::TextDisabled("Declare an action above before adding a binding.");
        return;
    }
    if (ImGui::BeginTable("inputBindings", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                              ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn("Scale", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Component", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 28.0f);
        ImGui::TableHeadersRow();
        for (usize i = 0; i < data_.bindings.size(); ++i) {
            const fmt::OcInputBinding& b = data_.bindings[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##action", b.action.c_str())) {
                for (const auto& a : data_.actions)
                    if (ImGui::Selectable(a.name.c_str(), a.name == b.action)) setBindingAction(i, a.name);
                ImGui::EndCombo();
            }

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##source", bindingSourceLabel(b.source))) {
                for (fmt::OcInputSource s : kAllSources)
                    if (ImGui::Selectable(bindingSourceLabel(s), s == b.source)) setBindingSource(i, s);
                ImGui::EndCombo();
            }

            ImGui::TableNextColumn();
            // Key/GamepadButton/GamepadAxis pick a NAME from that source's own vocabulary; the three
            // mouse sources ignore `key` entirely (OcInputBinding::key's own comment), so the column
            // shows nothing editable for them rather than a combo with no meaningful entries.
            if (b.source == fmt::OcInputSource::Key || b.source == fmt::OcInputSource::GamepadButton ||
                b.source == fmt::OcInputSource::GamepadAxis) {
                const char* const* names = b.source == fmt::OcInputSource::Key           ? kFwKeyNames
                                          : b.source == fmt::OcInputSource::GamepadButton ? kFwGamepadButtonNames
                                                                                          : kFwGamepadAxisNames;
                const usize count = b.source == fmt::OcInputSource::Key           ? kFwKeyNameCount
                                  : b.source == fmt::OcInputSource::GamepadButton ? kFwGamepadButtonNameCount
                                                                                  : kFwGamepadAxisNameCount;
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##key", b.key.empty() ? "(pick one)" : b.key.c_str())) {
                    for (usize k = 0; k < count; ++k)
                        if (ImGui::Selectable(names[k], b.key == names[k])) setBindingKey(i, names[k]);
                    ImGui::EndCombo();
                }
            } else {
                ImGui::TextDisabled("-");
            }

            ImGui::TableNextColumn();
            float scale = b.scale;
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat("##scale", &scale, 0.05f, -100.0f, 100.0f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) setBindingScale(i, scale);

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##component", componentLabel(b.component))) {
                for (i32 c = 0; c < 3; ++c)
                    if (ImGui::Selectable(componentLabel(c), c == b.component)) setBindingComponent(i, c);
                ImGui::EndCombo();
            }

            ImGui::TableNextColumn();
            if (ImGui::SmallButton(ICON_DELETE)) removeBinding(i);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button(ICON_ADD " Add Binding")) addBinding();
}

void InputSchemeEditor::drawProjectSchemeLine() {
    if (!g_hooks.contentDir) return;
    const std::string contentDir = g_hooks.contentDir();
    if (contentDir.empty()) return;   // no project open -- nothing to compare against
    std::error_code ec;
    std::string rel = std::filesystem::relative(path_, contentDir, ec).string();
    if (ec || rel.empty()) return;
    for (char& c : rel) if (c == '\\') c = '/';   // content-relative paths are stored '/'-separated
    const std::string projScheme = g_hooks.projectInputScheme ? g_hooks.projectInputScheme() : std::string();
    if (rel == projScheme) return;   // already the project's scheme -- nothing to offer

    ImGui::Dummy(ImVec2(0, 8.0f));
    ImGui::Separator();
    if (projScheme.empty())
        ImGui::TextDisabled("This project has no Input Scheme set (Project Settings > Description).");
    else
        ImGui::TextDisabled("This is not the project's Input Scheme ('%s').", projScheme.c_str());
    ImGui::BeginDisabled(!g_hooks.useAsProjectInputScheme);
    if (ImGui::Button("Use as Project Input Scheme") && g_hooks.useAsProjectInputScheme)
        g_hooks.useAsProjectInputScheme(rel);
    ImGui::EndDisabled();
}

void InputSchemeEditor::draw(Engine& e) {
    (void)e;
    if (!loaded_) {
        ImGui::TextWrapped("This file could not be read: %s", loadError_.c_str());
        return;
    }

    if (ImGui::Button(ICON_SAVE " Save") ||
        (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
         editor::keybinds().pressed(editor::CommandId::AssetSave, ImGui::GetIO()))) {
        std::string why;
        if (!save(&why)) AVER_ERROR("[InputSchemeEditor] save failed for '{}': {}", path_, why);
    }
    // Ctrl+Z / Ctrl+Y, skipped while an InputText has focus -- FoliageTypeEditor::draw()'s own
    // comment states the reason (WantTextInput is how GraphEditor's canvas tells the two apart).
    {
        const ImGuiIO& io = ImGui::GetIO();
        const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (focused && !io.WantTextInput) {
            if (canUndo() && editor::keybinds().pressed(editor::CommandId::EditUndo, io)) undo();
            if (canRedo() && editor::keybinds().pressed(editor::CommandId::EditRedo, io)) redo();
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!canUndo());
    if (ImGui::Button(ICON_UNDO " Undo")) undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!canRedo());
    if (ImGui::Button(ICON_REDO " Redo")) redo();
    ImGui::EndDisabled();

    ImGui::Separator();
    drawContext();
    ImGui::Dummy(ImVec2(0, 6.0f));
    drawActions();
    ImGui::Dummy(ImVec2(0, 6.0f));
    drawBindings();
    drawProjectSchemeLine();
}

#else   // AVER_WITH_IMGUI

// The headless build (and tests/editor's own target, which deliberately leaves AVER_WITH_IMGUI
// undefined) still gets load/save/dirty/undo/every field setter; only the window is absent --
// FoliageTypeEditor/BtEditor/SoundEditor's own #else branches do exactly this.
void InputSchemeEditor::draw(Engine& e) { (void)e; }

#endif  // AVER_WITH_IMGUI

std::unique_ptr<AssetEditor> makeInputSchemeEditor(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".ocinput") return nullptr;
    return std::make_unique<InputSchemeEditor>(path);
}

} // namespace aver::editor
