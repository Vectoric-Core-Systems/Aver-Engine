// The StateTree editor tab. See the header for the wiring.

#include "StateTreeEditor.hpp"
#include "EditorChrome.hpp"
#include "EditorKeybinds.hpp"

#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/synapse/StateTree.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <filesystem>

#if AVER_MODULE_SYNAPSE_SCENE && AVER_MODULE_SCENE
#  include "aver/scene/World.hpp"
#  include "aver/synapse/SynapseBt.hpp"
#  define AVER_STE_LIVE 1
#endif

#if AVER_WITH_IMGUI
#  include "imgui.h"
#endif

namespace aver::editor {

using synapse::BbKeyDef;
using synapse::BbOp;
using synapse::BbScope;
using synapse::BbType;
using synapse::BbValue;
using synapse::StateTreeAsset;
using synapse::StCond;
using synapse::StState;
using synapse::StTargetKind;
using synapse::StTransition;
using synapse::StTrigger;

namespace {

bool inRange(const StateTreeAsset& a, i32 i) { return i >= 0 && static_cast<usize>(i) < a.states.size(); }

std::string fmtG(f32 v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", static_cast<double>(v));
    return buf;
}

bool validLeafName(const std::string& s) {
    if (s.empty() || !std::isalpha(static_cast<unsigned char>(s[0]))) return false;
    for (const char c : s)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    return true;
}

bool validKeyName(const std::string& s) {
    if (s.empty() || std::isdigit(static_cast<unsigned char>(s[0]))) return false;
    for (const char c : s)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    return true;
}

f32 stateHeight(const StState& s) {
    constexpr f32 headerH = 24.0f, lineH = 16.0f, padY = 8.0f;
    f32 h = headerH + padY + lineH * static_cast<f32>(s.tasks.size() + s.transitions.size());
    if (!s.comment.empty()) h += lineH;
    return h;
}

void renameKeyInConds(std::vector<StCond>& list, const std::string& from, const std::string& to) {
    for (StCond& c : list)
        if (c.isBlackboard && c.bb.key == from) c.bb.key = to;
}

} // namespace

// ---- text -----------------------------------------------------------------------------------------

std::string stTaskText(const fmt::OcBtNode& n) {
    std::string s = n.name;
    for (int i = 0; i < 4; ++i)
        if (n.params[i] != 0.0f) s += " p" + std::to_string(i) + "=" + fmtG(n.params[i]);
    if (!n.stringParam.empty()) s += " \"" + n.stringParam + "\"";
    return s;
}

std::string stTransitionText(const StateTreeAsset& a, const StTransition& t) {
    std::string s;
    switch (t.trigger) {
        case StTrigger::Tick: s = "tick"; break;
        case StTrigger::Completed: s = "completed"; break;
        case StTrigger::Failed: s = "failed"; break;
        case StTrigger::Event: s = "event:" + t.event; break;
    }
    s += " -> ";
    switch (t.target) {
        case StTargetKind::Next: s += "NEXT"; break;
        case StTargetKind::Succeeded: s += "SUCCEEDED"; break;
        case StTargetKind::Failed: s += "FAILED"; break;
        case StTargetKind::Goto: s += t.targetState >= 0 ? a.statePath(t.targetState) : t.targetPath + " (?)"; break;
    }
    if (!t.conds.empty()) s += "  [" + std::to_string(t.conds.size()) + " if]";
    return s;
}

bool stateTreeWriteStarter(const std::string& path, std::string* why) {
    return synapse::saveStateTree(path, synapse::stStarter(), why);
}

// ---- the tab: load, save, undo ---------------------------------------------------------------------

StateTreeEditor::StateTreeEditor(std::string path) : path_(std::move(path)) { loadFromDisk(); }

StateTreeEditor::~StateTreeEditor() {
#if AVER_STE_LIVE
    if (debugEntity_ != 0 && synapse::btSystem().watched() == debugEntity_)
        synapse::btSystem().watch(scene::kInvalidEntity);
#endif
}

void StateTreeEditor::loadFromDisk() {
    StateTreeAsset a;
    std::string why;
    if (!synapse::loadStateTree(path_, a, &why)) {
        loaded_ = false;
        loadError_ = why;
        return;
    }
    asset_ = std::move(a);
    loaded_ = true;
    loadError_.clear();
    dirty_ = false;
    selected_ = asset_.states.empty() ? -1 : 0;
    history_.clear();
}

std::string StateTreeEditor::title() const {
    return std::filesystem::path(path_).filename().string() + "###sted:" + path_;
}

std::vector<std::string> StateTreeEditor::warnings() const {
#if AVER_STE_LIVE
    return synapse::validateStateTree(asset_, &synapse::btSystem().registry());
#else
    return synapse::validateStateTree(asset_, nullptr);
#endif
}

bool StateTreeEditor::save(std::string* why) {
    if (!loaded_) {
        if (why) *why = "cannot save: file failed to load (" + loadError_ + ")";
        return false;
    }
    // The file must read back: a name or an expression the parser refuses would make the tab unopenable.
    StateTreeAsset check;
    std::string parseWhy;
    if (!synapse::parseStateTree(synapse::writeStateTree(asset_), path_, check, &parseWhy)) {
        if (why) *why = "the tree would not read back: " + parseWhy;
        return false;
    }
    if (!synapse::saveStateTree(path_, asset_, why)) return false;
    dirty_ = false;
#if AVER_STE_LIVE
    synapse::btSystem().reloadTree(path_);   // running entities restart on the new tree
    if (const std::string rel = aiRelativeAssetPath(path_); !rel.empty())
        synapse::btSystem().reloadTree(rel);   // brains assigned by content path hash that spelling
#endif
    return true;
}

void StateTreeEditor::onFileChanged() {
    if (dirty_) {
        AVER_WARN("[StateTreeEditor] '{}' changed on disk, but this tab has unsaved edits -- keeping them", path_);
        return;
    }
    loadFromDisk();
}

void StateTreeEditor::undo() {
    if (!history_.undo(asset_)) return;
    dirty_ = true;
    if (!inRange(asset_, selected_)) selected_ = asset_.states.empty() ? -1 : 0;
}

void StateTreeEditor::redo() {
    if (!history_.redo(asset_)) return;
    dirty_ = true;
    if (!inRange(asset_, selected_)) selected_ = asset_.states.empty() ? -1 : 0;
}

StState* StateTreeEditor::sel() { return inRange(asset_, selected_) ? &asset_.states[static_cast<usize>(selected_)] : nullptr; }

bool StateTreeEditor::beginEdit() {
    if (!loaded_ || !sel()) return false;
    pushUndo();
    return true;
}

// ---- state edits ------------------------------------------------------------------------------------

bool StateTreeEditor::addState(bool asChild) {
    if (!loaded_) return false;
    i32 parent = -1;
    if (inRange(asset_, selected_)) parent = asChild ? selected_ : asset_.states[static_cast<usize>(selected_)].parent;
    pushUndo();
    const i32 added = synapse::stAddState(asset_, parent, std::string());
    if (added < 0) { history_.cancelPush(); return false; }
    selected_ = added;
    dirty_ = true;
    return true;
}

bool StateTreeEditor::deleteSelected() {
    if (!loaded_ || !inRange(asset_, selected_)) return false;
    pushUndo();
    i32 retargeted = 0;
    if (!synapse::stDeleteState(asset_, selected_, &retargeted)) { history_.cancelPush(); return false; }
    lastRetargeted_ = retargeted;
    selected_ = asset_.states.empty() ? -1 : std::min<i32>(selected_, static_cast<i32>(asset_.states.size()) - 1);
    dirty_ = true;
    return true;
}

bool StateTreeEditor::renameSelected(const std::string& name) {
    if (!loaded_ || !inRange(asset_, selected_) || asset_.states[static_cast<usize>(selected_)].name == name) return false;
    pushUndo();
    if (!synapse::stRenameState(asset_, selected_, name)) { history_.cancelPush(); return false; }
    dirty_ = true;
    return true;
}

bool StateTreeEditor::moveSelected(i32 delta) {
    if (!loaded_ || !inRange(asset_, selected_)) return false;
    pushUndo();
    const i32 moved = synapse::stMoveState(asset_, selected_, delta);
    if (moved < 0) { history_.cancelPush(); return false; }
    selected_ = moved;
    dirty_ = true;
    return true;
}

bool StateTreeEditor::reparentSelected(i32 newParent) {
    if (!loaded_ || !inRange(asset_, selected_)) return false;
    pushUndo();
    const i32 moved = synapse::stReparent(asset_, selected_, newParent);
    if (moved < 0) { history_.cancelPush(); return false; }
    selected_ = moved;
    dirty_ = true;
    return true;
}

bool StateTreeEditor::setGroup(bool on) {
    StState* s = sel();
    if (!loaded_ || !s || s->group == on) return false;
    pushUndo();
    sel()->group = on;
    endEdit();
    return true;
}

bool StateTreeEditor::setDisabled(bool on) {
    StState* s = sel();
    if (!loaded_ || !s || s->disabled == on) return false;
    pushUndo();
    sel()->disabled = on;
    endEdit();
    return true;
}

bool StateTreeEditor::setComplete(int mode) {
    StState* s = sel();
    if (!loaded_ || !s || mode < 0 || mode > 3) return false;
    const bool explicitNow = mode != 0;
    const synapse::StComplete value = mode == 1 ? synapse::StComplete::All : mode == 2 ? synapse::StComplete::Any : synapse::StComplete::None;
    if (s->completeExplicit == explicitNow && (!explicitNow || s->complete == value)) return false;
    pushUndo();
    s = sel();
    s->completeExplicit = explicitNow;
    if (explicitNow) s->complete = value;
    endEdit();
    return true;
}

bool StateTreeEditor::setComment(const std::string& text) {
    StState* s = sel();
    if (!loaded_ || !s || s->comment == text) return false;
    pushUndo();
    sel()->comment = text;
    endEdit();
    return true;
}

// ---- enter conditions, tasks, transitions -----------------------------------------------------------

static StCond defaultCond(const StateTreeAsset& a, bool blackboard, bool& ok) {
    StCond c;
    ok = true;
    if (blackboard) {
        c.isBlackboard = true;
        if (a.schema.keys.empty()) { ok = false; return c; }   // nothing to test: declare a key first
        const BbKeyDef& k = a.schema.keys.front();
        c.bb.key = k.name;
        c.bb.value = synapse::bbDefault(k.type);
        c.bb.op = k.type == BbType::Bool ? BbOp::IsSet : BbOp::Eq;
    } else {
        c.isBlackboard = false;
        c.leaf.kind = fmt::OcBtNodeKind::Condition;
        c.leaf.name = "HasTarget";
    }
    return c;
}

bool StateTreeEditor::addEnter(bool blackboard) {
    StState* s = sel();
    bool ok = false;
    const StCond c = defaultCond(asset_, blackboard, ok);
    if (!loaded_ || !s || !ok) return false;
    pushUndo();
    sel()->enter.push_back(c);
    endEdit();
    return true;
}

bool StateTreeEditor::removeEnter(usize index) {
    StState* s = sel();
    if (!loaded_ || !s || index >= s->enter.size()) return false;
    pushUndo();
    sel()->enter.erase(sel()->enter.begin() + static_cast<isize>(index));
    endEdit();
    return true;
}

bool StateTreeEditor::setEnter(usize index, const StCond& c) {
    StState* s = sel();
    if (!loaded_ || !s || index >= s->enter.size()) return false;
    pushUndo();
    sel()->enter[index] = c;
    endEdit();
    return true;
}

bool StateTreeEditor::addTask(const std::string& leaf) {
    StState* s = sel();
    if (!loaded_ || !s || s->tasks.size() >= synapse::kStMaxTasks) return false;
    pushUndo();
    fmt::OcBtNode n;
    n.kind = fmt::OcBtNodeKind::Action;
    n.name = validLeafName(leaf) ? leaf : "Wait";
    if (n.name == "Wait") n.params[0] = 1.0f;
    sel()->tasks.push_back(std::move(n));
    endEdit();
    return true;
}

bool StateTreeEditor::removeTask(usize index) {
    StState* s = sel();
    if (!loaded_ || !s || index >= s->tasks.size()) return false;
    pushUndo();
    sel()->tasks.erase(sel()->tasks.begin() + static_cast<isize>(index));
    endEdit();
    return true;
}

bool StateTreeEditor::setTask(usize index, const fmt::OcBtNode& n) {
    StState* s = sel();
    if (!loaded_ || !s || index >= s->tasks.size() || !validLeafName(n.name)) return false;
    pushUndo();
    sel()->tasks[index] = n;
    sel()->tasks[index].kind = fmt::OcBtNodeKind::Action;
    endEdit();
    return true;
}

bool StateTreeEditor::moveTask(usize index, i32 delta) {
    StState* s = sel();
    if (!loaded_ || !s || index >= s->tasks.size()) return false;
    const i32 to = static_cast<i32>(index) + delta;
    if (to < 0 || static_cast<usize>(to) >= s->tasks.size() || delta == 0) return false;
    pushUndo();
    std::swap(sel()->tasks[index], sel()->tasks[static_cast<usize>(to)]);
    endEdit();
    return true;
}

bool StateTreeEditor::addTransition() {
    StState* s = sel();
    if (!loaded_ || !s || s->transitions.size() >= synapse::kStMaxTransitions) return false;
    pushUndo();
    sel()->transitions.emplace_back();
    endEdit();
    return true;
}

bool StateTreeEditor::removeTransition(usize index) {
    StState* s = sel();
    if (!loaded_ || !s || index >= s->transitions.size()) return false;
    pushUndo();
    sel()->transitions.erase(sel()->transitions.begin() + static_cast<isize>(index));
    endEdit();
    return true;
}

bool StateTreeEditor::moveTransition(usize index, i32 delta) {
    StState* s = sel();
    if (!loaded_ || !s || index >= s->transitions.size()) return false;
    const i32 to = static_cast<i32>(index) + delta;
    if (to < 0 || static_cast<usize>(to) >= s->transitions.size() || delta == 0) return false;
    pushUndo();
    std::swap(sel()->transitions[index], sel()->transitions[static_cast<usize>(to)]);
    endEdit();
    return true;
}

bool StateTreeEditor::setTransitionTrigger(usize index, StTrigger trigger, const std::string& event) {
    StState* s = sel();
    if (!loaded_ || !s || index >= s->transitions.size()) return false;
    if (trigger == StTrigger::Event && !validKeyName(event)) return false;
    pushUndo();
    StTransition& t = sel()->transitions[index];
    t.trigger = trigger;
    t.event = trigger == StTrigger::Event ? event : std::string();
    endEdit();
    return true;
}

bool StateTreeEditor::setTransitionTarget(usize index, StTargetKind kind, i32 state) {
    StState* s = sel();
    if (!loaded_ || !s || index >= s->transitions.size()) return false;
    if (kind == StTargetKind::Goto && !inRange(asset_, state)) return false;
    pushUndo();
    StTransition& t = sel()->transitions[index];
    t.target = kind;
    t.targetState = kind == StTargetKind::Goto ? state : -1;
    t.targetPath = kind == StTargetKind::Goto ? asset_.statePath(state) : std::string();
    endEdit();
    return true;
}

bool StateTreeEditor::addTransitionCond(usize transition, bool blackboard) {
    StState* s = sel();
    bool ok = false;
    const StCond c = defaultCond(asset_, blackboard, ok);
    if (!loaded_ || !s || transition >= s->transitions.size() || !ok) return false;
    pushUndo();
    sel()->transitions[transition].conds.push_back(c);
    endEdit();
    return true;
}

bool StateTreeEditor::removeTransitionCond(usize transition, usize cond) {
    StState* s = sel();
    if (!loaded_ || !s || transition >= s->transitions.size() || cond >= s->transitions[transition].conds.size()) return false;
    pushUndo();
    auto& list = sel()->transitions[transition].conds;
    list.erase(list.begin() + static_cast<isize>(cond));
    endEdit();
    return true;
}

// ---- evaluators, header, schema -----------------------------------------------------------------------

bool StateTreeEditor::addEvaluator(const std::string& leaf) {
    if (!loaded_ || asset_.evaluators.size() >= synapse::kStMaxTasks || !validLeafName(leaf)) return false;
    pushUndo();
    fmt::OcBtNode n;
    n.kind = fmt::OcBtNodeKind::Action;
    n.name = leaf;
    asset_.evaluators.push_back(std::move(n));
    dirty_ = true;
    return true;
}

bool StateTreeEditor::removeEvaluator(usize index) {
    if (!loaded_ || index >= asset_.evaluators.size()) return false;
    pushUndo();
    asset_.evaluators.erase(asset_.evaluators.begin() + static_cast<isize>(index));
    dirty_ = true;
    return true;
}

bool StateTreeEditor::setDescription(const std::string& text) {
    if (!loaded_ || asset_.desc == text) return false;
    pushUndo();
    asset_.desc = text;
    dirty_ = true;
    return true;
}

bool StateTreeEditor::setRestartOnFinish(bool on) {
    if (!loaded_ || asset_.restartOnFinish == on) return false;
    pushUndo();
    asset_.restartOnFinish = on;
    dirty_ = true;
    return true;
}

bool StateTreeEditor::addKey(const BbKeyDef& def) {
    if (!loaded_ || !validKeyName(def.name)) return false;
    pushUndo();
    if (!asset_.schema.add(def)) { history_.cancelPush(); return false; }
    dirty_ = true;
    return true;
}

bool StateTreeEditor::removeKey(const std::string& name) {
    if (!loaded_ || asset_.schema.indexOf(name) < 0) return false;
    pushUndo();
    asset_.schema.remove(name);
    dirty_ = true;
    return true;
}

bool StateTreeEditor::renameKey(const std::string& from, const std::string& to) {
    if (!loaded_ || from == to || !validKeyName(to)) return false;
    const i32 idx = asset_.schema.indexOf(from);
    if (idx < 0 || asset_.schema.indexOf(to) >= 0) return false;
    pushUndo();
    asset_.schema.keys[static_cast<usize>(idx)].name = to;
    for (StState& s : asset_.states) {
        renameKeyInConds(s.enter, from, to);
        for (StTransition& t : s.transitions) renameKeyInConds(t.conds, from, to);
    }
    dirty_ = true;
    return true;
}

bool StateTreeEditor::setKey(const std::string& name, const BbKeyDef& def) {
    if (!loaded_) return false;
    const i32 idx = asset_.schema.indexOf(name);
    if (idx < 0) return false;
    pushUndo();
    if (def.name != name) {
        history_.cancelPush();
        if (!renameKey(name, def.name)) return false;
        pushUndo();   // the rename took its own snapshot; this one covers the field edits below
    }
    BbKeyDef& k = asset_.schema.keys[static_cast<usize>(asset_.schema.indexOf(def.name))];
    k.type = def.type;
    k.scope = def.scope;
    k.description = def.description;
    BbValue c;
    k.defaultValue = synapse::bbCoerce(def.type, def.defaultValue, c) ? c : synapse::bbDefault(def.type);
    dirty_ = true;
    return true;
}

std::unique_ptr<AssetEditor> makeStateTreeEditor(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".ocst") return nullptr;
    return std::make_unique<StateTreeEditor>(path);
}

