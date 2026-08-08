// The .ocgraph node editor tab: ImGui canvas on top of GraphEditorGeometry's headless core.
//
// LOAD/SAVE CONTRACT (the two hard requirements from the task brief):
//   1. A load -> save with no edits is byte-identical. Guaranteed structurally, not by luck: graph_
//      is the exact OcGraphData parseOcgraph() produced, and it is mutated ONLY inside the explicit
//      edit paths below (each preceded by pushUndo()). Auto-layout and canvas display never touch it
//      -- see displayPos_'s doc comment on GraphEditor.hpp. save() always calls
//      fmt::writeOcgraph(graph_, originalText_), matching testRoundTrip's own assertion
//      (tests/formats/src/OcGraphTest.cpp:149) that a write of an untouched parse reproduces its
//      input byte for byte.
//   2. Unknown records survive a save. This editor NEVER calls fmt::saveOcgraph() -- that function
//      (modules/formats/src/OcGraph.cpp:376-386) always writes fresh with no `existing` text and
//      would silently drop anything this editor doesn't model, exactly the docs/VISUAL_SCRIPTING.md
//      trap already documented for .ocworld. Every write here goes through save() below, which reads
//      writeOcgraph(graph_, originalText_) and writes THAT string, so any unrecognised line the
//      parser skipped (a comment, a future record type) rides through untouched, the same guarantee
//      testUnknownRecords exercises (tests/formats/src/OcGraphTest.cpp:183).
#include "GraphEditor.hpp"
#include "GraphNodeDefs.hpp"

#include "aver/core/Log.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

#if AVER_WITH_IMGUI
#  include "imgui.h"
#endif

