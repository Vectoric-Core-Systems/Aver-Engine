// The visual behaviour-tree editor tab. See the header for the wiring.

#include "BtGraphEditor.hpp"
#include "EditorKeybinds.hpp"

#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <filesystem>

#if AVER_MODULE_SYNAPSE_SCENE && AVER_MODULE_SCENE
#  include "aver/scene/World.hpp"
#  include "aver/synapse/SynapseBt.hpp"
#  define AVER_BTGE_LIVE 1
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
using synapse::BtAbortMode;
using synapse::BtAsset;
using synapse::BtAssetNode;
using synapse::BtDecorator;

namespace {

bool inRange(const BtAsset& a, i32 i) { return i >= 0 && static_cast<usize>(i) < a.nodes.size(); }

std::string fmtG(f32 v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", static_cast<double>(v));
    return buf;
}

f32 nodeHeight(const BtAssetNode& n, const BtLayoutParams& p) {
    f32 h = p.headerH + p.padY;
    h += p.lineH * static_cast<f32>(n.decorators.size());
    if (!n.comment.empty()) h += p.lineH;
    return h;
}

} // namespace

// ---- layout -------------------------------------------------------------------------------------

std::vector<BtNodeBox> btLayout(const BtAsset& asset, const BtLayoutParams& p) {
    const usize n = asset.nodes.size();
    std::vector<i32> parents(n, -2);
    std::vector<f32> heights(n, 0.0f);
    for (usize i = 0; i < n; ++i) {
        heights[i] = nodeHeight(asset.nodes[i], p);
        const i32 parent = asset.nodes[i].node.parent;
        if (i == 0) parents[i] = -1;                                              // the root
        else if (parent >= 0 && static_cast<usize>(parent) < i) parents[i] = parent;   // malformed: an orphan
    }
    return layoutTree(parents, heights, TreeLayoutParams{p.nodeW, p.gapX, p.gapY});
}

BtNodeBox btLayoutBounds(const std::vector<BtNodeBox>& boxes) { return treeBounds(boxes); }

i32 btHitTest(const std::vector<BtNodeBox>& boxes, f32 x, f32 y) { return treeHitTest(boxes, x, y); }

std::string btNodeTitle(const BtAssetNode& n) {
    std::string s = synapse::btNodeKindName(n.node.kind);
    if (!n.node.name.empty()) s += "  " + n.node.name;
    if (n.node.kind == fmt::OcBtNodeKind::Cooldown || n.node.name == "Wait")
        s += " (" + fmtG(n.node.params[0]) + "s)";
    else if (n.node.name == "BbCompare" || n.node.name == "BbSet" || n.node.name == "BbClear" ||
             n.node.name == "FireEvent")
        s += ": " + n.node.stringParam;
    return s;
}

std::string btDecoratorText(const BtDecorator& d) {
    std::string s = d.key.empty() ? "(no key)" : d.key;
    if (d.op == BbOp::IsSet) s += " is set";
    else if (d.op == BbOp::NotSet) s += " not set";
    else s += std::string(" ") + synapse::bbOpName(d.op) + " " + synapse::bbToString(d.value);
    if (d.abort != BtAbortMode::None) s += std::string("  [") + synapse::btAbortModeName(d.abort) + "]";
    return s;
}

bool btGraphWriteStarter(const std::string& path, std::string* why) {
    return synapse::saveBtAsset(path, synapse::btAssetStarter(), why);
}

// ---- the tab: load, save, undo ------------------------------------------------------------------

BtGraphEditor::BtGraphEditor(std::string path) : path_(std::move(path)) { loadFromDisk(); }

BtGraphEditor::~BtGraphEditor() {
#if AVER_BTGE_LIVE
    // Stop recording per-node results for an entity nobody is looking at any more.
    if (debugEntity_ != 0 && synapse::btSystem().watched() == debugEntity_)
        synapse::btSystem().watch(scene::kInvalidEntity);
#endif
}

void BtGraphEditor::loadFromDisk() {
    BtAsset a;
    std::string why;
    if (!synapse::loadBtAsset(path_, a, &why)) {
        loaded_ = false;
        loadError_ = why;
        return;
    }
    asset_ = std::move(a);
    loaded_ = true;
    loadError_.clear();
    dirty_ = false;
    selected_ = 0;
    history_.clear();
}

std::string BtGraphEditor::title() const {
    // The host marks unsaved documents itself (AssetEditor.cpp), so no '*' here.
    return std::filesystem::path(path_).filename().string() + "###btg:" + path_;
}

bool BtGraphEditor::save(std::string* why) {
    if (!loaded_) {
        if (why) *why = "cannot save: file failed to load (" + loadError_ + ")";
        return false;
    }
    if (!asset_.valid()) {
        if (why) *why = "the tree is not valid: a node is missing its name, or the structure is broken";
        return false;
    }
    if (!synapse::saveBtAsset(path_, asset_, why)) return false;
    dirty_ = false;
#if AVER_BTGE_LIVE
    synapse::btSystem().reloadTree(path_);   // running entities pick the new tree up on their next tick
    if (const std::string rel = aiRelativeAssetPath(path_); !rel.empty())
        synapse::btSystem().reloadTree(rel);   // AI: wave 3 #21 -- brains assigned by content path hash that spelling
#endif
    return true;
}