// ================================================================================= drawing ==========

#if AVER_WITH_IMGUI

namespace {

constexpr f32 kDefaultCanvasFraction = 0.62f;
constexpr const char* kPrefSplit = "stateTreeEditor.split";

const char* const kTriggerNames[] = {"tick", "completed", "failed", "event"};

ImU32 stateColor(const StState& s) {
    if (s.disabled) return IM_COL32(95, 95, 100, 255);
    if (s.group) return IM_COL32(130, 85, 165, 255);
    return IM_COL32(60, 105, 165, 255);
}

ImU32 taskStatusColor(u8 s) {
    switch (s) {
        case 0: return IM_COL32(245, 175, 40, 255);
        case 1: return IM_COL32(80, 205, 110, 255);
        case 2: return IM_COL32(225, 80, 80, 255);
        default: return IM_COL32(170, 175, 185, 255);
    }
}

struct StLive {
    bool valid = false;
    bool sameTree = false;
    const synapse::StateTreeInstance* inst = nullptr;
    const synapse::Blackboard* board = nullptr;
};

StLive stLive(u32 entity, usize stateCount) {
    StLive r;
#if AVER_STE_LIVE
    if (entity == 0) return r;
    synapse::StateTreeDebugView v;
    if (!synapse::btSystem().stateTreeDebugView(static_cast<scene::Entity>(entity), v)) return r;
    r.valid = true;
    r.sameTree = v.tree && v.tree->asset.states.size() == stateCount;
    r.inst = v.state;
    r.board = v.board;
#else
    (void)entity; (void)stateCount;
#endif
    return r;
}

// A dashed cubic: every other segment of a 28-step sampling.
void dashedBezier(ImDrawList* dl, ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d, ImU32 col, f32 thick) {
    constexpr int kSteps = 28;
    ImVec2 prev = a;
    for (int i = 1; i <= kSteps; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(kSteps), u = 1.0f - t;
        const f32 w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
        const ImVec2 p(w0 * a.x + w1 * b.x + w2 * c.x + w3 * d.x, w0 * a.y + w1 * b.y + w2 * c.y + w3 * d.y);
        if (i % 2 == 1) dl->AddLine(prev, p, col, thick);
        prev = p;
    }
}

} // namespace

