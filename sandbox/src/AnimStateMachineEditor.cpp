// The .ocasm state-machine editor tab. See the header for the SandboxApp hook and for why the edits
// are free functions rather than members.

#include "AnimStateMachineEditor.hpp"
#include "EditorKeybinds.hpp"

#include "aver/anim/AnimGraphAsset.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <set>

#if AVER_WITH_IMGUI
#  include "imgui.h"
#endif

namespace aver::editor {

using anim::AnimStateMachineAsset;
using anim::AsmCondition;
using anim::AsmMachine;
using anim::AsmOp;
using anim::AsmParamType;
using anim::AsmState;
using anim::AsmStateKind;
using anim::AsmTransition;

namespace {

bool inStates(const AsmMachine& m, i32 s) { return s >= 0 && static_cast<usize>(s) < m.states.size(); }
bool inMachines(const AnimStateMachineAsset& a, i32 m) { return m >= 0 && static_cast<usize>(m) < a.machines.size(); }

void collectSubtree(const AnimStateMachineAsset& a, i32 root, std::vector<i32>& out) {
    if (!inMachines(a, root) || std::find(out.begin(), out.end(), root) != out.end()) return;
    out.push_back(root);
    for (const AsmState& s : a.machines[static_cast<usize>(root)].states)
        if (s.kind == AsmStateKind::SubMachine) collectSubtree(a, s.subMachine, out);
}

} // namespace

// ---------------------------------------------------------------------------- free edits ----------

AnimStateMachineAsset asmStarterMachine() {
    AnimStateMachineAsset a;
    a.name = "NewStateMachine";
    a.params = {{"speed", AsmParamType::Float, 0.0f}};
    AsmMachine m;
    m.name = "Root";
    AsmState idle, walk;
    idle.name = "Idle";
    idle.asset = "Content/Anim/Idle.ocanim";
    idle.posX = 40.0f;
    idle.posY = 40.0f;
    walk.name = "Walk";
    walk.asset = "Content/Anim/Walk.ocanim";
    walk.posX = 280.0f;
    walk.posY = 40.0f;
    m.states = {idle, walk};
    AsmTransition up, down;
    up.from = 0;
    up.to = 1;
    up.blendTime = 0.2f;
    up.conditions = {{"speed", AsmOp::Greater, 0.1f}};
    down.from = 1;
    down.to = 0;
    down.blendTime = 0.2f;
    down.conditions = {{"speed", AsmOp::LessEq, 0.1f}};
    m.transitions = {up, down};
    a.machines = {m};
    return a;
}

std::string asmUniqueStateName(const AsmMachine& m, const std::string& base) {
    auto taken = [&](const std::string& n) {
        for (const AsmState& s : m.states)
            if (s.name == n) return true;
        return false;
    };
    if (!taken(base)) return base;
    for (int i = 2;; ++i) {
        const std::string n = base + " " + std::to_string(i);
        if (!taken(n)) return n;
    }
}

i32 asmAddState(AsmMachine& m, AsmStateKind kind, const std::string& name, f32 x, f32 y) {
    AsmState s;
    s.name = asmUniqueStateName(m, name);
    s.kind = kind;
    if (x == 0.0f && y == 0.0f) {
        const f32 n = static_cast<f32>(m.states.size());
        x = 40.0f + std::fmod(n, 4.0f) * 220.0f;
        y = 40.0f + std::floor(n / 4.0f) * 90.0f;
    }
    s.posX = x;
    s.posY = y;
    m.states.push_back(std::move(s));
    return static_cast<i32>(m.states.size()) - 1;
}

i32 asmAddSubMachine(AnimStateMachineAsset& a, i32 machine, const std::string& name, f32 x, f32 y) {
    if (!inMachines(a, machine)) return -1;
    AsmMachine inner;
    inner.name = name;
    AsmState first;
    first.name = "State";
    first.posX = 40.0f;
    first.posY = 40.0f;
    inner.states = {first};
    a.machines.push_back(std::move(inner));
    const i32 innerIndex = static_cast<i32>(a.machines.size()) - 1;

    AsmMachine& m = a.machines[static_cast<usize>(machine)];
    const i32 idx = asmAddState(m, AsmStateKind::SubMachine, name, x, y);
    m.states[static_cast<usize>(idx)].subMachine = innerIndex;
    a.machines[static_cast<usize>(innerIndex)].name = m.states[static_cast<usize>(idx)].name;
    return idx;
}

bool asmRemoveState(AnimStateMachineAsset& a, i32& machine, i32 state) {
    if (!inMachines(a, machine)) return false;
    AsmMachine& m = a.machines[static_cast<usize>(machine)];
    if (!inStates(m, state) || m.states.size() <= 1) return false;

    std::vector<i32> drop;
    if (m.states[static_cast<usize>(state)].kind == AsmStateKind::SubMachine)
        collectSubtree(a, m.states[static_cast<usize>(state)].subMachine, drop);

    // Transitions: drop what touches the state, renumber what points past it.
    std::vector<AsmTransition> kept;
    for (AsmTransition t : m.transitions) {
        if (t.from == state || t.to == state) continue;
        if (t.from > state) --t.from;
        if (t.to > state) --t.to;
        kept.push_back(std::move(t));
    }
    m.transitions = std::move(kept);
    m.states.erase(m.states.begin() + state);
    if (m.entry == state) m.entry = 0;
    else if (m.entry > state) --m.entry;

    // Owned machines, highest first so each erase only shifts indices above it.
    std::sort(drop.begin(), drop.end(), std::greater<i32>());
    for (const i32 d : drop) {
        a.machines.erase(a.machines.begin() + d);
        for (AsmMachine& mm : a.machines)
            for (AsmState& s : mm.states)
                if (s.kind == AsmStateKind::SubMachine && s.subMachine > d) --s.subMachine;
        if (machine > d) --machine;
    }
    return true;
}

i32 asmParentMachine(const AnimStateMachineAsset& a, i32 machine) {
    for (usize mi = 0; mi < a.machines.size(); ++mi)
        for (const AsmState& s : a.machines[mi].states)
            if (s.kind == AsmStateKind::SubMachine && s.subMachine == machine) return static_cast<i32>(mi);
    return -1;
}

i32 asmAddTransition(const AnimStateMachineAsset& a, AsmMachine& m, i32 from, i32 to, bool isRoot) {
    const bool fromOk = from == anim::kAsmAny || inStates(m, from);
    const bool toOk = inStates(m, to) || (to == anim::kAsmExit && !isRoot);
    if (!fromOk || !toOk) return -1;
    AsmTransition t;
    t.from = from;
    t.to = to;
    if (from == anim::kAsmAny) {
        if (!a.params.empty()) t.conditions.push_back({a.params[0].name, asmDefaultOp(a.params[0].type), 0.0f});
    } else {
        t.hasExitTime = true;
        t.exitTime = 1.0f;
    }
    m.transitions.push_back(std::move(t));
    return static_cast<i32>(m.transitions.size()) - 1;
}

bool asmRemoveTransition(AsmMachine& m, i32 index) {
    if (index < 0 || static_cast<usize>(index) >= m.transitions.size()) return false;
    m.transitions.erase(m.transitions.begin() + index);
    return true;
}

i32 asmMoveTransition(AsmMachine& m, i32 index, i32 delta) {
    const i32 to = index + delta;
    if (index < 0 || to < 0 || static_cast<usize>(index) >= m.transitions.size() ||
        static_cast<usize>(to) >= m.transitions.size())
        return -1;
    std::swap(m.transitions[static_cast<usize>(index)], m.transitions[static_cast<usize>(to)]);
    return to;
}

i32 asmAddParam(AnimStateMachineAsset& a, const std::string& name, AsmParamType type, f32 def) {
    if (name.empty() || a.paramIndex(name) >= 0) return -1;
    a.params.push_back({name, type, def});
    return static_cast<i32>(a.params.size()) - 1;
}

bool asmRemoveParam(AnimStateMachineAsset& a, i32 index) {
    if (index < 0 || static_cast<usize>(index) >= a.params.size()) return false;
    const std::string name = a.params[static_cast<usize>(index)].name;
    a.params.erase(a.params.begin() + index);
    for (AsmMachine& m : a.machines) {
        for (AsmTransition& t : m.transitions)
            t.conditions.erase(std::remove_if(t.conditions.begin(), t.conditions.end(),
                                              [&](const AsmCondition& c) { return c.param == name; }),
                               t.conditions.end());
        for (AsmState& s : m.states) {
            if (s.speedParam == name) s.speedParam.clear();
            if (s.xParam == name) s.xParam.clear();
            if (s.yParam == name) s.yParam.clear();
        }
    }
    return true;
}

bool asmRenameParam(AnimStateMachineAsset& a, i32 index, const std::string& name) {
    if (index < 0 || static_cast<usize>(index) >= a.params.size() || name.empty()) return false;
    const i32 other = a.paramIndex(name);
    if (other >= 0 && other != index) return false;
    const std::string old = a.params[static_cast<usize>(index)].name;
    a.params[static_cast<usize>(index)].name = name;
    for (AsmMachine& m : a.machines) {
        for (AsmTransition& t : m.transitions)
            for (AsmCondition& c : t.conditions)
                if (c.param == old) c.param = name;
        for (AsmState& s : m.states) {
            if (s.speedParam == old) s.speedParam = name;
            if (s.xParam == old) s.xParam = name;
            if (s.yParam == old) s.yParam = name;
        }
    }
    return true;
}

void asmSetParamType(AnimStateMachineAsset& a, i32 index, AsmParamType type) {
    if (index < 0 || static_cast<usize>(index) >= a.params.size()) return;
    anim::AsmParam& p = a.params[static_cast<usize>(index)];
    p.type = type;
    const std::vector<AsmOp> ok = asmOpsFor(type);
    for (AsmMachine& m : a.machines)
        for (AsmTransition& t : m.transitions)
            for (AsmCondition& c : t.conditions)
                if (c.param == p.name && std::find(ok.begin(), ok.end(), c.op) == ok.end()) c.op = asmDefaultOp(type);
}

AsmOp asmDefaultOp(AsmParamType type) {
    switch (type) {
        case AsmParamType::Bool:    return AsmOp::IsTrue;
        case AsmParamType::Trigger: return AsmOp::Trigger;
        default:                    return AsmOp::Greater;
    }
}

std::vector<AsmOp> asmOpsFor(AsmParamType type) {
    switch (type) {
        case AsmParamType::Bool:    return {AsmOp::IsTrue, AsmOp::IsFalse};
        case AsmParamType::Trigger: return {AsmOp::Trigger};
        default: return {AsmOp::Greater, AsmOp::GreaterEq, AsmOp::Less, AsmOp::LessEq, AsmOp::Equal, AsmOp::NotEqual};
    }
}

const char* asmOpLabel(AsmOp op) {
    static const char* n[] = {">", ">=", "<", "<=", "==", "!=", "is true", "is false", "trigger"};
    return n[static_cast<int>(op)];
}

bool asmOpTakesValue(AsmOp op) { return op <= AsmOp::NotEqual; }

std::string asmConditionText(const AsmCondition& c) {
    std::string s = c.param + " ";
    if (c.op == AsmOp::Trigger) return c.param + " (trigger)";
    s += asmOpLabel(c.op);
    if (asmOpTakesValue(c.op)) {
        char b[32];
        std::snprintf(b, sizeof b, " %g", static_cast<double>(c.value));
        s += b;
    }
    return s;
}

AsmPt asmRectAnchor(f32 cx, f32 cy, f32 hw, f32 hh, f32 tx, f32 ty) {
    const f32 dx = tx - cx, dy = ty - cy;
    if (std::fabs(dx) < 1e-6f && std::fabs(dy) < 1e-6f) return {cx, cy};
    const f32 sx = std::fabs(dx) > 1e-6f ? hw / std::fabs(dx) : 1e9f;
    const f32 sy = std::fabs(dy) > 1e-6f ? hh / std::fabs(dy) : 1e9f;
    const f32 s = std::min(sx, sy);
    return {cx + dx * s, cy + dy * s};
}

f32 asmDistToSegment(f32 px, f32 py, f32 ax, f32 ay, f32 bx, f32 by) {
    const f32 dx = bx - ax, dy = by - ay;
    const f32 l2 = dx * dx + dy * dy;
    f32 t = l2 > 1e-9f ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const f32 qx = ax + t * dx - px, qy = ay + t * dy - py;
    return std::sqrt(qx * qx + qy * qy);
}

// ------------------------------------------------------------------------------- the tab ----------

AnimStateMachineEditor::AnimStateMachineEditor(std::string path) : path_(std::move(path)) { loadFromDisk(); }

void AnimStateMachineEditor::loadFromDisk() {
    std::string why;
    AnimStateMachineAsset a;
    if (!anim::loadStateMachine(path_, a, &why)) {
        loaded_ = false;
        loadError_ = why;
        return;
    }
    if (a.machines.empty()) {
        // A file with no machine at all cannot be shown; give it the root every machine has.
        AsmMachine root;
        root.name = "Root";
        a.machines.push_back(std::move(root));
    }
    asset_ = std::move(a);
    loaded_ = true;
    loadError_.clear();
    dirty_ = false;
    machine_ = 0;
    selState_ = selTrans_ = -1;
    simStale_ = true;
    history_.clear();
}

std::string AnimStateMachineEditor::title() const {
    return std::filesystem::path(path_).filename().string() + "###asm:" + path_;
}

bool AnimStateMachineEditor::save(std::string* why) {
    if (!loaded_) {
        if (why) *why = "cannot save: file failed to load (" + loadError_ + ")";
        return false;
    }
    if (!anim::saveStateMachine(path_, asset_, why)) return false;
    dirty_ = false;
    return true;
}

void AnimStateMachineEditor::onFileChanged() {
    if (dirty_) {
        AVER_WARN("[AnimStateMachineEditor] '{}' changed on disk, but this tab has unsaved edits -- keeping them", path_);
        return;
    }
    loadFromDisk();
}

void AnimStateMachineEditor::openMachine(i32 m) {
    if (!inMachines(asset_, m)) return;
    machine_ = m;
    selState_ = selTrans_ = -1;
}

void AnimStateMachineEditor::undo() {
    if (!history_.undo(asset_)) return;
    dirty_ = true;
    simStale_ = true;
    if (!inMachines(asset_, machine_)) machine_ = 0;
    selState_ = selTrans_ = -1;
}

void AnimStateMachineEditor::redo() {
    if (!history_.redo(asset_)) return;
    dirty_ = true;
    simStale_ = true;
    if (!inMachines(asset_, machine_)) machine_ = 0;
    selState_ = selTrans_ = -1;
}

i32 AnimStateMachineEditor::addState(AsmStateKind kind, f32 x, f32 y) {
    if (!loaded_ || !inMachines(asset_, machine_)) return -1;
    pushUndo();
    const char* base = kind == AsmStateKind::BlendSpace ? "BlendSpace" : "State";
    const i32 idx = asmAddState(asset_.machines[static_cast<usize>(machine_)], kind, base, x, y);
    selectState(idx);
    markDirty();
    return idx;
}

i32 AnimStateMachineEditor::addSubMachine(f32 x, f32 y) {
    if (!loaded_) return -1;
    pushUndo();
    const i32 idx = asmAddSubMachine(asset_, machine_, "SubMachine", x, y);
    if (idx < 0) { history_.cancelPush(); return -1; }
    selectState(idx);
    markDirty();
    return idx;
}

void AnimStateMachineEditor::deleteSelectedState() {
    if (!loaded_ || selState_ < 0) return;
    pushUndo();
    if (!asmRemoveState(asset_, machine_, selState_)) { history_.cancelPush(); return; }
    selState_ = selTrans_ = -1;
    markDirty();
}

i32 AnimStateMachineEditor::addTransition(i32 from, i32 to) {
    if (!loaded_ || !inMachines(asset_, machine_)) return -1;
    pushUndo();
    const i32 idx = asmAddTransition(asset_, asset_.machines[static_cast<usize>(machine_)], from, to, machine_ == 0);
    if (idx < 0) { history_.cancelPush(); return -1; }
    selectTransition(idx);
    markDirty();
    return idx;
}

void AnimStateMachineEditor::deleteSelectedTransition() {
    if (!loaded_ || selTrans_ < 0 || !inMachines(asset_, machine_)) return;
    pushUndo();
    if (!asmRemoveTransition(asset_.machines[static_cast<usize>(machine_)], selTrans_)) { history_.cancelPush(); return; }
    selState_ = selTrans_ = -1;
    markDirty();
}

void AnimStateMachineEditor::setEntry(i32 state) {
    if (!loaded_ || !inMachines(asset_, machine_)) return;
    AsmMachine& m = asset_.machines[static_cast<usize>(machine_)];
    if (!inStates(m, state) || m.entry == state) return;
    pushUndo();
    m.entry = state;
    markDirty();
}

#if AVER_WITH_IMGUI

namespace {

constexpr i32 kLinkNone = -3;

const char* kindLabel(AsmStateKind k) {
    return k == AsmStateKind::Clip ? "Clip" : k == AsmStateKind::BlendSpace ? "Blend space" : "Sub-machine";
}

ImU32 kindColor(AsmStateKind k) {
    return k == AsmStateKind::Clip ? IM_COL32(48, 84, 130, 255)
         : k == AsmStateKind::BlendSpace ? IM_COL32(38, 112, 108, 255)
                                         : IM_COL32(104, 66, 140, 255);
}

void arrow(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, f32 thick, f32 head) {
    dl->AddLine(a, b, col, thick);
    const f32 dx = b.x - a.x, dy = b.y - a.y;
    const f32 len = std::sqrt(dx * dx + dy * dy);
    if (len < 1.0f) return;
    const f32 ux = dx / len, uy = dy / len;
    const ImVec2 base(b.x - ux * head, b.y - uy * head);
    dl->AddTriangleFilled(b, ImVec2(base.x - uy * head * 0.5f, base.y + ux * head * 0.5f),
                          ImVec2(base.x + uy * head * 0.5f, base.y - ux * head * 0.5f), col);
}

} // namespace

void AnimStateMachineEditor::drawCanvas() {
    const f32 dpi = ImGui::GetFontSize() / 16.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 size(std::max(avail.x, 160.0f * dpi), std::max(avail.y, 160.0f * dpi));
    ImGui::InvisibleButton("##asmcanvas", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const ImGuiIO& io = ImGui::GetIO();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(24, 25, 29, 255));

    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) { panX_ += io.MouseDelta.x; panY_ += io.MouseDelta.y; }