void BtGraphEditor::onFileChanged() {
    // A dirty tab keeps its edits; a clean one reloads (same rule as BtEditor).
    if (dirty_) {
        AVER_WARN("[BtGraphEditor] '{}' changed on disk, but this tab has unsaved edits -- keeping them", path_);
        return;
    }
    loadFromDisk();
}

void BtGraphEditor::undo() {
    if (!history_.undo(asset_)) return;
    dirty_ = true;
    if (!inRange(asset_, selected_)) selected_ = 0;
}

void BtGraphEditor::redo() {
    if (!history_.redo(asset_)) return;
    dirty_ = true;
    if (!inRange(asset_, selected_)) selected_ = 0;
}

// ---- edits --------------------------------------------------------------------------------------

bool BtGraphEditor::addChild(fmt::OcBtNodeKind kind, const std::string& leafName) {
    if (!loaded_ || !inRange(asset_, selected_)) return false;
    pushUndo();
    const i32 added = synapse::btAssetAddChild(asset_, selected_, kind, leafName);
    if (added < 0) { history_.cancelPush(); return false; }
    selected_ = added;
    dirty_ = true;
    return true;
}

bool BtGraphEditor::deleteSelected() {
    if (!loaded_ || selected_ == 0) return false;
    pushUndo();
    const i32 next = synapse::btAssetDeleteSubtree(asset_, selected_);
    if (next < 0) { history_.cancelPush(); return false; }
    selected_ = next;
    dirty_ = true;
    return true;
}

bool BtGraphEditor::reparentSelected(i32 newParent) {
    if (!loaded_) return false;
    pushUndo();
    const i32 moved = synapse::btAssetReparent(asset_, selected_, newParent);
    if (moved < 0) { history_.cancelPush(); return false; }
    selected_ = moved;
    dirty_ = true;
    return true;
}

bool BtGraphEditor::moveSelected(i32 delta) {
    if (!loaded_) return false;
    pushUndo();
    const i32 moved = synapse::btAssetMoveSibling(asset_, selected_, delta);
    if (moved < 0) { history_.cancelPush(); return false; }
    selected_ = moved;
    dirty_ = true;
    return true;
}

bool BtGraphEditor::setSelectedKind(fmt::OcBtNodeKind kind) {
    if (!loaded_ || !inRange(asset_, selected_)) return false;
    if (asset_.nodes[static_cast<usize>(selected_)].node.kind == kind) return false;
    pushUndo();
    synapse::btAssetSetKind(asset_, selected_, kind);
    dirty_ = true;
    return true;
}

bool BtGraphEditor::setNodeName(const std::string& name) {
    if (!loaded_ || !inRange(asset_, selected_) || name.empty()) return false;
    BtAssetNode& n = asset_.nodes[static_cast<usize>(selected_)];
    const bool leaf = n.node.kind == fmt::OcBtNodeKind::Condition || n.node.kind == fmt::OcBtNodeKind::Action;
    if (!leaf || n.node.name == name) return false;
    pushUndo();
    n.node.name = name;
    dirty_ = true;
    return true;
}

bool BtGraphEditor::setNodeParam(int slot, f32 value) {
    if (!loaded_ || !inRange(asset_, selected_) || slot < 0 || slot > 3) return false;
    BtAssetNode& n = asset_.nodes[static_cast<usize>(selected_)];
    if (n.node.params[slot] == value) return false;
    pushUndo();
    n.node.params[slot] = value;
    dirty_ = true;
    return true;
}

bool BtGraphEditor::setNodeString(const std::string& text) {
    if (!loaded_ || !inRange(asset_, selected_)) return false;
    BtAssetNode& n = asset_.nodes[static_cast<usize>(selected_)];
    if (n.node.stringParam == text) return false;
    pushUndo();
    n.node.stringParam = text;
    dirty_ = true;
    return true;
}

bool BtGraphEditor::setNodeComment(const std::string& text) {
    if (!loaded_ || !inRange(asset_, selected_)) return false;
    BtAssetNode& n = asset_.nodes[static_cast<usize>(selected_)];
    if (n.comment == text) return false;
    pushUndo();
    n.comment = text;
    dirty_ = true;
    return true;
}

bool BtGraphEditor::addDecorator() {
    if (!loaded_ || !inRange(asset_, selected_)) return false;
    pushUndo();
    BtDecorator d;
    if (!asset_.schema.keys.empty()) {
        const BbKeyDef& k = asset_.schema.keys.front();
        d.key = k.name;
        d.value = synapse::bbDefault(k.type);
        d.op = k.type == BbType::Bool ? BbOp::IsSet : BbOp::Eq;
    }
    asset_.nodes[static_cast<usize>(selected_)].decorators.push_back(std::move(d));
    dirty_ = true;
    return true;
}