void StateTreeEditor::drawToolbar() {
    if (ImGui::Button("Save") || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                                  keybinds().pressed(CommandId::AssetSave, ImGui::GetIO()))) {
        std::string why;
        if (!save(&why)) AVER_ERROR("[StateTreeEditor] save failed for '{}': {}", path_, why);
    }
    {
        const ImGuiIO& io = ImGui::GetIO();
        const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (focused && !io.WantTextInput) {
            if (canUndo() && keybinds().pressed(CommandId::EditUndo, io)) undo();
            if (canRedo() && keybinds().pressed(CommandId::EditRedo, io)) redo();
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!canUndo());
    if (ImGui::Button("Undo")) undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!canRedo());
    if (ImGui::Button("Redo")) redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!inRange(asset_, selected_) && !asset_.states.empty());
    if (ImGui::Button("Add child")) addState(true);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Add sibling")) addState(false);
    ImGui::SameLine();
    ImGui::BeginDisabled(!inRange(asset_, selected_));
    if (ImGui::Button("Delete")) deleteSelected();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Reset view")) view_.reset();

    const std::vector<std::string> w = warnings();
    if (!w.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.2f, 1.0f), "%zu warning%s", w.size(), w.size() == 1 ? "" : "s");
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            for (const std::string& s : w) ImGui::TextUnformatted(s.c_str());
            ImGui::EndTooltip();
        }
    }
    if (lastRetargeted_ > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("(last delete retargeted %d transition%s to NEXT)", lastRetargeted_, lastRetargeted_ == 1 ? "" : "s");
    }
}