namespace aver::editor {

namespace {
float g_graphEditorDpi = 1.0f;

f32 vecLen(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }

// Everything below is ImGui-typed (ImVec2/ImU32) and used only from draw(), which is itself entirely
// behind `#if AVER_WITH_IMGUI`. Guarding these too is what lets GraphEditorLoadSaveTest compile this
// translation unit with no ImGui headers and no ImGui context -- see that test's own file comment.
#if AVER_WITH_IMGUI
inline ImVec2 toIm(Vec2 v) { return ImVec2(v.x, v.y); }
inline Vec2 fromIm(ImVec2 v) { return Vec2(v.x, v.y); }

// Pin/link colour by declared type. Falls back to a neutral grey for anything unrecognised -- a type
// this editor has never seen must still draw, not vanish or assert.
ImU32 colorForType(const std::string& type) {
    std::string t = type;
    for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (t == "float")  return IM_COL32(120, 200, 255, 255);
    if (t == "int")    return IM_COL32(140, 220, 140, 255);
    if (t == "bool")   return IM_COL32(230, 150, 90, 255);
    if (t == "string") return IM_COL32(210, 140, 230, 255);
    return IM_COL32(190, 190, 190, 255);
}
#endif

} // namespace

void setGraphEditorDpi(float dpi) { g_graphEditorDpi = dpi; }

// ================================================================================== construction ===

GraphEditor::GraphEditor(std::string path) : path_(std::move(path)) {
    loadFromDisk();
}

void GraphEditor::loadFromDisk() {
    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        loaded_ = false;
        loadError_ = "could not open " + path_;
        return;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    originalText_ = ss.str();

    fmt::OcGraphData g;
    std::string err;
    if (!fmt::parseOcgraph(originalText_, g, &err)) {
        loaded_ = false;
        loadError_ = err.empty() ? "failed to parse .ocgraph" : err;
        return;
    }

    graph_ = std::move(g);
    loaded_ = true;
    loadError_.clear();
    dirty_ = false;

    // Before layout: a node's box height comes from its pin count, so filling the pins in afterwards
    // would space the graph for nodes smaller than the ones actually drawn.
    synthesizeMissingPins();

    displayPos_.clear();
    for (const auto& n : graph_.nodes)
        displayPos_[n.id] = Vec2(static_cast<f32>(n.x), static_cast<f32>(n.y));
    runAutoLayoutIfUnpositioned();

    selectedNodes_.clear();
    selectedLink_ = -1;
    undoStack_.clear();
    redoStack_.clear();
    dragMode_ = DragMode::None;
    view_ = CanvasTransform{};
}

// Auto-layout for a graph with no meaningful stored positions. Triggers only when EVERY node sits at
// exactly (0,0) -- the shape a hand-written file has when nobody bothered with NODE's x/y, since the
// parser requires x on every NODE line (modules/formats/src/OcGraph.cpp:78-81) but a human just types
// "0 0" or omits y. A file with one deliberate node at the origin and others placed elsewhere is left
// alone. See GraphEditor.hpp's displayPos_ comment: this writes ONLY to the display overlay.
// Gives every node the pins its TYPE implies but the file did not write down. See the
// synthesizedPins_ comment in the header for why a valid .ocgraph can be missing them.
//
// Matched on name AND direction: a node may legitimately carry an input and an output sharing a
// name, and treating those as one would leave a real pin unsynthesised while thinking it was there.
void GraphEditor::synthesizeMissingPins() {
    synthesizedPins_.clear();
    for (auto& n : graph_.nodes) {
        const GraphNodeDesc* desc = findGraphNodeDesc(n.type);
        if (!desc) continue;   // a type this build does not know: leave it exactly as written
        for (const GraphPinSpec& spec : desc->pins) {
            bool present = false;
            for (const auto& p : n.pins)
                if (p.isOutput == spec.isOutput && p.name == spec.name) { present = true; break; }
            if (present) continue;
            fmt::OcGraphPin p;
            p.name = spec.name;
            p.type = spec.type;
            p.isOutput = spec.isOutput;
            p.defaultValue = spec.defaultValue;
            n.pins.push_back(std::move(p));
            synthesizedPins_.insert(pinKey(n.id, spec.name, spec.isOutput));
        }
    }
}

void GraphEditor::runAutoLayoutIfUnpositioned() {
    if (graph_.nodes.empty()) return;
    bool allOrigin = true;
    for (const auto& n : graph_.nodes) {
        if (n.x != 0.0 || n.y != 0.0) { allOrigin = false; break; }
    }
    if (!allOrigin) return;
    // THE SAME SCALE recomputeLayouts() draws at, not 1.0. Node boxes are sized at DPI; spacing them
    // as if DPI were 1 packs the columns three times too tightly on a 300% display, so every node
    // lands on top of its neighbour and the titles clip. Layout and drawing have to agree on how big
    // a node is, and the only way to guarantee that is to hand both the same number.
    autoLayoutDpi_ = g_graphEditorDpi;
    autoLaidOut_ = true;
    const auto pos = autoLayoutPositions(graph_, style_, autoLayoutDpi_);
    for (const auto& kv : pos) displayPos_[kv.first] = kv.second;
}

std::string GraphEditor::title() const {
    return std::filesystem::path(path_).filename().string() + "  [Graph]";
}

// ===================================================================================== load/save ===

bool GraphEditor::save(std::string* why) {
    if (!loaded_) {
        if (why) *why = "cannot save: file failed to load (" + loadError_ + ")";
        return false;
    }
    // Strip the pins synthesizeMissingPins() invented, so a save writes what the author wrote plus
    // their edits -- not forty PIN records the editor made up to have something to draw wires to.
    fmt::OcGraphData toWrite = graph_;
    if (!synthesizedPins_.empty()) {
        for (auto& n : toWrite.nodes) {
            n.pins.erase(std::remove_if(n.pins.begin(), n.pins.end(),
                                        [&](const fmt::OcGraphPin& p) {
                                            return synthesizedPins_.count(pinKey(n.id, p.name, p.isOutput)) != 0;
                                        }),
                         n.pins.end());
        }
    }
    // See the file-header comment: writeOcgraph(graph_, originalText_), never saveOcgraph(), is what
    // preserves unrecognised records and keeps a no-op round trip byte-identical.
    const std::string text = fmt::writeOcgraph(toWrite, originalText_);

    std::error_code ec;
    const std::filesystem::path p(path_);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(path_, std::ios::binary | std::ios::trunc);
    if (!f) {
        if (why) *why = "could not open " + path_ + " for writing";
        return false;
    }
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) {
        if (why) *why = "write failed for " + path_;
        return false;
    }
    originalText_ = text; // the next save merges against what is now actually on disk
    dirty_ = false;
    return true;
}