bool BtGraphEditor::removeDecorator(usize index) {
    if (!loaded_ || !inRange(asset_, selected_)) return false;
    auto& list = asset_.nodes[static_cast<usize>(selected_)].decorators;
    if (index >= list.size()) return false;
    pushUndo();
    list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));
    dirty_ = true;
    return true;
}

bool BtGraphEditor::setDecorator(usize index, const BtDecorator& d) {
    if (!loaded_ || !inRange(asset_, selected_)) return false;
    auto& list = asset_.nodes[static_cast<usize>(selected_)].decorators;
    if (index >= list.size()) return false;
    pushUndo();
    list[index] = d;
    dirty_ = true;
    return true;
}

bool BtGraphEditor::addKey(const BbKeyDef& def) {
    if (!loaded_) return false;
    pushUndo();
    if (!asset_.schema.add(def)) { history_.cancelPush(); return false; }
    dirty_ = true;
    return true;
}

bool BtGraphEditor::removeKey(const std::string& name) {
    if (!loaded_ || asset_.schema.indexOf(name) < 0) return false;
    pushUndo();
    asset_.schema.remove(name);
    dirty_ = true;
    return true;
}

bool BtGraphEditor::renameKey(const std::string& from, const std::string& to) {
    if (!loaded_) return false;
    pushUndo();
    if (!synapse::btAssetRenameKey(asset_, from, to)) { history_.cancelPush(); return false; }
    dirty_ = true;
    return true;
}

bool BtGraphEditor::setKey(const std::string& name, const BbKeyDef& def) {
    if (!loaded_) return false;
    const i32 idx = asset_.schema.indexOf(name);
    if (idx < 0) return false;
    pushUndo();
    if (def.name != name && !synapse::btAssetRenameKey(asset_, name, def.name)) {
        history_.cancelPush();
        return false;
    }
    BbKeyDef& k = asset_.schema.keys[static_cast<usize>(idx)];
    k.type = def.type;
    k.scope = def.scope;
    k.description = def.description;
    BbValue c;
    k.defaultValue = synapse::bbCoerce(def.type, def.defaultValue, c) ? c : synapse::bbDefault(def.type);
    dirty_ = true;
    return true;
}

std::unique_ptr<AssetEditor> makeBtGraphEditor(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".ocbt") return nullptr;
    return std::make_unique<BtGraphEditor>(path);
}

// ================================================================================== drawing ========

#if AVER_WITH_IMGUI

namespace {

constexpr f32 kDefaultCanvasFraction = 0.66f;
constexpr const char* kPrefSplit = "btGraphEditor.split";

const char* const kConditionNames[] = {"BbCompare", "HasTarget", "CanSeeTarget", "DistanceToTargetLess"};
const char* const kActionNames[] = {"BbSet", "BbClear", "MoveTo", "Wait", "LookAt", "FireEvent"};
const fmt::OcBtNodeKind kStructural[] = {
    fmt::OcBtNodeKind::Selector, fmt::OcBtNodeKind::Sequence, fmt::OcBtNodeKind::Parallel,
    fmt::OcBtNodeKind::Inverter, fmt::OcBtNodeKind::Succeeder, fmt::OcBtNodeKind::Cooldown};

ImU32 kindColor(fmt::OcBtNodeKind k) {
    switch (k) {
        case fmt::OcBtNodeKind::Selector:  return IM_COL32(70, 100, 170, 255);
        case fmt::OcBtNodeKind::Sequence:  return IM_COL32(60, 135, 95, 255);
        case fmt::OcBtNodeKind::Parallel:  return IM_COL32(130, 85, 165, 255);
        case fmt::OcBtNodeKind::Inverter:  return IM_COL32(165, 105, 60, 255);
        case fmt::OcBtNodeKind::Succeeder: return IM_COL32(150, 120, 60, 255);
        case fmt::OcBtNodeKind::Cooldown:  return IM_COL32(85, 130, 145, 255);
        case fmt::OcBtNodeKind::Condition: return IM_COL32(170, 140, 50, 255);
        case fmt::OcBtNodeKind::Action:    return IM_COL32(80, 90, 110, 255);
    }
    return IM_COL32(80, 80, 80, 255);
}

ImU32 statusColor(synapse::BtStatus s) {
    switch (s) {
        case synapse::BtStatus::Running: return IM_COL32(245, 175, 40, 255);
        case synapse::BtStatus::Success: return IM_COL32(80, 205, 110, 255);
        case synapse::BtStatus::Failure: return IM_COL32(225, 80, 80, 255);
    }
    return IM_COL32(150, 150, 150, 255);
}

const char* statusText(synapse::BtStatus s) {
    switch (s) {
        case synapse::BtStatus::Running: return "run";
        case synapse::BtStatus::Success: return "ok";
        case synapse::BtStatus::Failure: return "fail";
    }
    return "?";
}

// What the canvas and panels read from the watched entity. Empty when nothing is being watched.
struct LiveRefs {
    bool valid = false;
    bool sameTree = false;
    const synapse::BtNodeTrace* trace = nullptr;
    usize traceCount = 0;
    u64 tick = 0;
    const synapse::Blackboard* board = nullptr;
    i32 lastStatus = -1;
    u32 aborts = 0;
    i32 lastAborted = -1;
    u32 entity = 0;
};

LiveRefs liveRefs(u32 entity, usize nodeCount) {
    LiveRefs r;
#if AVER_BTGE_LIVE
    if (entity == 0) return r;
    synapse::BtDebugView v;
    if (!synapse::btSystem().debugView(static_cast<scene::Entity>(entity), v)) return r;
    r.valid = true;
    r.entity = entity;
    r.sameTree = v.tree && v.tree->tree.nodes.size() == nodeCount;
    r.board = v.board;
    if (const auto* b = scene::World::instance().component<synapse::CSynapseBehavior>(
            static_cast<scene::Entity>(entity), synapse::btSystem().componentType()))
        r.lastStatus = b->lastStatus;
    if (v.state) {
        r.tick = v.state->tickCount;
        r.aborts = v.state->abortCount;
        r.lastAborted = v.state->lastAbortedNode;
        if (r.sameTree && v.state->nodeTrace.size() == nodeCount) {
            r.trace = v.state->nodeTrace.data();
            r.traceCount = v.state->nodeTrace.size();
        }
    }
#else
    (void)entity; (void)nodeCount;
#endif
    return r;
}

bool opAllowedFor(BbType t, BbOp op) {
    if (op == BbOp::IsSet || op == BbOp::NotSet || op == BbOp::Eq || op == BbOp::Ne) return true;
    return t != BbType::Vec3;
}

} // namespace