    AsmMachine& m = asset_.machines[static_cast<usize>(machine_)];
    const ImVec2 origin(p0.x + panX_ + 200.0f * dpi, p0.y + panY_ + 30.0f * dpi);
    const f32 nw = 150.0f * dpi, nh = 40.0f * dpi;

    struct Node { f32 cx, cy, hw, hh; };
    auto stateNode = [&](i32 i) {
        const AsmState& s = m.states[static_cast<usize>(i)];
        return Node{origin.x + s.posX * dpi + nw * 0.5f, origin.y + s.posY * dpi + nh * 0.5f, nw * 0.5f, nh * 0.5f};
    };
    const Node anyNode{origin.x - 130.0f * dpi, origin.y + 10.0f * dpi + nh * 0.25f, 40.0f * dpi, nh * 0.35f};
    const Node exitNode{origin.x - 130.0f * dpi, origin.y + 80.0f * dpi + nh * 0.25f, 40.0f * dpi, nh * 0.35f};
    auto nodeFor = [&](i32 id) {
        return id == anim::kAsmAny ? anyNode : id == anim::kAsmExit ? exitNode : stateNode(id);
    };

    // Which states are live in the simulator, by name along the active path.
    std::set<std::string> live;
    if (simOn_ && !simStale_) {   // a stale sim still indexes the asset as it was before the edit
        std::string path = sim_.activePath(), part;
        for (const char c : path + "/") {
            if (c == '/') { if (!part.empty()) live.insert(part); part.clear(); } else part += c;
        }
    }