void GraphEditor::onFileChanged() {
    if (dirty_) return; // never clobber unsaved edits behind the user's back
    loadFromDisk();
}

// ======================================================================================= undo/redo =

void GraphEditor::pushUndo() {
    UndoState s;
    s.graph = graph_;
    s.displayPos = displayPos_;
    undoStack_.push_back(std::move(s));
    constexpr usize kUndoCap = 200;
    if (undoStack_.size() > kUndoCap) undoStack_.erase(undoStack_.begin());
    redoStack_.clear();
}

void GraphEditor::undo() {
    if (undoStack_.empty()) return;
    UndoState redoEntry{graph_, displayPos_};
    redoStack_.push_back(std::move(redoEntry));
    UndoState s = std::move(undoStack_.back());
    undoStack_.pop_back();
    graph_ = std::move(s.graph);
    displayPos_ = std::move(s.displayPos);
    dirty_ = true;
    selectedNodes_.clear();
    selectedLink_ = -1;
}

void GraphEditor::redo() {
    if (redoStack_.empty()) return;
    UndoState undoEntry{graph_, displayPos_};
    undoStack_.push_back(std::move(undoEntry));
    UndoState s = std::move(redoStack_.back());
    redoStack_.pop_back();
    graph_ = std::move(s.graph);
    displayPos_ = std::move(s.displayPos);
    dirty_ = true;
    selectedNodes_.clear();
    selectedLink_ = -1;
}

// ========================================================================================= edits ===

std::string GraphEditor::makeUniqueNodeId(const std::string& typeId) const {
    std::string base = typeId;
    for (char& c : base) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (int i = 1; i < 1000000; ++i) {
        const std::string candidate = base + std::to_string(i);
        bool taken = false;
        for (const auto& n : graph_.nodes) {
            if (n.id == candidate) { taken = true; break; }
        }
        if (!taken) return candidate;
    }
    return base + "_x"; // unreachable in practice
}

void GraphEditor::deleteSelection() {
    if (selectedNodes_.empty() && selectedLink_ < 0) return;
    pushUndo();

    // Links touching a deleted node must go too, or the file would ship a dangling reference the
    // parser's own LINK validation (modules/formats/src/OcGraph.cpp:166-178) would refuse to reload.
    auto touchesSelection = [&](const fmt::OcGraphLink& l) {
        for (const auto& id : selectedNodes_)
            if (l.sourceNode == id || l.destNode == id) return true;
        return false;
    };
    std::vector<fmt::OcGraphLink> keptLinks;
    keptLinks.reserve(graph_.links.size());
    for (usize i = 0; i < graph_.links.size(); ++i) {
        if (static_cast<int>(i) == selectedLink_) continue;
        if (touchesSelection(graph_.links[i])) continue;
        keptLinks.push_back(graph_.links[i]);
    }
    graph_.links = std::move(keptLinks);

    if (!selectedNodes_.empty()) {
        std::vector<fmt::OcGraphNode> keptNodes;
        keptNodes.reserve(graph_.nodes.size());
        for (auto& n : graph_.nodes) {
            const bool isSelected = std::find(selectedNodes_.begin(), selectedNodes_.end(), n.id) != selectedNodes_.end();
            if (isSelected) { displayPos_.erase(n.id); continue; }
            keptNodes.push_back(std::move(n));
        }
        graph_.nodes = std::move(keptNodes);
    }

    selectedNodes_.clear();
    selectedLink_ = -1;
    dirty_ = true;
}

