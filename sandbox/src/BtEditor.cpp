// The .ocbt behaviour-tree editor tab. See the header for the three-line SandboxApp hook and for
// why the structural edits are free functions rather than members.

#include "BtEditor.hpp"
#include "EditorKeybinds.hpp"

#include "aver/core/Log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <unordered_map>

#if AVER_WITH_IMGUI
#  include "imgui.h"
#endif

namespace aver::editor {
namespace {

// parent index -> its children, in array order. Built from the array and mutated by whichever edit
// is running, then handed to rebuild() -- so every edit is expressed as "change who is whose child,
// in what order" and nothing has to reason about index arithmetic directly.
using ChildOrder = std::unordered_map<i32, std::vector<i32>>;

ChildOrder captureOrder(const std::vector<fmt::OcBtNode>& nodes) {
    ChildOrder order;
    for (i32 i = 0; i < static_cast<i32>(nodes.size()); ++i)
        if (nodes[static_cast<usize>(i)].parent != fmt::kOcBtNoParent)
            order[nodes[static_cast<usize>(i)].parent].push_back(i);
    return order;
}

// Emits `oldIndex` then its children, depth first, recording where each landed.
void emitDfs(const std::vector<fmt::OcBtNode>& oldNodes, const ChildOrder& order, i32 oldIndex,
             i32 newParent, std::vector<fmt::OcBtNode>& out, std::unordered_map<i32, i32>& remap) {
    const i32 newIndex = static_cast<i32>(out.size());
    remap[oldIndex] = newIndex;
    out.push_back(oldNodes[static_cast<usize>(oldIndex)]);
    out.back().parent = newParent;
    const auto it = order.find(oldIndex);
    if (it == order.end()) return;
    for (const i32 child : it->second)
        emitDfs(oldNodes, order, child, newIndex, out, remap);
}

// Rewrites `nodes` in pre-order DFS from node 0, returning old index -> new index.
//
// PRE-ORDER IS WHAT MAKES THIS SAFE: a parent is always emitted before any of its descendants, so
// OcBtData::valid()'s "parent index must be strictly less than the child's own" holds for anything
// this produces, no matter which edit asked for the rebuild. Anything no longer reachable from node
// 0 is simply not emitted -- which is exactly how a delete works, with no separate erase pass.
std::unordered_map<i32, i32> rebuild(std::vector<fmt::OcBtNode>& nodes, const ChildOrder& order) {
    std::vector<fmt::OcBtNode> out;
    out.reserve(nodes.size());
    std::unordered_map<i32, i32> remap;
    if (!nodes.empty()) emitDfs(nodes, order, 0, fmt::kOcBtNoParent, out, remap);
    nodes = std::move(out);
    return remap;
}

bool inRange(const std::vector<fmt::OcBtNode>& nodes, i32 index) {
    return index >= 0 && static_cast<usize>(index) < nodes.size();
}

// Whether `maybeAncestor` is `index` itself or sits above it -- the cycle guard for a reparent,
// mirroring GraphEditor's own componentIsAncestorOf, which exists for the identical reason.
bool isAncestorOf(const std::vector<fmt::OcBtNode>& nodes, i32 maybeAncestor, i32 index) {
    for (i32 walk = index; walk != fmt::kOcBtNoParent && inRange(nodes, walk);
         walk = nodes[static_cast<usize>(walk)].parent)
        if (walk == maybeAncestor) return true;
    return false;
}

// Removes `child` from whichever list holds it.
void detach(ChildOrder& order, const std::vector<fmt::OcBtNode>& nodes, i32 child) {
    const i32 parent = nodes[static_cast<usize>(child)].parent;
    const auto it = order.find(parent);
    if (it == order.end()) return;
    it->second.erase(std::remove(it->second.begin(), it->second.end(), child), it->second.end());
}

} // namespace

const char* btKindName(fmt::OcBtNodeKind kind) {
    switch (kind) {
        case fmt::OcBtNodeKind::Selector:  return "Selector";
        case fmt::OcBtNodeKind::Sequence:  return "Sequence";
        case fmt::OcBtNodeKind::Parallel:  return "Parallel";
        case fmt::OcBtNodeKind::Inverter:  return "Inverter";
        case fmt::OcBtNodeKind::Succeeder: return "Succeeder";
        case fmt::OcBtNodeKind::Cooldown:  return "Cooldown";
        case fmt::OcBtNodeKind::Condition: return "Condition";
        case fmt::OcBtNodeKind::Action:    return "Action";
    }
    return "?";
}

std::vector<i32> btChildrenOf(const std::vector<fmt::OcBtNode>& nodes, i32 parent) {
    std::vector<i32> out;
    for (i32 i = 0; i < static_cast<i32>(nodes.size()); ++i)
        if (nodes[static_cast<usize>(i)].parent == parent) out.push_back(i);
    return out;
}

i32 btAddChild(std::vector<fmt::OcBtNode>& nodes, i32 parent, fmt::OcBtNodeKind kind) {
    if (!inRange(nodes, parent)) return -1;

    fmt::OcBtNode fresh;
    fresh.kind = kind;
    fresh.parent = parent;
    // A NAME FROM BIRTH for the two kinds that require one -- see the header. Both are real
    // registered built-ins (registerBuiltinBehaviors, modules/synapse.scene), so a node dropped into
    // a tree does something meaningful immediately rather than warning "not registered" on its first
    // tick.
    if (kind == fmt::OcBtNodeKind::Condition) fresh.name = "HasTarget";
    else if (kind == fmt::OcBtNodeKind::Action) fresh.name = "Wait";

    const i32 addedOld = static_cast<i32>(nodes.size());
    nodes.push_back(std::move(fresh));

    // Captured AFTER the push, so the new node is already the last child of `parent`.
    const std::unordered_map<i32, i32> remap = rebuild(nodes, captureOrder(nodes));
    const auto it = remap.find(addedOld);
    return it != remap.end() ? it->second : -1;
}

i32 btDeleteSubtree(std::vector<fmt::OcBtNode>& nodes, i32 index) {
    if (!inRange(nodes, index) || index == 0) return -1;   // the root is not deletable

    const i32 parentOld = nodes[static_cast<usize>(index)].parent;
    ChildOrder order = captureOrder(nodes);
    detach(order, nodes, index);
    // Not emitted by the DFS, so the whole subtree simply ceases to exist -- see rebuild().
    const std::unordered_map<i32, i32> remap = rebuild(nodes, order);
    const auto it = remap.find(parentOld);
    return it != remap.end() ? it->second : 0;
}

i32 btReparent(std::vector<fmt::OcBtNode>& nodes, i32 index, i32 newParent) {
    if (!inRange(nodes, index) || !inRange(nodes, newParent)) return -1;
    if (index == 0) return -1;                              // the root has no parent to change
    // A CYCLE IS SILENT DATA LOSS HERE, not merely an invalid tree: rebuild() emits only what is
    // reachable from node 0, so reparenting a node onto its own descendant would drop that whole
    // subtree on the floor -- and the caller would still see a -1 "refused" return, because the
    // remap lookup below then fails to find the node it just destroyed. Verified by removing this
    // line: BtEditorTest's per-call refusal checks all still passed while three nodes vanished, and
    // only its "every refusal left the tree exactly as it was" assertion caught it.
    if (isAncestorOf(nodes, index, newParent)) return -1;

    ChildOrder order = captureOrder(nodes);
    detach(order, nodes, index);
    order[newParent].push_back(index);
    const std::unordered_map<i32, i32> remap = rebuild(nodes, order);
    const auto it = remap.find(index);
    return it != remap.end() ? it->second : -1;
}

// Whether btMoveSibling below would do anything. Shares its reasoning by construction: the move
// calls this first, so a disabled button and a refused move can never disagree about what is
// possible. The two buttons used to be enabled at both ends and silently do nothing there.
bool btCanMoveSibling(const std::vector<fmt::OcBtNode>& nodes, i32 index, i32 delta) {
    if (!inRange(nodes, index) || index == 0 || delta == 0) return false;

    const ChildOrder order = captureOrder(const_cast<std::vector<fmt::OcBtNode>&>(nodes));
    const i32 parent = nodes[static_cast<usize>(index)].parent;
    const auto it = order.find(parent);
    if (it == order.end()) return false;
    const std::vector<i32>& siblings = it->second;

    const auto pos = std::find(siblings.begin(), siblings.end(), index);
    if (pos == siblings.end()) return false;
    const auto at = static_cast<i32>(pos - siblings.begin());
    const i32 want = at + (delta < 0 ? -1 : 1);
    return want >= 0 && want < static_cast<i32>(siblings.size());
}

i32 btMoveSibling(std::vector<fmt::OcBtNode>& nodes, i32 index, i32 delta) {
    if (!btCanMoveSibling(nodes, index, delta)) return -1;

    ChildOrder order = captureOrder(nodes);
    const i32 parent = nodes[static_cast<usize>(index)].parent;
    const auto it = order.find(parent);
    if (it == order.end()) return -1;
    std::vector<i32>& siblings = it->second;

    const auto pos = std::find(siblings.begin(), siblings.end(), index);
    if (pos == siblings.end()) return -1;
    const auto at = static_cast<i32>(pos - siblings.begin());
    const i32 want = at + (delta < 0 ? -1 : 1);
    std::swap(siblings[static_cast<usize>(at)], siblings[static_cast<usize>(want)]);

    const std::unordered_map<i32, i32> remap = rebuild(nodes, order);
    const auto found = remap.find(index);
    return found != remap.end() ? found->second : -1;
}

void btSetKind(std::vector<fmt::OcBtNode>& nodes, i32 index, fmt::OcBtNodeKind kind) {
    if (!inRange(nodes, index)) return;
    fmt::OcBtNode& n = nodes[static_cast<usize>(index)];
    n.kind = kind;
    const bool needsName = kind == fmt::OcBtNodeKind::Condition || kind == fmt::OcBtNodeKind::Action;
    if (needsName && n.name.empty())
        n.name = kind == fmt::OcBtNodeKind::Condition ? "HasTarget" : "Wait";
    else if (!needsName)
        n.name.clear();
}

fmt::OcBtData btStarterTree() {
    fmt::OcBtData bt;

    fmt::OcBtNode root;
    root.kind = fmt::OcBtNodeKind::Selector;
    root.parent = fmt::kOcBtNoParent;
    bt.nodes.push_back(root);

    // Built through btAddChild rather than hand-rolled, so the starter is produced by the same edit
    // path every other node in this editor goes through -- including its choice of a registered name.
    btAddChild(bt.nodes, 0, fmt::OcBtNodeKind::Action);
    if (bt.nodes.size() > 1) bt.nodes[1].params[0] = 1.0f;   // Wait's duration, seconds

    return bt;
}

// ================================================================================== the tab =======

BtEditor::BtEditor(std::string path) : path_(std::move(path)) { loadFromDisk(); }

void BtEditor::loadFromDisk() {
    std::string why;
    fmt::OcBtData loadedTree;
    if (!fmt::loadOcBt(path_, loadedTree, &why)) {
        loaded_ = false;
        loadError_ = why;
        return;
    }
    tree_ = std::move(loadedTree);
    loaded_ = true;
    loadError_.clear();
    dirty_ = false;
    selected_ = 0;
    history_.clear();
}

std::string BtEditor::title() const {
    // No manual dirty marker: the host passes ImGuiWindowFlags_UnsavedDocument for every editor
    // whose dirty() is true (AssetEditor.cpp), so adding a '*' here would double it up.
    return std::filesystem::path(path_).filename().string() + "###bt:" + path_;
}

bool BtEditor::save(std::string* why) {
    if (!loaded_) {
        if (why) *why = "cannot save: file failed to load (" + loadError_ + ")";
        return false;
    }
    if (!fmt::saveOcBt(path_, tree_, why)) return false;
    dirty_ = false;
    return true;
}

void BtEditor::onFileChanged() {
    // A DIRTY TAB KEEPS ITS EDITS. Reloading here would silently discard whatever the user has
    // typed because something else touched the file -- the one outcome an editor must never
    // produce. A clean tab reloads, which is the case this callback is actually useful for (a bake
    // or a tool rewrote the asset while it happened to be open).
    if (dirty_) {
        AVER_WARN("[BtEditor] '{}' changed on disk, but this tab has unsaved edits -- keeping them",
                  path_);
        return;
    }
    loadFromDisk();
}

void BtEditor::pushUndo() {
    history_.push(tree_);
}

void BtEditor::undo() {
    if (!history_.undo(tree_)) return;
    dirty_ = true;
    // The selection is an INDEX, and undo can shrink the array under it.
    if (!inRange(tree_.nodes, selected_)) selected_ = 0;
}

void BtEditor::redo() {
    if (!history_.redo(tree_)) return;
    dirty_ = true;
    if (!inRange(tree_.nodes, selected_)) selected_ = 0;
}

void BtEditor::addChild(fmt::OcBtNodeKind kind) {
    if (!loaded_ || !inRange(tree_.nodes, selected_)) return;
    pushUndo();
    // Added under whatever is selected, matching GraphEditor's own addComponent: building a
    // hierarchy means adding under the thing just clicked, not at the root every time.
    const i32 added = btAddChild(tree_.nodes, selected_, kind);
    if (added < 0) { history_.cancelPush(); return; }
    selected_ = added;
    dirty_ = true;
}

void BtEditor::deleteSelected() {
    if (!loaded_ || selected_ == 0) return;
    pushUndo();
    const i32 next = btDeleteSubtree(tree_.nodes, selected_);
    if (next < 0) { history_.cancelPush(); return; }
    selected_ = next;
    dirty_ = true;
}

void BtEditor::reparentSelected(i32 newParent) {
    if (!loaded_) return;
    pushUndo();
    const i32 moved = btReparent(tree_.nodes, selected_, newParent);
    if (moved < 0) { history_.cancelPush(); return; }
    selected_ = moved;
    dirty_ = true;
}

void BtEditor::moveSelected(i32 delta) {
    if (!loaded_) return;
    pushUndo();
    const i32 moved = btMoveSibling(tree_.nodes, selected_, delta);
    if (moved < 0) { history_.cancelPush(); return; }
    selected_ = moved;
    dirty_ = true;
}

void BtEditor::setSelectedKind(fmt::OcBtNodeKind kind) {
    if (!loaded_ || !inRange(tree_.nodes, selected_)) return;
    if (tree_.nodes[static_cast<usize>(selected_)].kind == kind) return;
    pushUndo();
    btSetKind(tree_.nodes, selected_, kind);
    dirty_ = true;
}

#if AVER_WITH_IMGUI

namespace {
// The seven names registerBuiltinBehaviors installs (modules/synapse.scene/src/SynapseBt.cpp). A
// CONVENIENCE, not a constraint: the name is free text underneath, because a project registers its
// own conditions and actions into the same registry and the editor has no way to know them.
const char* const kBuiltinConditions[] = {"HasTarget", "CanSeeTarget", "DistanceToTargetLess"};
const char* const kBuiltinActions[] = {"MoveTo", "Wait", "LookAt", "FireEvent"};

// The tree/details split -- see EditorWidgets.hpp's own top comment for why this is a FRACTION
// (SplitPane) rather than ActorEditor's pixel-width convention. 0.45f is this tab's own PRE-EXISTING
// default (it used to be `avail.x * 0.45f`, recomputed fresh every frame with no persistence at
// all) -- kept exactly, so adopting the shared helper changes draggability and persistence only.
constexpr f32 kDefaultTreeFraction = 0.45f;
constexpr const char* kPrefTreeSplit = "btEditor.treeSplit";
} // namespace

void BtEditor::drawTreeRow(i32 index) {
    const fmt::OcBtNode& node = tree_.nodes[static_cast<usize>(index)];
    const std::vector<i32> children = btChildrenOf(tree_.nodes, index);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if (children.empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (selected_ == index) flags |= ImGuiTreeNodeFlags_Selected;

    // Condition/Action rows say their NAME, because that is what distinguishes them from each
    // other; a structural row says only its kind, because there is nothing else to say.
    const std::string label = node.name.empty()
        ? std::string(btKindName(node.kind))
        : std::string(btKindName(node.kind)) + "  " + node.name;

    ImGui::PushID(index);
    const bool open = ImGui::TreeNodeEx("##row", flags, "%s", label.c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) selected_ = index;
    ImGui::PopID();

    if (open && !children.empty()) {
        for (const i32 child : children) drawTreeRow(child);
        ImGui::TreePop();
    }
}

void BtEditor::drawDetails() {
    if (!inRange(tree_.nodes, selected_)) { ImGui::TextUnformatted("Nothing selected"); return; }
    fmt::OcBtNode& node = tree_.nodes[static_cast<usize>(selected_)];

    if (ImGui::BeginCombo("Kind", btKindName(node.kind))) {
        for (u32 k = 0; k <= static_cast<u32>(fmt::OcBtNodeKind::Action); ++k) {
            const auto kind = static_cast<fmt::OcBtNodeKind>(k);
            if (ImGui::Selectable(btKindName(kind), kind == node.kind)) setSelectedKind(kind);
        }
        ImGui::EndCombo();
    }

    const bool isLeaf = node.kind == fmt::OcBtNodeKind::Condition ||
                        node.kind == fmt::OcBtNodeKind::Action;
    if (isLeaf) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s", node.name.c_str());
        if (ImGui::InputText("Name", buf, sizeof buf)) { /* committed on release, below */ }
        // COMMITTED ON RELEASE, not per keystroke -- GraphEditor's own IsItemDeactivatedAfterEdit
        // convention, so one rename is one undo entry rather than one per character typed.
        if (ImGui::IsItemDeactivatedAfterEdit() && buf != node.name) {
            pushUndo();
            node.name = buf;
            dirty_ = true;
        }
        // The built-ins, as a shortcut beside the free-text field above.
        const char* const* names = node.kind == fmt::OcBtNodeKind::Condition ? kBuiltinConditions
                                                                            : kBuiltinActions;
        const int count = node.kind == fmt::OcBtNodeKind::Condition ? 3 : 4;
        if (ImGui::BeginCombo("Built-in", "pick...")) {
            for (int i = 0; i < count; ++i) {
                if (ImGui::Selectable(names[i], node.name == names[i])) {
                    pushUndo();
                    node.name = names[i];
                    dirty_ = true;
                }
            }
            ImGui::EndCombo();
        }
    }

    // params[0..3] and stringParam are meaningful only for some kinds, but shown for all of them:
    // a project's OWN registered condition or action reads whichever it likes, and the editor
    // cannot know which. The labels name the built-in meaning where there is one.
    const char* paramHint = "";
    if (node.kind == fmt::OcBtNodeKind::Cooldown) paramHint = " (seconds)";
    else if (node.name == "Wait") paramHint = " (seconds)";
    else if (node.name == "DistanceToTargetLess") paramHint = " (centimetres)";
    else if (node.name == "MoveTo") paramHint = " (goal x, y, z)";

    ImGui::Separator();
    ImGui::Text("Parameters%s", paramHint);
    for (int i = 0; i < 4; ++i) {
        char label[16];
        std::snprintf(label, sizeof label, "params[%d]", i);
        const f32 before = node.params[i];
        ImGui::DragFloat(label, &node.params[i], 0.5f);
        if (ImGui::IsItemDeactivatedAfterEdit() && node.params[i] != before) {
            // Same one-edit-one-undo reasoning as the name field above: the drag itself writes
            // straight into the node every frame, and only the RELEASE records an undo entry.
            const f32 after = node.params[i];
            node.params[i] = before;
            pushUndo();
            node.params[i] = after;
            dirty_ = true;
        }
    }

    if (node.kind == fmt::OcBtNodeKind::Action) {
        char sbuf[128];
        std::snprintf(sbuf, sizeof sbuf, "%s", node.stringParam.c_str());
        ImGui::InputText("String param", sbuf, sizeof sbuf);
        if (ImGui::IsItemDeactivatedAfterEdit() && sbuf != node.stringParam) {
            pushUndo();
            node.stringParam = sbuf;
            dirty_ = true;
        }
        if (node.name == "FireEvent") ImGui::TextUnformatted("FireEvent raises this event name.");
    }

    // Reparent as a COMBO, not drag-and-drop -- GraphEditor's own "Attach to" convention, and the
    // editor has no BeginDragDropSource for a structural edit anywhere.
    if (selected_ != 0) {
        ImGui::Separator();
        const i32 parent = node.parent;
        const std::string current = std::string(btKindName(tree_.nodes[static_cast<usize>(parent)].kind)) +
                                    " #" + std::to_string(parent);
        if (ImGui::BeginCombo("Attach to", current.c_str())) {
            for (i32 i = 0; i < static_cast<i32>(tree_.nodes.size()); ++i) {
                // Never offer this node or one of its own descendants -- that is the cycle guard,
                // shown rather than merely enforced, so the option is not there to be clicked.
                if (isAncestorOf(tree_.nodes, selected_, i)) continue;
                const std::string label = std::string(btKindName(tree_.nodes[static_cast<usize>(i)].kind)) +
                                          " #" + std::to_string(i);
                if (ImGui::Selectable(label.c_str(), i == parent)) reparentSelected(i);
            }
            ImGui::EndCombo();
        }
        // Disabled at the ends, like Undo/Redo/Delete beside them. These two were the only controls
        // in this toolbar that stayed enabled when they could not act.
        ImGui::BeginDisabled(!btCanMoveSibling(tree_.nodes, selected_, -1));
        if (ImGui::Button("Move up")) moveSelected(-1);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!btCanMoveSibling(tree_.nodes, selected_, 1));
        if (ImGui::Button("Move down")) moveSelected(1);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("(order is execution order)");
    }
}