    // Transitions under the nodes. A pair that has both directions is drawn as two parallel lines.
    ImU32 colLine = IM_COL32(170, 175, 185, 255), colSel = IM_COL32(255, 190, 70, 255);
    std::vector<ImVec2> mids(m.transitions.size());
    std::vector<std::pair<ImVec2, ImVec2>> segs(m.transitions.size());
    for (usize i = 0; i < m.transitions.size(); ++i) {
        const AsmTransition& t = m.transitions[i];
        if ((t.from != anim::kAsmAny && !inStates(m, t.from)) || (t.to != anim::kAsmExit && !inStates(m, t.to))) continue;
        const Node a = nodeFor(t.from), b = nodeFor(t.to);
        f32 ox = 0, oy = 0;
        bool reverse = false;
        for (const AsmTransition& u : m.transitions)
            if (u.from == t.to && u.to == t.from && t.from != t.to) reverse = true;
        if (reverse) {
            const f32 dx = b.cx - a.cx, dy = b.cy - a.cy, len = std::max(std::sqrt(dx * dx + dy * dy), 1.0f);
            ox = -dy / len * 7.0f * dpi;
            oy = dx / len * 7.0f * dpi;
        }
        const AsmPt pa = asmRectAnchor(a.cx, a.cy, a.hw, a.hh, b.cx + ox, b.cy + oy);
        const AsmPt pb = asmRectAnchor(b.cx, b.cy, b.hw, b.hh, a.cx + ox, a.cy + oy);
        const ImVec2 A(pa.x + ox, pa.y + oy), B(pb.x + ox, pb.y + oy);
        segs[i] = {A, B};
        mids[i] = ImVec2((A.x + B.x) * 0.5f, (A.y + B.y) * 0.5f);
        arrow(dl, A, B, static_cast<i32>(i) == selTrans_ ? colSel : colLine, static_cast<i32>(i) == selTrans_ ? 3.0f : 1.8f, 9.0f * dpi);
        std::string label = t.conditions.empty() ? (t.hasExitTime ? "exit time" : "always") : asmConditionText(t.conditions[0]);
        if (t.conditions.size() > 1) label += " +" + std::to_string(t.conditions.size() - 1);
        dl->AddText(ImVec2(mids[i].x - 30.0f * dpi, mids[i].y - 14.0f * dpi), IM_COL32(190, 195, 205, 255), label.c_str());
    }