void GraphEditor::commitLink(const std::string& srcNode, const std::string& srcPin,
                              const std::string& dstNode, const std::string& dstPin) {
    const GraphLinkCheck check = canConnectPins(graph_, srcNode, srcPin, dstNode, dstPin);
    if (!check.ok) {
        reportLinkRejection(check);
        return;
    }
    pushUndo();
    fmt::OcGraphLink link;
    link.sourceNode = srcNode; link.sourcePin = srcPin;
    link.destNode = dstNode; link.destPin = dstPin;
    graph_.links.push_back(link);
    dirty_ = true;
}

void GraphEditor::reportLinkRejection(const GraphLinkCheck& check) {
    lastRejectMsg_ = check.message.empty() ? "connection refused" : check.message;
#if AVER_WITH_IMGUI
    lastRejectAtSec_ = ImGui::GetTime();
#endif
    AVER_WARN("[GraphEditor] link refused: {}", lastRejectMsg_);
}

// ======================================================================================== layout ===

void GraphEditor::recomputeLayouts(float dpi) {
    layouts_.clear();
    layouts_.reserve(graph_.nodes.size());
    for (const auto& n : graph_.nodes) {
        fmt::OcGraphNode display = n; // layout reads position from the node it's given; substitute
                                       // the DISPLAY position so a live drag (which only touches
                                       // displayPos_, see below) affects drawing without touching
                                       // graph_ until the drag commits.
        auto it = displayPos_.find(n.id);
        if (it != displayPos_.end()) { display.x = it->second.x; display.y = it->second.y; }
        const GraphNodeDesc* desc = findGraphNodeDesc(n.type);
        const std::string title = desc ? desc->displayName : n.type;
        // `dpi` only -- NOT dpi*zoom. Canvas-space geometry is computed once at a fixed logical
        // scale (DPI only); CanvasTransform::zoom is applied uniformly afterwards, at the point
        // canvas coordinates are converted to screen coordinates (canvasToScreen) and back
        // (screenToCanvas, used to convert the mouse position before hit-testing). Folding zoom into
        // `scale` here AS WELL would double-apply it -- node boxes would grow twice as fast as the
        // pins drawn at their corners. See draw()'s canvasToScreen calls for the other half of this.
        layouts_.push_back(computeNodeLayout(display, title, style_, dpi));
    }
}

// ========================================================================================== draw ===