void StateTreeEditor::drawCanvas(f32 width) {
    if (!ImGui::BeginChild("##stcanvas", ImVec2(width, 0), ImGuiChildFlags_Borders,
                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::EndChild();
        return;
    }
    const f32 dpi = fontScale();
    TreeLayoutParams lp;
    lp.nodeW = 200.0f * dpi; lp.gapX = 36.0f * dpi; lp.gapY = 70.0f * dpi;
    const f32 headerH = 24.0f * dpi, lineH = 16.0f * dpi;
    std::vector<i32> parents;
    std::vector<f32> heights;
    for (const StState& s : asset_.states) {
        parents.push_back(s.parent);
        heights.push_back(stateHeight(s) * dpi);
    }
    const std::vector<TreeBox> boxes = layoutTree(parents, heights, lp);
    const StLive live = stLive(debugEntity_, asset_.states.size());
    std::vector<char> active(asset_.states.size(), 0);
    if (live.sameTree && live.inst)
        for (const i32 a : live.inst->active)
            if (inRange(asset_, a)) active[static_cast<usize>(a)] = 1;

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##stsurface", avail,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();
    const auto toScreen = [&](f32 x, f32 y) { return ImVec2(origin.x + view_.panX + x * view_.zoom, origin.y + view_.panY + y * view_.zoom); };
    const f32 gx = (io.MousePos.x - origin.x - view_.panX) / view_.zoom;
    const f32 gy = (io.MousePos.y - origin.y - view_.panY) / view_.zoom;
    const i32 hit = hovered ? treeHitTest(boxes, gx, gy) : -1;
    bool clickedEmpty = false;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hit >= 0) selected_ = hit; else clickedEmpty = true;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        if (hit >= 0) selected_ = hit;
        ImGui::OpenPopup("##stctx");
    }
    canvasInput(view_, origin.x, origin.y, hovered, clickedEmpty);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), true);
    ImFont* font = ImGui::GetFont();
    const f32 fontSize = ImGui::GetFontSize() * view_.zoom;

    // parent -> child edges
    for (usize i = 0; i < asset_.states.size(); ++i) {
        const i32 p = asset_.states[i].parent;
        if (p < 0) continue;
        const TreeBox& pb = boxes[static_cast<usize>(p)];
        const TreeBox& cb = boxes[i];
        const ImVec2 a = toScreen(pb.x + pb.w * 0.5f, pb.y + pb.h);
        const ImVec2 b = toScreen(cb.x + cb.w * 0.5f, cb.y);
        const f32 k = (b.y - a.y) * 0.5f;
        dl->AddBezierCubic(a, ImVec2(a.x, a.y + k), ImVec2(b.x, b.y - k), b, IM_COL32(150, 155, 165, 220), 1.5f * view_.zoom);
    }
    // Goto transitions, dashed, labelled with the trigger
    for (usize i = 0; i < asset_.states.size(); ++i) {
        for (const StTransition& t : asset_.states[i].transitions) {
            if (t.target != StTargetKind::Goto || t.targetState < 0 || static_cast<usize>(t.targetState) == i) continue;
            const TreeBox& sb = boxes[i];
            const TreeBox& tb = boxes[static_cast<usize>(t.targetState)];
            const bool right = tb.x + tb.w * 0.5f >= sb.x + sb.w * 0.5f;
            const ImVec2 a = toScreen(right ? sb.x + sb.w : sb.x, sb.y + sb.h * 0.5f);
            const ImVec2 b = toScreen(right ? tb.x : tb.x + tb.w, tb.y + tb.h * 0.5f);
            const f32 k = std::max(40.0f * view_.zoom, std::fabs(b.x - a.x) * 0.4f) * (right ? 1.0f : -1.0f);
            const ImU32 col = static_cast<i32>(i) == selected_ ? IM_COL32(255, 210, 90, 230) : IM_COL32(120, 190, 235, 170);
            dashedBezier(dl, a, ImVec2(a.x + k, a.y), ImVec2(b.x - k, b.y), b, col, 1.5f * view_.zoom);
            dl->AddTriangleFilled(b, ImVec2(b.x - (right ? 7.0f : -7.0f) * view_.zoom, b.y - 4.0f * view_.zoom),
                                  ImVec2(b.x - (right ? 7.0f : -7.0f) * view_.zoom, b.y + 4.0f * view_.zoom), col);
            const ImVec2 mid((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f - fontSize);
            const char* label = kTriggerNames[static_cast<int>(t.trigger)];
            dl->AddText(font, fontSize * 0.8f, mid, IM_COL32(150, 205, 240, 230), label);
        }
    }
    // states
    for (usize i = 0; i < asset_.states.size(); ++i) {
        const StState& s = asset_.states[i];
        const TreeBox& b = boxes[i];
        const ImVec2 p0 = toScreen(b.x, b.y);
        const ImVec2 p1 = toScreen(b.x + b.w, b.y + b.h);
        const f32 round = 5.0f * view_.zoom;
        const f32 hh = headerH * view_.zoom;
        dl->AddRectFilled(p0, p1, IM_COL32(32, 35, 42, 245), round);
        dl->AddRectFilled(p0, ImVec2(p1.x, p0.y + hh), stateColor(s), round, ImDrawFlags_RoundCornersTop);
        dl->PushClipRect(p0, p1, true);
        std::string title = s.name;
        if (s.group) title += "  [group]";
        if (s.disabled) title += "  [off]";
        dl->AddText(font, fontSize, ImVec2(p0.x + 6.0f * view_.zoom, p0.y + (hh - fontSize) * 0.5f), IM_COL32(245, 245, 250, 255), title.c_str());
        f32 ty = p0.y + hh + 3.0f * view_.zoom;
        const std::vector<u8>* tr = (live.sameTree && live.inst && i < live.inst->traceTasks.size()) ? &live.inst->traceTasks[i] : nullptr;
        for (usize ti = 0; ti < s.tasks.size(); ++ti) {
            ImU32 col = IM_COL32(205, 210, 220, 255);
            if (tr && ti < tr->size() && active[i]) col = taskStatusColor((*tr)[ti]);
            dl->AddText(font, fontSize * 0.9f, ImVec2(p0.x + 8.0f * view_.zoom, ty), col, ("- " + stTaskText(s.tasks[ti])).c_str());
            ty += lineH * view_.zoom;
        }
        for (const StTransition& t : s.transitions) {
            dl->AddText(font, fontSize * 0.9f, ImVec2(p0.x + 8.0f * view_.zoom, ty), IM_COL32(150, 205, 240, 255), ("> " + stTransitionText(asset_, t)).c_str());
            ty += lineH * view_.zoom;
        }
        if (!s.comment.empty())
            dl->AddText(font, fontSize * 0.9f, ImVec2(p0.x + 8.0f * view_.zoom, ty), IM_COL32(150, 160, 175, 255), s.comment.c_str());
        dl->PopClipRect();
        if (active[i]) dl->AddRect(p0, p1, IM_COL32(245, 175, 40, 255), round, 0, 3.0f * view_.zoom);
        else dl->AddRect(p0, p1, IM_COL32(95, 100, 112, 255), round, 0, 1.0f * view_.zoom);
        if (static_cast<i32>(i) == selected_) dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 255), round, 0, 2.0f * view_.zoom);
    }
    dl->PopClipRect();

    if (ImGui::BeginPopup("##stctx")) {
        if (ImGui::MenuItem("Add child state")) addState(true);
        if (ImGui::MenuItem("Add sibling state")) addState(false);
        ImGui::Separator();
        if (ImGui::MenuItem("Move earlier", nullptr, false, inRange(asset_, selected_))) moveSelected(-1);
        if (ImGui::MenuItem("Move later", nullptr, false, inRange(asset_, selected_))) moveSelected(1);
        if (const StState* s = sel()) {
            if (ImGui::MenuItem("Group (a path, never a leaf)", nullptr, s->group)) setGroup(!s->group);
            if (ImGui::MenuItem("Disabled", nullptr, s->disabled)) setDisabled(!s->disabled);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete", nullptr, false, inRange(asset_, selected_))) deleteSelected();
        ImGui::EndPopup();
    }

    ImGui::SetCursorScreenPos(ImVec2(origin.x + 6.0f, origin.y + avail.y - ImGui::GetTextLineHeightWithSpacing()));
    ImGui::TextDisabled("Drag empty space to pan  |  Ctrl+wheel zooms  |  right-click a state for actions  |  dashed = transition");
    ImGui::EndChild();
}