    // Pseudo nodes.
    auto pill = [&](const Node& n, const char* text, ImU32 col) {
        dl->AddRectFilled(ImVec2(n.cx - n.hw, n.cy - n.hh), ImVec2(n.cx + n.hw, n.cy + n.hh), col, 8.0f * dpi);
        dl->AddText(ImVec2(n.cx - n.hw + 8.0f * dpi, n.cy - 7.0f * dpi), IM_COL32(240, 240, 240, 255), text);
    };
    pill(anyNode, "Any state", IM_COL32(120, 80, 40, 255));
    if (machine_ != 0) pill(exitNode, "Exit", IM_COL32(120, 50, 50, 255));

    // States.
    for (usize i = 0; i < m.states.size(); ++i) {
        const AsmState& s = m.states[i];
        const Node n = stateNode(static_cast<i32>(i));
        const ImVec2 a(n.cx - n.hw, n.cy - n.hh), b(n.cx + n.hw, n.cy + n.hh);
        dl->AddRectFilled(a, b, kindColor(s.kind), 6.0f * dpi);
        const bool sel = static_cast<i32>(i) == selState_;
        const bool isLive = live.count(s.name) != 0;
        dl->AddRect(a, b, isLive ? IM_COL32(110, 255, 140, 255) : sel ? colSel : IM_COL32(0, 0, 0, 160), 6.0f * dpi, 0,
                    (sel || isLive) ? 3.0f : 1.0f);
        dl->AddText(ImVec2(a.x + 8.0f * dpi, a.y + 5.0f * dpi), IM_COL32(245, 245, 245, 255), s.name.c_str());
        dl->AddText(ImVec2(a.x + 8.0f * dpi, a.y + 21.0f * dpi), IM_COL32(190, 200, 215, 255),
                    s.kind == AsmStateKind::SubMachine ? "sub-machine (double-click)"
                                                       : std::filesystem::path(s.asset).stem().string().c_str());
        if (static_cast<i32>(i) == m.entry) dl->AddText(ImVec2(a.x, a.y - 16.0f * dpi), IM_COL32(110, 255, 140, 255), "ENTRY");
    }