void GraphEditor::draw(Engine&) {
#if AVER_WITH_IMGUI
    const float dpi = g_graphEditorDpi;
    // An auto-laid-out graph is spaced for one DPI. If that changed since -- the asset opened before
    // applyDpi ran, or the window moved to a differently-scaled monitor -- redo it, or the spacing
    // and the boxes disagree and the columns collide. Only auto-layout is redone: positions the user
    // dragged, or that came from the file, are theirs and are left exactly where they are.
    if (autoLaidOut_ && dpi != autoLayoutDpi_) {
        autoLayoutDpi_ = dpi;
        const auto pos = autoLayoutPositions(graph_, style_, dpi);
        for (const auto& kv : pos) displayPos_[kv.first] = kv.second;
    }

    if (!loaded_) {
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "This file could not be read.");
        ImGui::Separator();
        ImGui::TextWrapped("%s", loadError_.c_str());
        return;
    }

    // ---- toolbar --------------------------------------------------------------------------------
    if (ImGui::Button("Save") || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                                   ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))) {
        std::string why;
        if (!save(&why)) AVER_ERROR("[GraphEditor] save failed for '{}': {}", path_, why);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(undoStack_.empty());
    if (ImGui::Button("Undo")) undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(redoStack_.empty());
    if (ImGui::Button("Redo")) redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s  |  %zu nodes, %zu links  |  zoom %.0f%%",
                         path_.c_str(), graph_.nodes.size(), graph_.links.size(), view_.zoom * 100.0f);

    recomputeLayouts(dpi);

    // ---- canvas -----------------------------------------------------------------------------------
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("##graphCanvas", ImVec2(avail.x, std::max(avail.y, 80.0f * dpi)), true,
                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const ImVec2 originIm = ImGui::GetCursorScreenPos();
    const Vec2 origin = fromIm(originIm);
    const ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##graphCanvasHit", ImVec2(std::max(canvasSize.x, 1.0f), std::max(canvasSize.y, 1.0f)),
                            ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                            ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool canvasFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) || hovered;

    ImGuiIO& io = ImGui::GetIO();
    const Vec2 mouseScreen = fromIm(io.MousePos) - origin; // canvas-local screen space
    const Vec2 mouseCanvas = screenToCanvas(view_, mouseScreen);

    auto toScreenAbs = [&](Vec2 canvasPt) -> ImVec2 {
        const Vec2 s = canvasToScreen(view_, canvasPt);
        return ImVec2(originIm.x + s.x, originIm.y + s.y);
    };

    // ---- background grid ----------------------------------------------------------------------
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(originIm, ImVec2(originIm.x + canvasSize.x, originIm.y + canvasSize.y), IM_COL32(30, 30, 34, 255));
    {
        const f32 gridStep = 64.0f * dpi * view_.zoom;
        if (gridStep > 6.0f) { // don't draw a fog of lines when zoomed far out
            const f32 startX = std::fmod(view_.panPx.x, gridStep);
            const f32 startY = std::fmod(view_.panPx.y, gridStep);
            for (f32 x = startX; x < canvasSize.x; x += gridStep)
                dl->AddLine(ImVec2(originIm.x + x, originIm.y), ImVec2(originIm.x + x, originIm.y + canvasSize.y), IM_COL32(255, 255, 255, 12));
            for (f32 y = startY; y < canvasSize.y; y += gridStep)
                dl->AddLine(ImVec2(originIm.x, originIm.y + y), ImVec2(originIm.x + canvasSize.x, originIm.y + y), IM_COL32(255, 255, 255, 12));
        }
    }

    // ---- input: start a new interaction ---------------------------------------------------------
    if (hovered && dragMode_ == DragMode::None) {
        const bool spacePan = io.KeyShift == false && ImGui::IsKeyDown(ImGuiKey_Space);
        if ((spacePan && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
            dragMode_ = DragMode::PanCanvas;
            dragStartScreen_ = mouseScreen;
            panAnchorPx_ = view_.panPx;
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            pendingSpawnCanvasPos_ = mouseCanvas;
            ImGui::OpenPopup("##graphAddNode");
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const GraphHitResult hit = hitTest(graph_, layouts_, mouseCanvas, style_, dpi);
            const bool ctrl = io.KeyCtrl;
            if (hit.kind == GraphHitKind::Pin) {
                dragMode_ = DragMode::DrawLink;
                linkDragFromNode_ = hit.nodeId;
                linkDragFromPin_ = hit.pinName;
                linkDragFromIsOutput_ = hit.pinIsOutput;
            } else if (hit.kind == GraphHitKind::Node) {
                const bool already = std::find(selectedNodes_.begin(), selectedNodes_.end(), hit.nodeId) != selectedNodes_.end();
                if (ctrl) {
                    if (already) selectedNodes_.erase(std::remove(selectedNodes_.begin(), selectedNodes_.end(), hit.nodeId), selectedNodes_.end());
                    else selectedNodes_.push_back(hit.nodeId);
                } else if (!already) {
                    selectedNodes_ = {hit.nodeId};
                }
                selectedLink_ = -1;
                dragMode_ = DragMode::MoveNodes;
                dragStartCanvas_ = mouseCanvas;
                moveStart_.clear();
                for (const auto& id : selectedNodes_) {
                    auto it = displayPos_.find(id);
                    moveStart_[id] = it != displayPos_.end() ? it->second : Vec2{};
                }
                pendingMoveSnapshot_ = UndoState{graph_, displayPos_};
                moveUndoPushed_ = false;
            } else if (hit.kind == GraphHitKind::Link) {
                if (!ctrl) selectedNodes_.clear();
                selectedLink_ = static_cast<int>(hit.linkIndex);
            } else {
                if (!ctrl) { selectedNodes_.clear(); selectedLink_ = -1; }
                dragMode_ = DragMode::BoxSelect;
                dragStartCanvas_ = mouseCanvas;
                boxSelectCurrentCanvas_ = mouseCanvas;
            }
        }
    }

    // ---- input: continue / end the active interaction --------------------------------------------
    switch (dragMode_) {
    case DragMode::PanCanvas: {
        const bool stillDown = ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Middle);
        if (stillDown) {
            view_.panPx = panAnchorPx_ + (mouseScreen - dragStartScreen_);
        } else {
            dragMode_ = DragMode::None;
        }
        break;
    }
    case DragMode::MoveNodes: {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const Vec2 delta = mouseCanvas - dragStartCanvas_;
            if (!moveUndoPushed_ && vecLen(delta) * view_.zoom > 3.0f) {
                undoStack_.push_back(pendingMoveSnapshot_);
                constexpr usize kUndoCap = 200;
                if (undoStack_.size() > kUndoCap) undoStack_.erase(undoStack_.begin());
                redoStack_.clear();
                moveUndoPushed_ = true;
            }
            for (const auto& id : selectedNodes_) displayPos_[id] = moveStart_[id] + delta;
        } else {
            if (moveUndoPushed_) {
                for (const auto& id : selectedNodes_) {
                    for (auto& n : graph_.nodes) {
                        if (n.id == id) {
                            const Vec2 p = displayPos_[id];
                            n.x = p.x; n.y = p.y;
                            break;
                        }
                    }
                }
                dirty_ = true;
            }
            dragMode_ = DragMode::None;
        }
        break;
    }
    case DragMode::BoxSelect: {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            boxSelectCurrentCanvas_ = mouseCanvas;
        } else {
            const Vec2 lo(std::min(dragStartCanvas_.x, boxSelectCurrentCanvas_.x), std::min(dragStartCanvas_.y, boxSelectCurrentCanvas_.y));
            const Vec2 hi(std::max(dragStartCanvas_.x, boxSelectCurrentCanvas_.x), std::max(dragStartCanvas_.y, boxSelectCurrentCanvas_.y));
            for (const auto& nl : layouts_) {
                const bool intersects = nl.min.x <= hi.x && nl.max.x >= lo.x && nl.min.y <= hi.y && nl.max.y >= lo.y;
                if (intersects && std::find(selectedNodes_.begin(), selectedNodes_.end(), nl.nodeId) == selectedNodes_.end())
                    selectedNodes_.push_back(nl.nodeId);
            }
            dragMode_ = DragMode::None;
        }
        break;
    }
    case DragMode::DrawLink: {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const GraphHitResult hit = hitTest(graph_, layouts_, mouseCanvas, style_, dpi);
            if (hit.kind == GraphHitKind::Pin && !(hit.nodeId == linkDragFromNode_ && hit.pinName == linkDragFromPin_)) {
                if (linkDragFromIsOutput_) commitLink(linkDragFromNode_, linkDragFromPin_, hit.nodeId, hit.pinName);
                else                       commitLink(hit.nodeId, hit.pinName, linkDragFromNode_, linkDragFromPin_);
            }
            dragMode_ = DragMode::None;
        }
        break;
    }
    case DragMode::None: default: break;
    }

    // ---- wheel zoom, centred on the cursor ---------------------------------------------------------
    if (hovered && io.MouseWheel != 0.0f) {
        const f32 newZoom = std::clamp(view_.zoom * std::pow(1.1f, io.MouseWheel), 0.15f, 4.0f);
        view_ = zoomAroundScreenPoint(view_, newZoom, mouseScreen);
    }

    // ---- keyboard: delete selection, undo/redo -----------------------------------------------------
    if (canvasFocused) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) deleteSelection();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) { if (io.KeyShift) redo(); else undo(); }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) redo();
    }

    // ---- draw: links underneath, nodes on top -- ImDrawListSplitter, not submission order ----------
    // (submission order would put every link over whichever node happened to draw after it; the
    // splitter is exactly what it exists for, per third_party/imgui/imgui.h's own recommendation).
    dl->ChannelsSplit(2);

    dl->ChannelsSetCurrent(0); // links
    for (usize i = 0; i < graph_.links.size(); ++i) {
        const auto& link = graph_.links[i];
        const GraphPinLayout* sp = findPinLayout(layouts_, link.sourceNode, link.sourcePin);
        const GraphPinLayout* dp = findPinLayout(layouts_, link.destNode, link.destPin);
        if (!sp || !dp) continue; // a link to a pin this file's PIN records never declared; draw nothing rather than guess
        const GraphBezier b = linkBezier(sp->pos, dp->pos);
        const bool selected = (static_cast<int>(i) == selectedLink_);
        const ImU32 col = selected ? IM_COL32(255, 220, 90, 255) : colorForType(sp->type);
        dl->AddBezierCubic(toScreenAbs(b.p1), toScreenAbs(b.p2), toScreenAbs(b.p3), toScreenAbs(b.p4),
                            col, (selected ? 3.0f : 2.0f) * dpi, 24);
    }

    dl->ChannelsSetCurrent(1); // nodes
    for (const auto& nl : layouts_) {
        const bool selected = std::find(selectedNodes_.begin(), selectedNodes_.end(), nl.nodeId) != selectedNodes_.end();
        const ImVec2 pMin = toScreenAbs(nl.min);
        const ImVec2 pMax = toScreenAbs(nl.max);
        const f32 headerH = style_.headerHeightPx * dpi * view_.zoom;

        dl->AddRectFilled(pMin, pMax, IM_COL32(52, 52, 58, 235), 4.0f * dpi);
        dl->AddRectFilled(pMin, ImVec2(pMax.x, pMin.y + headerH), IM_COL32(70, 90, 120, 255), 4.0f * dpi, ImDrawFlags_RoundCornersTop);
        dl->AddRect(pMin, pMax, selected ? IM_COL32(255, 220, 90, 255) : IM_COL32(15, 15, 18, 255), 4.0f * dpi, 0, selected ? 2.5f * dpi : 1.0f * dpi);

        const fmt::OcGraphNode* srcNode = nullptr;
        for (const auto& n : graph_.nodes) if (n.id == nl.nodeId) { srcNode = &n; break; }
        const GraphNodeDesc* desc = srcNode ? findGraphNodeDesc(srcNode->type) : nullptr;
        const std::string label = desc ? desc->displayName : (srcNode ? srcNode->type : nl.nodeId);
        dl->PushClipRect(pMin, ImVec2(pMax.x, pMin.y + headerH), true);
        dl->AddText(ImVec2(pMin.x + 6.0f * dpi, pMin.y + 3.0f * dpi), IM_COL32(255, 255, 255, 255), label.c_str());
        dl->PopClipRect();

        for (const auto& pl : nl.pins) {
            const ImVec2 dot = toScreenAbs(pl.pos);
            const f32 r = style_.pinRadiusPx * dpi * view_.zoom;
            const bool isLinkEnd = (dragMode_ == DragMode::DrawLink && nl.nodeId == linkDragFromNode_ && pl.name == linkDragFromPin_);
            dl->AddCircleFilled(dot, r, colorForType(pl.type));
            if (isLinkEnd) dl->AddCircle(dot, r + 2.0f * dpi, IM_COL32(255, 220, 90, 255), 0, 2.0f * dpi);
            const ImVec2 textSize = ImGui::CalcTextSize(pl.name.c_str());
            const f32 tx = pl.isOutput ? dot.x - textSize.x - r - 3.0f * dpi : dot.x + r + 3.0f * dpi;
            dl->AddText(ImVec2(tx, dot.y - textSize.y * 0.5f), IM_COL32(220, 220, 220, 255), pl.name.c_str());
        }
    }
    dl->ChannelsMerge();

    // ---- overlays: in-progress link drag, box-select rect, rejection banner (drawn AFTER merge, so
    // always on top regardless of channel) ----------------------------------------------------------
    if (dragMode_ == DragMode::DrawLink) {
        const GraphPinLayout* from = findPinLayout(layouts_, linkDragFromNode_, linkDragFromPin_);
        if (from) {
            const Vec2 a = linkDragFromIsOutput_ ? from->pos : mouseCanvas;
            const Vec2 b = linkDragFromIsOutput_ ? mouseCanvas : from->pos;
            const GraphBezier bez = linkBezier(a, b);
            dl->AddBezierCubic(toScreenAbs(bez.p1), toScreenAbs(bez.p2), toScreenAbs(bez.p3), toScreenAbs(bez.p4),
                                IM_COL32(255, 255, 255, 180), 2.0f * dpi, 24);
        }
    }
    if (dragMode_ == DragMode::BoxSelect) {
        const ImVec2 a = toScreenAbs(dragStartCanvas_);
        const ImVec2 b = toScreenAbs(boxSelectCurrentCanvas_);
        dl->AddRectFilled(ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)), ImVec2(std::max(a.x, b.x), std::max(a.y, b.y)), IM_COL32(120, 160, 255, 40));
        dl->AddRect(ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)), ImVec2(std::max(a.x, b.x), std::max(a.y, b.y)), IM_COL32(120, 160, 255, 200));
    }
    if (!lastRejectMsg_.empty() && (ImGui::GetTime() - lastRejectAtSec_) < 4.0) {
        const std::string msg = "Connection refused: " + lastRejectMsg_;
        dl->AddRectFilled(originIm, ImVec2(originIm.x + ImGui::CalcTextSize(msg.c_str()).x + 16.0f * dpi, originIm.y + 22.0f * dpi), IM_COL32(90, 25, 25, 220));
        dl->AddText(ImVec2(originIm.x + 8.0f * dpi, originIm.y + 4.0f * dpi), IM_COL32(255, 210, 210, 255), msg.c_str());
    }

    // ---- right-click "add node" palette, built entirely from graphNodeCatalog() -- see
    // GraphNodeDefs.hpp's own header comment for why this is the one place a node type is registered.
    if (ImGui::BeginPopup("##graphAddNode")) {
        std::vector<std::string> categories;
        for (const auto& d : graphNodeCatalog()) {
            if (std::find(categories.begin(), categories.end(), d.category) == categories.end())
                categories.push_back(d.category);
        }
        for (const auto& cat : categories) {
            if (ImGui::BeginMenu(cat.c_str())) {
                for (const auto& d : graphNodeCatalog()) {
                    if (d.category != cat) continue;
                    if (ImGui::MenuItem(d.displayName.c_str())) {
                        pushUndo();
                        fmt::OcGraphNode node;
                        node.id = makeUniqueNodeId(d.typeId);
                        node.type = d.typeId;
                        node.x = pendingSpawnCanvasPos_.x;
                        node.y = pendingSpawnCanvasPos_.y;
                        for (const auto& ps : d.pins)
                            node.pins.push_back(fmt::OcGraphPin{ps.name, ps.type, ps.isOutput, ps.defaultValue});
                        graph_.nodes.push_back(node);
                        displayPos_[node.id] = pendingSpawnCanvasPos_;
                        selectedNodes_ = {node.id};
                        selectedLink_ = -1;
                        dirty_ = true;
                    }
                }
                ImGui::EndMenu();
            }
        }
        ImGui::EndPopup();
    }

    ImGui::EndChild();
#endif
}

// ------------------------------------------------------------------------------------------ factory

std::unique_ptr<AssetEditor> makeGraphEditor(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    for (char& c : ext) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (ext != ".ocgraph") return nullptr;
    return std::make_unique<GraphEditor>(path);
}

} // namespace aver::editor