void BtGraphEditor::drawToolbar() {
    if (ImGui::Button("Save") || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                                  keybinds().pressed(CommandId::AssetSave, ImGui::GetIO()))) {
        std::string why;
        if (!save(&why)) AVER_ERROR("[BtGraphEditor] save failed for '{}': {}", path_, why);
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
    if (ImGui::BeginCombo("##addnode", "Add child")) {
        for (const fmt::OcBtNodeKind k : kStructural)
            if (ImGui::Selectable(synapse::btNodeKindName(k))) addChild(k);
        ImGui::Separator();
        for (const char* n : kConditionNames)
            if (ImGui::Selectable((std::string("Condition  ") + n).c_str())) addChild(fmt::OcBtNodeKind::Condition, n);
        for (const char* n : kActionNames)
            if (ImGui::Selectable((std::string("Action  ") + n).c_str())) addChild(fmt::OcBtNodeKind::Action, n);
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(selected_ == 0);
    if (ImGui::Button("Delete")) deleteSelected();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Reset view")) view_.reset();

    const usize problems = asset_.validate().size();
    if (problems > 0) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.2f, 1.0f), "%zu warning%s", problems, problems == 1 ? "" : "s");
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            for (const std::string& s : asset_.validate()) ImGui::TextUnformatted(s.c_str());
            ImGui::EndTooltip();
        }
    }
}