    // Link in progress.
    if (linkFrom_ != kLinkNone) {
        const Node a = nodeFor(linkFrom_);
        arrow(dl, ImVec2(a.cx, a.cy), io.MousePos, IM_COL32(255, 190, 70, 200), 2.0f, 9.0f * dpi);
        dl->AddText(ImVec2(p0.x + 8.0f * dpi, p0.y + size.y - 22.0f * dpi), IM_COL32(255, 190, 70, 255),
                    "Click the target state (Esc to cancel)");
    }
    dl->PopClipRect();

    auto hitNode = [&](f32 x, f32 y) -> i32 {      // -3 none, kAsmAny, kAsmExit, or a state
        for (usize i = m.states.size(); i-- > 0;) {
            const Node n = stateNode(static_cast<i32>(i));
            if (std::fabs(x - n.cx) <= n.hw && std::fabs(y - n.cy) <= n.hh) return static_cast<i32>(i);
        }
        if (std::fabs(x - anyNode.cx) <= anyNode.hw && std::fabs(y - anyNode.cy) <= anyNode.hh) return anim::kAsmAny;
        if (machine_ != 0 && std::fabs(x - exitNode.cx) <= exitNode.hw && std::fabs(y - exitNode.cy) <= exitNode.hh) return anim::kAsmExit;
        return kLinkNone;
    };