bool StateTreeEditor::drawLeafRow(fmt::OcBtNode& n, const char* id, bool condition) {
    bool changed = false;
    ImGui::PushID(id);
    char nb[64];
    std::snprintf(nb, sizeof nb, "%s", n.name.c_str());
    ImGui::SetNextItemWidth(150.0f);
    ImGui::InputText("##leaf", nb, sizeof nb);
    if (ImGui::IsItemDeactivatedAfterEdit() && validLeafName(nb) && n.name != nb) {
        pushUndo();
        n.name = nb;
        changed = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f);
#if AVER_STE_LIVE
    if (ImGui::BeginCombo("##pick", "pick")) {
        const std::vector<std::string> names = condition ? synapse::btSystem().registry().conditionNames()
                                                         : synapse::btSystem().registry().actionNames();
        for (const std::string& nm : names) {
            if (ImGui::Selectable(nm.c_str(), nm == n.name) && nm != n.name) {
                pushUndo();
                n.name = nm;
                changed = true;
                break;
            }
        }
        ImGui::EndCombo();
    }
#else
    (void)condition;
    ImGui::TextDisabled("pick");
#endif
    for (int i = 0; i < 4; ++i) {
        char label[8];
        std::snprintf(label, sizeof label, "##p%d", i);
        f32 v = n.params[i];
        ImGui::SetNextItemWidth(52.0f);
        if (i > 0) ImGui::SameLine();
        ImGui::DragFloat(label, &v, 0.25f);
        if (ImGui::IsItemActivated()) pushUndo();
        if (v != n.params[i]) { n.params[i] = v; changed = true; }
    }
    char sb[160];
    std::snprintf(sb, sizeof sb, "%s", n.stringParam.c_str());
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##s", "string param", sb, sizeof sb);
    if (ImGui::IsItemDeactivatedAfterEdit() && n.stringParam != sb) {
        pushUndo();
        n.stringParam = sb;
        changed = true;
    }
    ImGui::PopID();
    if (changed) dirty_ = true;
    return changed;
}