void BtGraphEditor::drawCanvas(f32 width) {
    if (!ImGui::BeginChild("##btgcanvas", ImVec2(width, 0), ImGuiChildFlags_Borders,
                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::EndChild();
        return;
    }
    const f32 dpi = ImGui::GetFontSize() / 16.0f;
    BtLayoutParams lp;
    lp.nodeW *= dpi; lp.gapX *= dpi; lp.gapY *= dpi; lp.headerH *= dpi; lp.lineH *= dpi; lp.padY *= dpi;
    const std::vector<BtNodeBox> boxes = btLayout(asset_, lp);
    const LiveRefs live = liveRefs(debugEntity_, asset_.nodes.size());

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##btgsurface", avail,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                           ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();

    const auto toGraph = [&](const ImVec2& s) {
        return ImVec2((s.x - origin.x - view_.panX) / view_.zoom, (s.y - origin.y - view_.panY) / view_.zoom);
    };
    const auto toScreen = [&](f32 x, f32 y) {
        return ImVec2(origin.x + view_.panX + x * view_.zoom, origin.y + view_.panY + y * view_.zoom);
    };

    const ImVec2 g = toGraph(io.MousePos);
    const i32 hit = hovered ? btHitTest(boxes, g.x, g.y) : -1;
    bool clickedEmpty = false;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hit >= 0) selected_ = hit; else clickedEmpty = true;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hit >= 0) {
        selected_ = hit;
        ImGui::OpenPopup("##btgctx");
    }
    canvasInput(view_, origin.x, origin.y, hovered, clickedEmpty);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), true);
    ImFont* font = ImGui::GetFont();
    const f32 fontSize = ImGui::GetFontSize() * view_.zoom;

    // edges, with the execution order beside each child
    for (usize i = 1; i < asset_.nodes.size(); ++i) {
        const i32 p = asset_.nodes[i].node.parent;
        if (p < 0) continue;
        const BtNodeBox& pb = boxes[static_cast<usize>(p)];
        const BtNodeBox& cb = boxes[i];
        const ImVec2 a = toScreen(pb.x + pb.w * 0.5f, pb.y + pb.h);
        const ImVec2 b = toScreen(cb.x + cb.w * 0.5f, cb.y);
        const f32 k = (b.y - a.y) * 0.5f;
        const bool ran = live.trace && i < live.traceCount && live.trace[i].visited && live.trace[i].tick == live.tick;
        dl->AddBezierCubic(a, ImVec2(a.x, a.y + k), ImVec2(b.x, b.y - k), b,
                           ran ? IM_COL32(245, 175, 40, 255) : IM_COL32(150, 155, 165, 220), (ran ? 2.5f : 1.5f) * view_.zoom);
        const std::vector<i32> sibs = synapse::btAssetChildren(asset_, p);
        for (usize s = 0; s < sibs.size(); ++s)
            if (sibs[s] == static_cast<i32>(i)) {
                char num[12];
                std::snprintf(num, sizeof num, "%zu", s + 1);
                dl->AddText(font, fontSize * 0.85f, ImVec2(b.x + 5.0f * view_.zoom, b.y - fontSize - 2.0f * view_.zoom),
                            IM_COL32(190, 195, 205, 255), num);
            }
    }

    // nodes
    for (usize i = 0; i < asset_.nodes.size(); ++i) {
        const BtAssetNode& n = asset_.nodes[i];
        const BtNodeBox& b = boxes[i];
        const ImVec2 p0 = toScreen(b.x, b.y);
        const ImVec2 p1 = toScreen(b.x + b.w, b.y + b.h);
        const f32 round = 5.0f * view_.zoom;
        const f32 headerH = lp.headerH * view_.zoom;
        const synapse::BtNodeTrace* tr = (live.trace && i < live.traceCount && live.trace[i].visited) ? &live.trace[i] : nullptr;
        const bool freshRun = tr && tr->tick == live.tick && tr->status == synapse::BtStatus::Running;

        dl->AddRectFilled(p0, p1, IM_COL32(32, 35, 42, 245), round);
        dl->AddRectFilled(p0, ImVec2(p1.x, p0.y + headerH), kindColor(n.node.kind), round,
                          ImDrawFlags_RoundCornersTop);

        dl->PushClipRect(p0, p1, true);
        dl->AddText(font, fontSize, ImVec2(p0.x + 6.0f * view_.zoom, p0.y + (headerH - fontSize) * 0.5f),
                    IM_COL32(245, 245, 250, 255), btNodeTitle(n).c_str());
        f32 ty = p0.y + headerH + 3.0f * view_.zoom;
        for (const BtDecorator& d : n.decorators) {
            const bool pass = live.board ? synapse::evaluateDecorator(d, live.board) : true;
            const ImU32 col = !live.board ? IM_COL32(215, 190, 120, 255)
                                          : (pass ? IM_COL32(120, 215, 140, 255) : IM_COL32(230, 120, 120, 255));
            dl->AddText(font, fontSize * 0.9f, ImVec2(p0.x + 8.0f * view_.zoom, ty), col, ("- " + btDecoratorText(d)).c_str());
            ty += lp.lineH * view_.zoom;
        }
        if (!n.comment.empty())
            dl->AddText(font, fontSize * 0.9f, ImVec2(p0.x + 8.0f * view_.zoom, ty), IM_COL32(150, 160, 175, 255), n.comment.c_str());
        dl->PopClipRect();

        if (tr) {
            const bool fresh = tr->tick == live.tick;
            const ImU32 col = statusColor(tr->status);
            const ImU32 dim = fresh ? col : (col & 0x00FFFFFFu) | (110u << 24);
            const char* label = tr->gated ? "blocked" : statusText(tr->status);
            const ImVec2 ts = ImGui::CalcTextSize(label);
            const f32 sc = view_.zoom * 0.85f;
            dl->AddText(font, fontSize * 0.85f, ImVec2(p1.x - ts.x * sc - 4.0f * view_.zoom, p0.y + 3.0f * view_.zoom),
                        tr->gated ? IM_COL32(230, 120, 120, fresh ? 255 : 120) : dim, label);
            dl->AddRect(p0, p1, dim, round, 0, (freshRun ? 3.0f : 1.5f) * view_.zoom);
        } else {
            dl->AddRect(p0, p1, IM_COL32(95, 100, 112, 255), round, 0, 1.0f * view_.zoom);
        }
        if (static_cast<i32>(i) == selected_) dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 255), round, 0, 2.0f * view_.zoom);
    }
    dl->PopClipRect();

    if (ImGui::BeginPopup("##btgctx")) {
        if (ImGui::BeginMenu("Add child")) {
            // Typing filters the list (case-insensitive, any part of the label); Enter adds the first match.
            if (ImGui::IsWindowAppearing()) {
                addSearch_[0] = '\0';
                ImGui::SetKeyboardFocusHere();
            }
            ImGui::SetNextItemWidth(220.0f);
            const bool enter = ImGui::InputTextWithHint("##btaddsearch", "Search...", addSearch_, sizeof addSearch_,
                                                        ImGuiInputTextFlags_EnterReturnsTrue);
            const auto matches = [this](const std::string& label) {
                std::string a = label, q = addSearch_;
                for (char& c : a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                for (char& c : q) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return a.find(q) != std::string::npos;
            };
            bool firstTaken = !enter;
            const auto item = [&](const std::string& label, auto&& add) {
                if (!matches(label)) return;
                if (!firstTaken) { firstTaken = true; add(); ImGui::CloseCurrentPopup(); return; }
                if (ImGui::MenuItem(label.c_str())) add();
            };
            ImGui::Separator();
            for (const fmt::OcBtNodeKind k : kStructural)
                item(synapse::btNodeKindName(k), [&] { addChild(k); });
            ImGui::Separator();
            for (const char* nm : kConditionNames)
                item(std::string("Condition  ") + nm, [&] { addChild(fmt::OcBtNodeKind::Condition, nm); });
            for (const char* nm : kActionNames)
                item(std::string("Action  ") + nm, [&] { addChild(fmt::OcBtNodeKind::Action, nm); });
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Add decorator")) addDecorator();
        ImGui::Separator();
        if (ImGui::MenuItem("Move earlier", nullptr, false, synapse::btAssetCanMoveSibling(asset_, selected_, -1)))
            moveSelected(-1);
        if (ImGui::MenuItem("Move later", nullptr, false, synapse::btAssetCanMoveSibling(asset_, selected_, 1)))
            moveSelected(1);
        ImGui::Separator();
        if (ImGui::MenuItem("Delete", nullptr, false, selected_ != 0)) deleteSelected();
        ImGui::EndPopup();
    }

    ImGui::SetCursorScreenPos(ImVec2(origin.x + 6.0f, origin.y + avail.y - ImGui::GetTextLineHeightWithSpacing()));
    ImGui::TextDisabled("Drag empty space to pan  |  Ctrl+wheel zooms  |  right-click a node for actions");
    ImGui::EndChild();
}

bool BtGraphEditor::drawDecoratorEditor(usize di) {
    const usize nodeIndex = static_cast<usize>(selected_);
    BtDecorator& d = asset_.nodes[nodeIndex].decorators[di];
    bool removed = false;
    ImGui::PushID(static_cast<int>(di));
    ImGui::Separator();

    const BbKeyDef* def = asset_.schema.find(d.key);
    if (ImGui::BeginCombo("Key", d.key.empty() ? "(choose a key)" : d.key.c_str())) {
        for (const BbKeyDef& k : asset_.schema.keys) {
            if (!ImGui::Selectable(k.name.c_str(), k.name == d.key)) continue;
            BtDecorator nd = d;
            nd.key = k.name;
            if (!def || def->type != k.type) {
                nd.value = synapse::bbDefault(k.type);
                if (!opAllowedFor(k.type, nd.op)) nd.op = BbOp::Eq;
            }
            setDecorator(di, nd);
            break;
        }
        ImGui::EndCombo();
    }
    def = asset_.schema.find(d.key);
    const BbType type = def ? def->type : d.value.type;

    if (ImGui::BeginCombo("Condition", synapse::bbOpName(d.op))) {
        for (u8 o = 0; o <= static_cast<u8>(BbOp::NotSet); ++o) {
            const BbOp op = static_cast<BbOp>(o);
            if (!opAllowedFor(type, op)) continue;
            if (ImGui::Selectable(synapse::bbOpName(op), op == d.op) && op != d.op) {
                BtDecorator nd = d;
                nd.op = op;
                setDecorator(di, nd);
                break;
            }
        }
        ImGui::EndCombo();
    }

    if (d.op != BbOp::IsSet && d.op != BbOp::NotSet) {
        BbValue v = d.value;
        if (v.type != type) v = synapse::bbDefault(type);
        bool activated = false;
        if (bbValueEditor("Value", v, type, activated)) {
            if (activated) pushUndo();
            d.value = v;
            dirty_ = true;
        } else if (activated) {
            pushUndo();
        }
    }

    if (ImGui::BeginCombo("Observer abort", synapse::btAbortModeName(d.abort))) {
        for (u8 m = 0; m <= static_cast<u8>(BtAbortMode::Both); ++m) {
            const BtAbortMode mode = static_cast<BtAbortMode>(m);
            if (ImGui::Selectable(synapse::btAbortModeName(mode), mode == d.abort) && mode != d.abort) {
                BtDecorator nd = d;
                nd.abort = mode;
                setDecorator(di, nd);
                break;
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("None: checked on entry only.\nSelf: abort this branch when the condition turns false.\n"
                          "Lower Priority: when it turns true, abort a running lower-priority branch.\nBoth: both.");

    if (ImGui::SmallButton("Remove decorator")) removed = true;
    ImGui::PopID();
    if (removed) removeDecorator(di);
    return removed;
}

void BtGraphEditor::drawNodePanel() {
    if (!inRange(asset_, selected_)) { ImGui::TextUnformatted("Nothing selected"); return; }
    BtAssetNode& n = asset_.nodes[static_cast<usize>(selected_)];

    if (ImGui::BeginCombo("Kind", synapse::btNodeKindName(n.node.kind))) {
        for (u32 k = 0; k <= static_cast<u32>(fmt::OcBtNodeKind::Action); ++k) {
            const auto kind = static_cast<fmt::OcBtNodeKind>(k);
            if (ImGui::Selectable(synapse::btNodeKindName(kind), kind == n.node.kind)) { setSelectedKind(kind); break; }
        }
        ImGui::EndCombo();
    }

    const bool isLeaf = n.node.kind == fmt::OcBtNodeKind::Condition || n.node.kind == fmt::OcBtNodeKind::Action;
    if (isLeaf) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s", n.node.name.c_str());
        ImGui::InputText("Name", buf, sizeof buf);
        if (ImGui::IsItemDeactivatedAfterEdit()) setNodeName(buf);
        if (ImGui::BeginCombo("Built-in", "pick...")) {
            const bool cond = n.node.kind == fmt::OcBtNodeKind::Condition;
            const char* const* names = cond ? kConditionNames : kActionNames;
            const int count = cond ? static_cast<int>(sizeof kConditionNames / sizeof *kConditionNames)
                                   : static_cast<int>(sizeof kActionNames / sizeof *kActionNames);
            for (int i = 0; i < count; ++i)
                if (ImGui::Selectable(names[i], n.node.name == names[i])) { setNodeName(names[i]); break; }
            ImGui::EndCombo();
        }
    }

    const bool bbLeaf = isLeaf && (n.node.name == "BbCompare" || n.node.name == "BbSet" || n.node.name == "BbClear");
    if (n.node.kind == fmt::OcBtNodeKind::Cooldown || n.node.name == "Wait") {
        f32 secs = n.node.params[0];
        ImGui::DragFloat("Seconds", &secs, 0.05f, 0.0f, 3600.0f);
        if (ImGui::IsItemActivated()) pushUndo();
        if (secs != n.node.params[0]) { n.node.params[0] = secs; dirty_ = true; }
    } else if (!bbLeaf && isLeaf) {
        ImGui::Separator();
        ImGui::TextUnformatted("Parameters");
        for (int i = 0; i < 4; ++i) {
            char label[16];
            std::snprintf(label, sizeof label, "params[%d]", i);
            f32 v = n.node.params[i];
            ImGui::DragFloat(label, &v, 0.5f);
            if (ImGui::IsItemActivated()) pushUndo();
            if (v != n.node.params[i]) { n.node.params[i] = v; dirty_ = true; }
        }
    }

    if (isLeaf) {
        char sbuf[160];
        std::snprintf(sbuf, sizeof sbuf, "%s", n.node.stringParam.c_str());
        ImGui::InputText(bbLeaf ? "Expression" : "String param", sbuf, sizeof sbuf);
        if (ImGui::IsItemDeactivatedAfterEdit()) setNodeString(sbuf);
        if (n.node.name == "BbCompare") ImGui::TextDisabled("key, !key, or key <op> value  (e.g. Alert == true)");
        else if (n.node.name == "BbSet") ImGui::TextDisabled("key = value  (e.g. Ammo = 5)");
        else if (n.node.name == "BbClear") ImGui::TextDisabled("key  (resets it to its default)");
        else if (n.node.name == "FireEvent") ImGui::TextDisabled("FireEvent raises this event name.");
        if (bbLeaf) {
            synapse::BbExpr ex;
            std::string why;
            if (n.node.name != "BbClear" && !synapse::bbParseExpr(n.node.stringParam, &asset_.schema, ex, &why))
                ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1.0f), "%s", why.c_str());
        }
    }

    {
        char cbuf[160];
        std::snprintf(cbuf, sizeof cbuf, "%s", n.comment.c_str());
        ImGui::InputText("Comment", cbuf, sizeof cbuf);
        if (ImGui::IsItemDeactivatedAfterEdit()) setNodeComment(cbuf);
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Blackboard decorators");
    if (asset_.schema.keys.empty())
        ImGui::TextDisabled("Add keys in the Blackboard tab first.");
    for (usize di = 0; di < n.decorators.size(); ++di)
        if (drawDecoratorEditor(di)) break;   // the list changed under us
    if (ImGui::Button("Add decorator")) addDecorator();

    if (selected_ != 0) {
        ImGui::Separator();
        const i32 parent = n.node.parent;
        const std::string current = std::string(synapse::btNodeKindName(asset_.nodes[static_cast<usize>(parent)].node.kind)) +
                                    " #" + std::to_string(parent);
        if (ImGui::BeginCombo("Attach to", current.c_str())) {
            for (i32 i = 0; i < static_cast<i32>(asset_.nodes.size()); ++i) {
                bool descendant = false;
                for (i32 w = i; w != fmt::kOcBtNoParent; w = asset_.nodes[static_cast<usize>(w)].node.parent)
                    if (w == selected_) { descendant = true; break; }
                if (descendant) continue;
                const std::string label = std::string(synapse::btNodeKindName(asset_.nodes[static_cast<usize>(i)].node.kind)) +
                                          " #" + std::to_string(i);
                if (ImGui::Selectable(label.c_str(), i == parent)) { reparentSelected(i); break; }
            }
            ImGui::EndCombo();
        }
        ImGui::BeginDisabled(!synapse::btAssetCanMoveSibling(asset_, selected_, -1));
        if (ImGui::Button("Move earlier")) moveSelected(-1);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!synapse::btAssetCanMoveSibling(asset_, selected_, 1));
        if (ImGui::Button("Move later")) moveSelected(1);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("(order is priority)");
    }
}

void BtGraphEditor::drawBlackboardPanel() {
    const LiveRefs live = liveRefs(debugEntity_, asset_.nodes.size());
    drawSchemaPanel(asset_.schema, live.board, liveBoard_, schemaState_, *this);
}

void BtGraphEditor::drawDebugPanel() {
#if AVER_BTGE_LIVE
    std::vector<u64> ids{fnv1a64(path_)};
    if (const std::string rel = aiRelativeAssetPath(path_); !rel.empty()) ids.push_back(fnv1a64(rel));
    drawEntityPicker(debugEntity_, ids, "tree");

    const LiveRefs live = liveRefs(debugEntity_, asset_.nodes.size());
    if (debugEntity_ == 0) {
        ImGui::TextWrapped("Pick an entity while the game runs (Play) to see its active nodes, last result per node "
                           "and blackboard values here and on the canvas.");
        return;
    }
    if (!live.valid) { ImGui::TextDisabled("That entity has no behaviour tree right now."); return; }
    if (!live.sameTree)
        ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.2f, 1.0f),
                           "This entity runs a different tree (or an older version of this file): nodes are not shown.");

    synapse::BtStatus st = static_cast<synapse::BtStatus>(live.lastStatus < 0 ? 2 : live.lastStatus);
    ImGui::Text("Root result:");
    ImGui::SameLine();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(statusColor(st)), "%s", statusText(st));
    ImGui::Text("Ticks: %llu    Observer aborts: %u", static_cast<unsigned long long>(live.tick), live.aborts);
    if (live.lastAborted >= 0 && static_cast<usize>(live.lastAborted) < asset_.nodes.size())
        ImGui::Text("Last aborted: #%d %s", live.lastAborted,
                    btNodeTitle(asset_.nodes[static_cast<usize>(live.lastAborted)]).c_str());

    ImGui::Separator();
    ImGui::TextUnformatted("Running now");
    bool any = false;
    for (usize i = 0; live.trace && i < live.traceCount; ++i) {
        const synapse::BtNodeTrace& t = live.trace[i];
        if (!t.visited || t.tick != live.tick || t.status != synapse::BtStatus::Running) continue;
        any = true;
        if (ImGui::Selectable((std::string("#") + std::to_string(i) + "  " + btNodeTitle(asset_.nodes[i])).c_str(),
                              selected_ == static_cast<i32>(i)))
            selected_ = static_cast<i32>(i);
    }
    if (!any) ImGui::TextDisabled("nothing (or the tree is idle)");