    if (linkFrom_ != kLinkNone && ImGui::IsKeyPressed(ImGuiKey_Escape)) linkFrom_ = kLinkNone;

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const i32 hit = hitNode(io.MousePos.x, io.MousePos.y);
        if (linkFrom_ != kLinkNone) {
            if (hit != kLinkNone && hit != anim::kAsmAny) addTransition(linkFrom_, hit);
            linkFrom_ = kLinkNone;
        } else if (hit >= 0) {
            selectState(hit);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && m.states[static_cast<usize>(hit)].kind == AsmStateKind::SubMachine) {
                openMachine(m.states[static_cast<usize>(hit)].subMachine);
            } else {
                pushUndo();
                dragState_ = hit;
            }
        } else if (hit == anim::kAsmAny) {
            linkFrom_ = anim::kAsmAny;
        } else {
            i32 best = -1;
            f32 bestD = 7.0f * dpi;
            for (usize i = 0; i < segs.size(); ++i) {
                const f32 d = asmDistToSegment(io.MousePos.x, io.MousePos.y, segs[i].first.x, segs[i].first.y, segs[i].second.x, segs[i].second.y);
                if (d < bestD) { bestD = d; best = static_cast<i32>(i); }
            }
            if (best >= 0) selectTransition(best); else { selState_ = selTrans_ = -1; }
        }
    }
    if (dragState_ >= 0 && active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        AsmState& s = m.states[static_cast<usize>(dragState_)];
        s.posX += io.MouseDelta.x / dpi;
        s.posY += io.MouseDelta.y / dpi;
        markDirty();
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) dragState_ = -1;

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        popupX_ = (io.MousePos.x - origin.x) / dpi;
        popupY_ = (io.MousePos.y - origin.y) / dpi;
        ImGui::OpenPopup("##asmctx");
    }
    if (ImGui::BeginPopup("##asmctx")) {
        if (ImGui::MenuItem("Add clip state")) addState(AsmStateKind::Clip, popupX_, popupY_);
        if (ImGui::MenuItem("Add blend-space state")) addState(AsmStateKind::BlendSpace, popupX_, popupY_);
        if (ImGui::MenuItem("Add sub-machine")) addSubMachine(popupX_, popupY_);
        ImGui::EndPopup();
    }
    if (hovered && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        if (selTrans_ >= 0) deleteSelectedTransition();
        else if (selState_ >= 0) deleteSelectedState();
    }
}