bool StateTreeEditor::drawCondRow(StCond& c, const char* id) {
    bool changed = false;
    ImGui::PushID(id);
    if (ImGui::Checkbox("not", &c.negate)) { dirty_ = true; changed = true; }
    ImGui::SameLine();
    if (c.isBlackboard) {
        const BbKeyDef* def = asset_.schema.find(c.bb.key);
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::BeginCombo("##key", c.bb.key.empty() ? "(key)" : c.bb.key.c_str())) {
            for (const BbKeyDef& k : asset_.schema.keys) {
                if (ImGui::Selectable(k.name.c_str(), k.name == c.bb.key) && k.name != c.bb.key) {
                    pushUndo();
                    c.bb.key = k.name;
                    c.bb.value = synapse::bbDefault(k.type);
                    if (k.type == BbType::Vec3 && c.bb.op != BbOp::IsSet && c.bb.op != BbOp::NotSet && c.bb.op != BbOp::Eq && c.bb.op != BbOp::Ne) c.bb.op = BbOp::Eq;
                    dirty_ = true;
                    changed = true;
                    break;
                }
            }
            ImGui::EndCombo();
        }
        def = asset_.schema.find(c.bb.key);
        const BbType type = def ? def->type : c.bb.value.type;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::BeginCombo("##op", synapse::bbOpName(c.bb.op))) {
            for (u8 o = 0; o <= static_cast<u8>(BbOp::NotSet); ++o) {
                const BbOp op = static_cast<BbOp>(o);
                if (type == BbType::Vec3 && op != BbOp::IsSet && op != BbOp::NotSet && op != BbOp::Eq && op != BbOp::Ne) continue;
                if (ImGui::Selectable(synapse::bbOpName(op), op == c.bb.op) && op != c.bb.op) {
                    pushUndo();
                    c.bb.op = op;
                    dirty_ = true;
                    changed = true;
                    break;
                }
            }
            ImGui::EndCombo();
        }
        if (c.bb.op != BbOp::IsSet && c.bb.op != BbOp::NotSet) {
            BbValue v = c.bb.value;
            if (v.type != type) v = synapse::bbDefault(type);
            bool activated = false;
            ImGui::SetNextItemWidth(110.0f);
            ImGui::SameLine();
            if (bbValueEditor("##val", v, type, activated)) {
                if (activated) pushUndo();
                c.bb.value = v;
                dirty_ = true;
                changed = true;
            } else if (activated) {
                pushUndo();
            }
        }
    } else {
        ImGui::TextUnformatted("cond");
        ImGui::SameLine();
        if (drawLeafRow(c.leaf, "leaf", true)) changed = true;
    }
    ImGui::PopID();
    return changed;
}