#else
    ImGui::TextWrapped("Live debugging needs a build with the AI scene module (Aver.Synapse.Scene).");
#endif
}

void BtGraphEditor::draw(Engine& e) {
    (void)e;
    if (!loaded_) {
        ImGui::TextWrapped("This file could not be read: %s", loadError_.c_str());
        return;
    }

    // Flash keys that changed since the last frame (live board only).
    liveBoard_.update(liveRefs(debugEntity_, asset_.nodes.size()).board, ImGui::GetIO().DeltaTime);

    drawToolbar();
    ImGui::Separator();

    const f32 dpi = ImGui::GetFontSize() / 16.0f;
    const f32 avail = ImGui::GetContentRegionAvail().x;
    const f32 minCanvas = 240.0f * dpi, minSide = 260.0f * dpi;
    const f32 canvasW = splitPaneWidth(split_, kPrefSplit, kDefaultCanvasFraction, avail, minCanvas, minSide);
    drawCanvas(canvasW);
    drawSplitHandle(split_, "##btgsplit", kPrefSplit, avail, minCanvas, minSide, 6.0f * dpi);
    if (ImGui::BeginChild("##btgside", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        if (ImGui::BeginTabBar("##btgtabs")) {
            if (ImGui::BeginTabItem("Node")) { drawNodePanel(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Blackboard")) { drawBlackboardPanel(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Debug")) { drawDebugPanel(); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    }
    ImGui::EndChild();
}

void BtGraphEditor::resetLayout() {
    resetSplitPane(split_, kPrefSplit, kDefaultCanvasFraction);
    view_.panX = 24.0f; view_.panY = 24.0f; view_.zoom = 1.0f;
}

#else   // AVER_WITH_IMGUI

void BtGraphEditor::draw(Engine& e) { (void)e; }
void BtGraphEditor::resetLayout() { view_.panX = 24.0f; view_.panY = 24.0f; view_.zoom = 1.0f; }

#endif  // AVER_WITH_IMGUI

} // namespace aver::editor