void AnimStateMachineEditor::drawDetails() {
    const f32 dpi = ImGui::GetFontSize() / 16.0f;
    AnimStateMachineAsset before = asset_;
    bool ended = false;
    auto touch = [&](bool changed) {
        if (changed) {
            if (!gesture_) { history_.push(before); gesture_ = true; }
            markDirty();
        }
        if (ImGui::IsItemDeactivated()) ended = true;
    };
    char buf[256];
    auto text = [&](const char* label, std::string& v) {
        std::snprintf(buf, sizeof buf, "%s", v.c_str());
        const bool c = ImGui::InputText(label, buf, sizeof buf);
        if (c) v = buf;
        touch(c);
    };

    AsmMachine& m = asset_.machines[static_cast<usize>(machine_)];

    // Breadcrumb up to the root.
    {
        std::vector<i32> chain;
        for (i32 w = machine_; w >= 0; w = asmParentMachine(asset_, w)) {
            chain.push_back(w);
            if (chain.size() > 16) break;
        }
        std::reverse(chain.begin(), chain.end());
        for (usize i = 0; i < chain.size(); ++i) {
            if (i) ImGui::SameLine(0, 4);
            if (i) ImGui::TextUnformatted(">");
            if (i) ImGui::SameLine(0, 4);
            const std::string label = (asset_.machines[static_cast<usize>(chain[i])].name.empty() ? "Machine" : asset_.machines[static_cast<usize>(chain[i])].name) + "##crumb" + std::to_string(i);
            if (ImGui::SmallButton(label.c_str())) openMachine(chain[i]);
        }
    }
    text("Asset name", asset_.name);

    ImGui::SeparatorText("Parameters");
    {
        int removeAt = -1;
        for (usize i = 0; i < asset_.params.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            anim::AsmParam& p = asset_.params[i];
            std::snprintf(buf, sizeof buf, "%s", p.name.c_str());
            ImGui::SetNextItemWidth(110.0f * dpi);
            if (ImGui::InputText("##pn", buf, sizeof buf)) {
                pushUndo();
                if (asmRenameParam(asset_, static_cast<i32>(i), buf)) markDirty(); else history_.cancelPush();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f * dpi);
            int ty = static_cast<int>(p.type);
            if (ImGui::Combo("##pt", &ty, "float\0int\0bool\0trigger\0")) {
                pushUndo();
                asmSetParamType(asset_, static_cast<i32>(i), static_cast<AsmParamType>(ty));
                markDirty();
            }
            if (p.type != AsmParamType::Trigger) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(60.0f * dpi);
                touch(ImGui::DragFloat("##pd", &p.def, 0.05f));
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) removeAt = static_cast<int>(i);
            ImGui::PopID();
        }
        if (removeAt >= 0) {
            pushUndo();
            asmRemoveParam(asset_, removeAt);
            markDirty();
        }
        if (ImGui::Button("Add parameter")) {
            pushUndo();
            std::string n = "param";
            for (int k = 1; asset_.paramIndex(n) >= 0; ++k) n = "param" + std::to_string(k);
            asmAddParam(asset_, n, AsmParamType::Float, 0.0f);
            markDirty();
        }
    }

    ImGui::SeparatorText(m.name.empty() ? "Machine" : m.name.c_str());
    if (inStates(m, selState_)) {
        AsmState& s = m.states[static_cast<usize>(selState_)];
        ImGui::PushID("state");
        text("Name", s.name);
        ImGui::TextDisabled("%s", kindLabel(s.kind));
        if (s.kind == AsmStateKind::SubMachine) {
            if (ImGui::Button("Open")) openMachine(s.subMachine);
        } else {
            text(s.kind == AsmStateKind::Clip ? "Clip" : "Blend space (.ocblend)", s.asset);
            touch(ImGui::DragFloat("Speed", &s.speed, 0.01f, 0.0f, 8.0f));
            if (s.kind == AsmStateKind::Clip) touch(ImGui::Checkbox("Loop", &s.loop));
            auto paramCombo = [&](const char* label, std::string& field) {
                if (ImGui::BeginCombo(label, field.empty() ? "(none)" : field.c_str())) {
                    if (ImGui::Selectable("(none)", field.empty())) { pushUndo(); field.clear(); markDirty(); }
                    for (const anim::AsmParam& p : asset_.params)
                        if (ImGui::Selectable(p.name.c_str(), field == p.name)) { pushUndo(); field = p.name; markDirty(); }
                    ImGui::EndCombo();
                }
            };
            paramCombo("Speed parameter", s.speedParam);
            if (s.kind == AsmStateKind::BlendSpace) {
                paramCombo("Blend X parameter", s.xParam);
                paramCombo("Blend Y parameter", s.yParam);
            }
        }
        text("On enter event", s.onEnter);
        text("On exit event", s.onExit);
        if (ImGui::Button("Set as entry")) setEntry(selState_);
        ImGui::SameLine();
        if (ImGui::Button("New transition")) linkFrom_ = selState_;
        ImGui::SameLine();
        if (ImGui::Button("Delete state")) deleteSelectedState();
        ImGui::PopID();
    } else if (selTrans_ >= 0 && static_cast<usize>(selTrans_) < m.transitions.size()) {
        AsmTransition& t = m.transitions[static_cast<usize>(selTrans_)];
        ImGui::PushID("trans");
        auto nameOf = [&](i32 id) -> std::string {
            return id == anim::kAsmAny ? "Any" : id == anim::kAsmExit ? "Exit" : inStates(m, id) ? m.states[static_cast<usize>(id)].name : "?";
        };
        ImGui::Text("%s -> %s   (priority %d)", nameOf(t.from).c_str(), nameOf(t.to).c_str(), selTrans_ + 1);
        touch(ImGui::DragFloat("Blend time (s)", &t.blendTime, 0.01f, 0.0f, 10.0f));
        touch(ImGui::Checkbox("Has exit time", &t.hasExitTime));
        if (t.hasExitTime) touch(ImGui::DragFloat("Exit time (normalised)", &t.exitTime, 0.01f, 0.0f, 8.0f));
        touch(ImGui::Checkbox("Interruptible", &t.interruptible));
        if (t.from == anim::kAsmAny) touch(ImGui::Checkbox("Allow self re-entry", &t.allowSelf));
        ImGui::SeparatorText("All of these must hold");
        int removeCond = -1;
        for (usize k = 0; k < t.conditions.size(); ++k) {
            AsmCondition& c = t.conditions[k];
            ImGui::PushID(static_cast<int>(k));
            ImGui::SetNextItemWidth(100.0f * dpi);
            if (ImGui::BeginCombo("##cp", c.param.c_str())) {
                for (const anim::AsmParam& p : asset_.params)
                    if (ImGui::Selectable(p.name.c_str(), p.name == c.param)) {
                        pushUndo();
                        c.param = p.name;
                        const auto ops = asmOpsFor(p.type);
                        if (std::find(ops.begin(), ops.end(), c.op) == ops.end()) c.op = asmDefaultOp(p.type);
                        markDirty();
                    }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            const i32 pi = asset_.paramIndex(c.param);
            const auto ops = asmOpsFor(pi >= 0 ? asset_.params[static_cast<usize>(pi)].type : AsmParamType::Float);
            ImGui::SetNextItemWidth(70.0f * dpi);
            if (ImGui::BeginCombo("##co", asmOpLabel(c.op))) {
                for (const AsmOp op : ops)
                    if (ImGui::Selectable(asmOpLabel(op), op == c.op)) { pushUndo(); c.op = op; markDirty(); }
                ImGui::EndCombo();
            }
            if (asmOpTakesValue(c.op)) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(70.0f * dpi);
                touch(ImGui::DragFloat("##cv", &c.value, 0.05f));
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) removeCond = static_cast<int>(k);
            ImGui::PopID();
        }
        if (removeCond >= 0) {
            pushUndo();
            t.conditions.erase(t.conditions.begin() + removeCond);
            markDirty();
        }
        ImGui::BeginDisabled(asset_.params.empty());
        if (ImGui::Button("Add condition")) {
            pushUndo();
            t.conditions.push_back({asset_.params[0].name, asmDefaultOp(asset_.params[0].type), 0.0f});
            markDirty();
        }
        ImGui::EndDisabled();
        if (t.conditions.empty() && !t.hasExitTime) ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "No condition and no exit time: fires immediately.");
        if (ImGui::Button("Higher priority")) { pushUndo(); const i32 n = asmMoveTransition(m, selTrans_, -1); if (n >= 0) { selTrans_ = n; markDirty(); } else history_.cancelPush(); }
        ImGui::SameLine();
        if (ImGui::Button("Lower priority")) { pushUndo(); const i32 n = asmMoveTransition(m, selTrans_, 1); if (n >= 0) { selTrans_ = n; markDirty(); } else history_.cancelPush(); }
        ImGui::SameLine();
        if (ImGui::Button("Delete transition")) deleteSelectedTransition();
        ImGui::PopID();
    } else {
        ImGui::TextDisabled("Select a state or transition. Right-click the canvas to add states; middle-drag pans.");
        if (ImGui::Button("New Any-state transition")) linkFrom_ = anim::kAsmAny;
    }

    std::string why;
    if (!asset_.valid(&why)) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "Invalid: %s", why.c_str());
    if (ended) gesture_ = false;

    drawSimulator();
}