void StateTreeEditor::drawStatePanel() {
    StState* s = sel();
    if (!s) { ImGui::TextUnformatted("Nothing selected"); return; }
    ImGui::PushID(selected_);

    char nb[64];
    std::snprintf(nb, sizeof nb, "%s", s->name.c_str());
    ImGui::InputText("Name", nb, sizeof nb);
    if (ImGui::IsItemDeactivatedAfterEdit()) renameSelected(nb);
    ImGui::TextDisabled("%s", asset_.statePath(selected_).c_str());

    bool group = s->group, disabled = s->disabled;
    if (ImGui::Checkbox("Group", &group)) setGroup(group);
    ImGui::SameLine();
    if (ImGui::Checkbox("Disabled", &disabled)) setDisabled(disabled);
    s = sel();
    const char* const completeNames[] = {"Default (all with tasks, else none)", "all", "any", "none"};
    const int mode = !s->completeExplicit ? 0 : s->complete == synapse::StComplete::All ? 1 : s->complete == synapse::StComplete::Any ? 2 : 3;
    if (ImGui::BeginCombo("Complete when", completeNames[mode])) {
        for (int m = 0; m < 4; ++m)
            if (ImGui::Selectable(completeNames[m], m == mode)) { setComplete(m); break; }
        ImGui::EndCombo();
    }
    char cb[160];
    s = sel();
    std::snprintf(cb, sizeof cb, "%s", s->comment.c_str());
    ImGui::InputText("Comment", cb, sizeof cb);
    if (ImGui::IsItemDeactivatedAfterEdit()) setComment(cb);

    // The parent, as a combo that cannot pick the state itself or a descendant.
    s = sel();
    if (ImGui::BeginCombo("Parent", s->parent < 0 ? "(root)" : asset_.statePath(s->parent).c_str())) {
        if (ImGui::Selectable("(root)", s->parent < 0)) reparentSelected(-1);
        for (i32 i = 0; i < static_cast<i32>(asset_.states.size()); ++i) {
            bool descendant = false;
            for (i32 w = i; w >= 0; w = asset_.states[static_cast<usize>(w)].parent)
                if (w == selected_) { descendant = true; break; }
            if (descendant) continue;
            if (ImGui::Selectable(asset_.statePath(i).c_str(), i == s->parent)) { reparentSelected(i); break; }
        }
        ImGui::EndCombo();
    }
    if (ImGui::Button("Move earlier")) moveSelected(-1);
    ImGui::SameLine();
    if (ImGui::Button("Move later")) moveSelected(1);
    ImGui::SameLine();
    ImGui::TextDisabled("(order is priority)");

    // ---- enter conditions
    ImGui::Separator();
    ImGui::TextUnformatted("Enter conditions (all must pass)");
    s = sel();
    {
        int removeAt = -1;
        for (usize i = 0; i < s->enter.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            drawCondRow(s->enter[i], "cond");
            if (ImGui::SmallButton("remove")) removeAt = static_cast<int>(i);
            ImGui::PopID();
            ImGui::Separator();
        }
        if (removeAt >= 0) removeEnter(static_cast<usize>(removeAt));
        if (ImGui::SmallButton("+ blackboard test")) addEnter(true);
        ImGui::SameLine();
        if (ImGui::SmallButton("+ condition leaf")) addEnter(false);
        if (asset_.schema.keys.empty()) ImGui::TextDisabled("Declare keys in the Parameters tab to test them.");
    }

    // ---- tasks
    ImGui::Separator();
    ImGui::TextUnformatted("Tasks (run together while the state is active)");
    s = sel();
    {
        int removeAt = -1, moveAt = -1, moveDelta = 0;
        for (usize i = 0; i < s->tasks.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            drawLeafRow(s->tasks[i], "task", false);
            if (ImGui::SmallButton("up")) { moveAt = static_cast<int>(i); moveDelta = -1; }
            ImGui::SameLine();
            if (ImGui::SmallButton("down")) { moveAt = static_cast<int>(i); moveDelta = 1; }
            ImGui::SameLine();
            if (ImGui::SmallButton("remove")) removeAt = static_cast<int>(i);
            ImGui::PopID();
            ImGui::Separator();
        }
        if (removeAt >= 0) removeTask(static_cast<usize>(removeAt));
        else if (moveAt >= 0) moveTask(static_cast<usize>(moveAt), moveDelta);
        if (ImGui::SmallButton("+ task")) addTask("Wait");
    }

    // ---- transitions
    ImGui::Separator();
    ImGui::TextUnformatted("Transitions (first match wins; leaf states are scanned first)");
    s = sel();
    {
        int removeAt = -1, moveAt = -1, moveDelta = 0;
        for (usize ti = 0; ti < s->transitions.size(); ++ti) {
            StTransition& t = s->transitions[ti];
            ImGui::PushID(static_cast<int>(ti) + 1000);
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::BeginCombo("##trig", kTriggerNames[static_cast<int>(t.trigger)])) {
                for (int k = 0; k < 4; ++k)
                    if (ImGui::Selectable(kTriggerNames[k], k == static_cast<int>(t.trigger)))
                        { setTransitionTrigger(ti, static_cast<StTrigger>(k), t.event.empty() ? "Event" : t.event); break; }
                ImGui::EndCombo();
            }
            if (t.trigger == StTrigger::Event) {
                ImGui::SameLine();
                char eb[64];
                std::snprintf(eb, sizeof eb, "%s", t.event.c_str());
                ImGui::SetNextItemWidth(110.0f);
                ImGui::InputText("##event", eb, sizeof eb);
                if (ImGui::IsItemDeactivatedAfterEdit()) setTransitionTrigger(ti, StTrigger::Event, eb);
            }
            ImGui::SameLine();
            ImGui::TextUnformatted("->");
            ImGui::SameLine();
            const std::string current = t.target == StTargetKind::Goto ? (t.targetState >= 0 ? asset_.statePath(t.targetState) : t.targetPath + " (?)")
                                        : t.target == StTargetKind::Next ? "NEXT" : t.target == StTargetKind::Succeeded ? "SUCCEEDED" : "FAILED";
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##target", current.c_str())) {
                if (ImGui::Selectable("NEXT", t.target == StTargetKind::Next)) { setTransitionTarget(ti, StTargetKind::Next, -1); }
                if (ImGui::Selectable("SUCCEEDED", t.target == StTargetKind::Succeeded)) { setTransitionTarget(ti, StTargetKind::Succeeded, -1); }
                if (ImGui::Selectable("FAILED", t.target == StTargetKind::Failed)) { setTransitionTarget(ti, StTargetKind::Failed, -1); }
                ImGui::Separator();
                for (i32 i = 0; i < static_cast<i32>(asset_.states.size()); ++i)
                    if (ImGui::Selectable(asset_.statePath(i).c_str(), t.target == StTargetKind::Goto && t.targetState == i)) { setTransitionTarget(ti, StTargetKind::Goto, i); break; }
                ImGui::EndCombo();
            }
            s = sel();
            StTransition& t2 = s->transitions[ti];
            int removeCond = -1;
            for (usize ci = 0; ci < t2.conds.size(); ++ci) {
                ImGui::PushID(static_cast<int>(ci));
                ImGui::Indent();
                ImGui::TextUnformatted("if");
                ImGui::SameLine();
                drawCondRow(t2.conds[ci], "cond");
                if (ImGui::SmallButton("remove condition")) removeCond = static_cast<int>(ci);
                ImGui::Unindent();
                ImGui::PopID();
            }
            if (removeCond >= 0) removeTransitionCond(ti, static_cast<usize>(removeCond));
            if (ImGui::SmallButton("+ if blackboard")) addTransitionCond(ti, true);
            ImGui::SameLine();
            if (ImGui::SmallButton("+ if condition")) addTransitionCond(ti, false);
            ImGui::SameLine();
            if (ImGui::SmallButton("up")) { moveAt = static_cast<int>(ti); moveDelta = -1; }
            ImGui::SameLine();
            if (ImGui::SmallButton("down")) { moveAt = static_cast<int>(ti); moveDelta = 1; }
            ImGui::SameLine();
            if (ImGui::SmallButton("remove")) removeAt = static_cast<int>(ti);
            ImGui::PopID();
            ImGui::Separator();
            s = sel();
        }
        if (removeAt >= 0) removeTransition(static_cast<usize>(removeAt));
        else if (moveAt >= 0) moveTransition(static_cast<usize>(moveAt), moveDelta);
        if (ImGui::SmallButton("+ transition")) addTransition();
    }
    ImGui::PopID();
}