void BtEditor::draw(Engine& e) {
    (void)e;
    if (!loaded_) {
        ImGui::TextWrapped("This file could not be read: %s", loadError_.c_str());
        return;
    }

    if (ImGui::Button("Save") || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                                  keybinds().pressed(CommandId::AssetSave, ImGui::GetIO()))) {
        std::string why;
        if (!save(&why)) AVER_ERROR("[BtEditor] save failed for '{}': {}", path_, why);
    }
    // Ctrl+Z / Ctrl+Y reach the same undo()/redo() the buttons below call: guarded by canUndo()/
    // canRedo() the way the buttons are, and skipped while an InputText has focus -- it has its own
    // Ctrl+Z, and WantTextInput is how GraphEditor.cpp's canvas tells the two apart.
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
    if (ImGui::BeginCombo("##add", "Add child")) {
        for (u32 k = 0; k <= static_cast<u32>(fmt::OcBtNodeKind::Action); ++k) {
            const auto kind = static_cast<fmt::OcBtNodeKind>(k);
            if (ImGui::Selectable(btKindName(kind))) addChild(kind);
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(selected_ == 0);
    if (ImGui::Button("Delete")) deleteSelected();
    ImGui::EndDisabled();

    ImGui::Separator();

    // Draggable, persisted, through the shared SplitPane helper (EditorWidgets.hpp) -- see this
    // file's own kDefaultTreeFraction comment for why 0.45f is not a new number.
    const f32 dpi = ImGui::GetFontSize() / 16.0f;
    const f32 avail = ImGui::GetContentRegionAvail().x;
    const f32 minTree = 140.0f * dpi, minDetails = 200.0f * dpi;
    const f32 paneW = splitPaneWidth(split_, kPrefTreeSplit, kDefaultTreeFraction, avail,
                                      minTree, minDetails);
    if (ImGui::BeginChild("##bttree", ImVec2(paneW, 0), true)) {
        if (!tree_.nodes.empty()) drawTreeRow(0);
    }
    ImGui::EndChild();
    drawSplitHandle(split_, "##btsplit", kPrefTreeSplit, avail, minTree, minDetails, 6.0f * dpi);
    if (ImGui::BeginChild("##btdetails", ImVec2(0, 0), true)) drawDetails();
    ImGui::EndChild();
}

// Restores the tree/details split to its default proportion and persists that immediately -- see
// AssetEditor.hpp's own resetLayout() comment for why "Reset Tab Layout" needs every tab to implement
// this rather than just ActorEditor.
void BtEditor::resetLayout() {
    resetSplitPane(split_, kPrefTreeSplit, kDefaultTreeFraction);
}

#else   // AVER_WITH_IMGUI

// The headless build (and tests/editor's own target, which deliberately leaves AVER_WITH_IMGUI
// undefined -- see tests/editor/CMakeLists.txt) still gets load/save/undo/edits; only the window is
// absent. GraphEditor.cpp does exactly this.
void BtEditor::draw(Engine& e) { (void)e; }

// A headless build never lays the panels out at all, so there is nothing for a reset to restore.
void BtEditor::resetLayout() {}

#endif  // AVER_WITH_IMGUI

std::unique_ptr<AssetEditor> makeBtEditor(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".ocbt") return nullptr;
    return std::make_unique<BtEditor>(path);
}

} // namespace aver::editor