void AnimStateMachineEditor::drawSimulator() {
    ImGui::SeparatorText("Simulator");
    const bool wasOn = simOn_;
    ImGui::Checkbox("Run", &simOn_);
    if (!simOn_) return;
    std::string why;
    if (!asset_.valid(&why)) {
        ImGui::TextDisabled("Fix the asset first: %s", why.c_str());
        simOn_ = false;
        return;
    }
    if (simStale_ || !wasOn) {
        sim_.bind(&asset_, [](const std::string&) -> const fmt::OcAnimation* { return nullptr; },
                  [](const std::string&) -> const anim::BlendSpaceAsset* { return nullptr; });
        simStale_ = false;
        simLog_.clear();
    }
    ImGui::TextDisabled("Clip lengths are unknown here: every clip counts as 1 s.");
    for (const anim::AsmParam& p : asset_.params) {
        ImGui::PushID(p.name.c_str());
        f32 v = sim_.value(p.name);
        switch (p.type) {
            case AsmParamType::Float:
                if (ImGui::DragFloat(p.name.c_str(), &v, 0.02f)) sim_.setFloat(p.name, v);
                break;
            case AsmParamType::Int: {
                int iv = static_cast<int>(v);
                if (ImGui::DragInt(p.name.c_str(), &iv)) sim_.setInt(p.name, iv);
                break;
            }
            case AsmParamType::Bool: {
                bool b = v != 0.0f;
                if (ImGui::Checkbox(p.name.c_str(), &b)) sim_.setBool(p.name, b);
                break;
            }
            case AsmParamType::Trigger:
                if (ImGui::Button(("Fire " + p.name).c_str())) sim_.setTrigger(p.name);
                break;
        }
        ImGui::PopID();
    }
    sim_.tick(ImGui::GetIO().DeltaTime);
    sim_.drainEvents(simLog_);
    if (simLog_.size() > 8) simLog_.erase(simLog_.begin(), simLog_.end() - 8);

    ImGui::Text("Active: %s   (t = %.2f)", sim_.activePath().c_str(), sim_.normalizedTime());
    const std::vector<f32> w = sim_.layerWeights();
    const std::vector<std::string> names = sim_.layerStates();
    for (usize i = 0; i < w.size() && i < names.size(); ++i) ImGui::ProgressBar(w[i], ImVec2(-1, 0), names[i].c_str());
    for (const anim::AsmEvent& e : simLog_)
        ImGui::TextDisabled("%s %s%s%s", e.kind == anim::AsmEvent::Kind::Enter ? "enter" : "exit ", e.state.c_str(),
                            e.name.empty() ? "" : "  ->  ", e.name.c_str());
}

void AnimStateMachineEditor::draw(Engine& e) {
    (void)e;
    if (!loaded_) {
        ImGui::TextWrapped("This file could not be read: %s", loadError_.c_str());
        return;
    }
    if (ImGui::Button("Save") || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                                  keybinds().pressed(CommandId::AssetSave, ImGui::GetIO()))) {
        std::string why;
        if (!save(&why)) AVER_ERROR("[AnimStateMachineEditor] save failed for '{}': {}", path_, why);
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
    ImGui::BeginDisabled(machine_ == 0);
    if (ImGui::Button("Up")) { const i32 p = asmParentMachine(asset_, machine_); openMachine(p >= 0 ? p : 0); }
    ImGui::EndDisabled();
    ImGui::Separator();

    const f32 dpi = ImGui::GetFontSize() / 16.0f;
    const f32 detailsW = std::min(ImGui::GetContentRegionAvail().x * 0.4f, 380.0f * dpi);
    if (ImGui::BeginChild("##asmcanvaspane", ImVec2(ImGui::GetContentRegionAvail().x - detailsW - 8.0f * dpi, 0), true)) drawCanvas();
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##asmdetails", ImVec2(0, 0), true)) drawDetails();
    ImGui::EndChild();
}

#else   // AVER_WITH_IMGUI

void AnimStateMachineEditor::draw(Engine& e) { (void)e; }

#endif  // AVER_WITH_IMGUI

std::unique_ptr<AssetEditor> makeAnimStateMachineEditor(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".ocasm") return nullptr;
    return std::make_unique<AnimStateMachineEditor>(path);
}

} // namespace aver::editor