void StateTreeEditor::drawEvaluatorPanel() {
    char db[200];
    std::snprintf(db, sizeof db, "%s", asset_.desc.c_str());
    ImGui::InputText("Description", db, sizeof db);
    if (ImGui::IsItemDeactivatedAfterEdit()) setDescription(db);
    bool restart = asset_.restartOnFinish;
    if (ImGui::Checkbox("Restart when the tree finishes", &restart)) setRestartOnFinish(restart);
    ImGui::Separator();
    ImGui::TextWrapped("Evaluators run before the tasks, every update, in this order. Their status is ignored.");
    int removeAt = -1;
    for (usize i = 0; i < asset_.evaluators.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        drawLeafRow(asset_.evaluators[i], "eval", false);
        if (ImGui::SmallButton("remove")) removeAt = static_cast<int>(i);
        ImGui::PopID();
        ImGui::Separator();
    }
    if (removeAt >= 0) removeEvaluator(static_cast<usize>(removeAt));
    if (ImGui::SmallButton("+ evaluator")) addEvaluator("Wait");
}

void StateTreeEditor::drawDebugPanel() {
#if AVER_STE_LIVE
    std::vector<u64> ids{fnv1a64(path_)};
    if (const std::string rel = aiRelativeAssetPath(path_); !rel.empty()) ids.push_back(fnv1a64(rel));
    drawEntityPicker(debugEntity_, ids, "state tree");
    const StLive live = stLive(debugEntity_, asset_.states.size());
    if (debugEntity_ == 0) {
        ImGui::TextWrapped("Pick an entity while the game runs (Play) to see its active chain, running tasks and "
                           "the last transition here and on the canvas.");
        return;
    }
    if (!live.valid) { ImGui::TextDisabled("That entity has no brain right now."); return; }
    if (!live.sameTree)
        ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.2f, 1.0f), "This entity runs a different tree (or an older version of this file).");
    if (!live.inst) { ImGui::TextDisabled("It has not ticked yet."); return; }
    const char* st = live.inst->status == synapse::StStatus::Running ? "running" : live.inst->status == synapse::StStatus::Succeeded ? "succeeded"
                     : live.inst->status == synapse::StStatus::Failed ? "failed" : "stopped";
    ImGui::Text("Status: %s    Updates: %llu", st, static_cast<unsigned long long>(live.inst->tickCount));
    if (live.sameTree) {
        ImGui::TextUnformatted("Active chain");
        for (const i32 a : live.inst->active)
            if (ImGui::Selectable(asset_.statePath(a).c_str(), selected_ == a)) selected_ = a;
        if (live.inst->lastTo >= 0 || live.inst->lastFrom >= 0) {
            const std::string from = live.inst->lastFrom >= 0 ? asset_.statePath(live.inst->lastFrom) : std::string("(start)");
            const std::string to = live.inst->lastTo >= 0 ? asset_.statePath(live.inst->lastTo) : std::string("(finished)");
            ImGui::Text("Last transition: %s -> %s @ update %llu", from.c_str(), to.c_str(), static_cast<unsigned long long>(live.inst->lastTransitionTick));
        }
    }
    if (!live.inst->events.empty()) {
        ImGui::TextUnformatted("Pending events");
        for (const auto& e : live.inst->events) ImGui::BulletText("%s  (%lld, %g)", e.name.c_str(), static_cast<long long>(e.i), static_cast<double>(e.f));
    }
#else
    ImGui::TextWrapped("Live debugging needs a build with the AI scene module (Aver.Synapse.Scene).");
#endif
}

void StateTreeEditor::draw(Engine& e) {
    (void)e;
    if (!loaded_) {
        ImGui::TextWrapped("This file could not be read: %s", loadError_.c_str());
        return;
    }
    {
        const StLive live = stLive(debugEntity_, asset_.states.size());
        liveBoard_.update(live.board, ImGui::GetIO().DeltaTime);
    }
    drawToolbar();
    ImGui::Separator();

    const f32 dpi = fontScale();
    const f32 avail = ImGui::GetContentRegionAvail().x;
    const f32 minCanvas = 240.0f * dpi, minSide = 300.0f * dpi;
    const f32 canvasW = splitPaneWidth(split_, kPrefSplit, kDefaultCanvasFraction, avail, minCanvas, minSide);
    drawCanvas(canvasW);
    drawSplitHandle(split_, "##stsplit", kPrefSplit, avail, minCanvas, minSide, 6.0f * dpi);
    if (ImGui::BeginChild("##stside", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        if (ImGui::BeginTabBar("##sttabs")) {
            if (ImGui::BeginTabItem("State")) { drawStatePanel(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Parameters")) {
                const StLive live = stLive(debugEntity_, asset_.states.size());
                drawSchemaPanel(asset_.schema, live.board, liveBoard_, schemaState_, *this);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Evaluators")) { drawEvaluatorPanel(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Debug")) { drawDebugPanel(); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    }
    ImGui::EndChild();
}

void StateTreeEditor::resetLayout() {
    resetSplitPane(split_, kPrefSplit, kDefaultCanvasFraction);
    view_.reset();
}

#else   // AVER_WITH_IMGUI

void StateTreeEditor::draw(Engine& e) { (void)e; }
void StateTreeEditor::resetLayout() { view_.reset(); }

#endif  // AVER_WITH_IMGUI

} // namespace aver::editor
