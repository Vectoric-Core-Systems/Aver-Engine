#include "PcgGraphEditor.hpp"

#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/world/PcgSinks.hpp"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

#if AVER_WITH_IMGUI
#include "EditorIcons.hpp"
#include "imgui.h"
#endif

namespace aver::editor {

namespace fs = std::filesystem;

namespace {

PcgEditorHost& hostRef() {
    static PcgEditorHost h;
    return h;
}

std::vector<PcgGraphEditor*>& openTabs() {
    static std::vector<PcgGraphEditor*> v;
    return v;
}

}  // namespace

void setPcgEditorHost(PcgEditorHost host) { hostRef() = std::move(host); }

PcgGraphEditor::PcgGraphEditor(std::string path) : path_(std::move(path)) {
    openTabs().push_back(this);
    loadFromDisk();
}

PcgGraphEditor::~PcgGraphEditor() {
    openTabs().erase(std::remove(openTabs().begin(), openTabs().end(), this), openTabs().end());
    if (debugOn_) pcgDebugSegments().clear();
}

std::string PcgGraphEditor::title() const { return fs::path(path_).filename().string() + (dirty_ ? " *" : ""); }

void PcgGraphEditor::loadFromDisk() {
    std::ifstream in(path_, std::ios::binary);
    if (!in) { loaded_ = false; loadError_ = "could not open " + path_; return; }
    std::ostringstream ss;
    ss << in.rdbuf();
    originalText_ = ss.str();
    fmt::OcGraphData g;
    std::string err;
    if (!fmt::parseOcgraph(originalText_, g, &err)) { loaded_ = false; loadError_ = err.empty() ? "failed to parse .ocgraph" : err; return; }
    graph_ = std::move(g);
    loaded_ = true;
    loadError_.clear();
    dirty_ = false;
    history_.clear();
    selNode_.clear();
    displayPos_.clear();
    runStale_ = true;
    run_ = pcged::PreviewResult{};
}

void PcgGraphEditor::onFileChanged() {
    if (!dirty_) loadFromDisk();
}

bool PcgGraphEditor::save(std::string* why) {
    // Positions the user never set stay unwritten; only a node that was moved or added carries x y.
    const std::string text = fmt::writeOcgraph(graph_, originalText_);
    std::error_code ec;
    const fs::path p(path_);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    if (!writeFileTextAtomic(path_, text)) {
        if (why) *why = "could not write " + path_;
        return false;
    }
    originalText_ = text;
    dirty_ = false;
    return true;
}

// ---- edits ----
bool PcgGraphEditor::addNode(const std::string& type, f64 x, f64 y, std::string* id) {
    if (!world::pcgFindNode(type)) return false;
    pushUndo();
    std::string made;
    pcged::addNode(graph_, type, x, y, &made);
    displayPos_[made] = Vec2(static_cast<f32>(x), static_cast<f32>(y));
    selNode_ = made;
    dirty_ = true;
    invalidate();
    if (id) *id = made;
    return true;
}

bool PcgGraphEditor::removeSelected() {
    if (selNode_.empty() || !pcged::hasNode(graph_, selNode_)) return false;
    pushUndo();
    pcged::removeNode(graph_, selNode_);
    displayPos_.erase(selNode_);
    selNode_.clear();
    dirty_ = true;
    invalidate();
    return true;
}

bool PcgGraphEditor::linkPins(const std::string& srcNode, const std::string& srcPin, const std::string& dstNode, const std::string& dstPin, std::string* why) {
    pushUndo();
    const GraphLinkCheck c = tryAddLink(graph_, srcNode, srcPin, dstNode, dstPin);
    if (!c.ok) {
        history_.cancelPush();
        if (why) *why = c.message;
        return false;
    }
    dirty_ = true;
    invalidate();
    return true;
}

bool PcgGraphEditor::unlink(usize index) {
    if (index >= graph_.links.size()) return false;
    pushUndo();
    pcged::removeLink(graph_, index);
    dirty_ = true;
    invalidate();
    return true;
}

bool PcgGraphEditor::moveNode(const std::string& id, f64 x, f64 y) {
    for (fmt::OcGraphNode& n : graph_.nodes) {
        if (n.id != id) continue;
        n.x = x; n.y = y; n.hasPosition = true;
        displayPos_[id] = Vec2(static_cast<f32>(x), static_cast<f32>(y));
        dirty_ = true;
        return true;
    }
    return false;
}

bool PcgGraphEditor::setParam(const std::string& nodeId, const std::string& key, const std::string& value) {
    for (fmt::OcGraphNode& n : graph_.nodes) {
        if (n.id != nodeId) continue;
        const std::vector<std::string> before = n.extraTokens;
        fmt::OcGraphNode probe = n;
        pcged::setParam(probe, key, value);
        if (probe.extraTokens == before) return false;
        pushUndo();
        n.extraTokens = std::move(probe.extraTokens);
        dirty_ = true;
        invalidate();
        return true;
    }
    return false;
}

// Undo/redo restore the graph; the canvas draws from displayPos_, so positioned nodes take their restored places (a moved node went back
// in the data but stayed where it was on screen). Unpositioned nodes keep their display-only layout.
void PcgGraphEditor::resyncPositionsFromGraph() {
    for (const fmt::OcGraphNode& n : graph_.nodes)
        if (n.hasPosition) displayPos_[n.id] = Vec2(static_cast<f32>(n.x), static_cast<f32>(n.y));
    for (auto it = displayPos_.begin(); it != displayPos_.end();)
        it = pcged::hasNode(graph_, it->first) ? std::next(it) : displayPos_.erase(it);
}

void PcgGraphEditor::undo() {
    if (!history_.undo(graph_)) return;
    resyncPositionsFromGraph();
    dirty_ = true;
    if (!pcged::hasNode(graph_, selNode_)) selNode_.clear();
    invalidate();
}

void PcgGraphEditor::redo() {
    if (!history_.redo(graph_)) return;
    resyncPositionsFromGraph();
    dirty_ = true;
    if (!pcged::hasNode(graph_, selNode_)) selNode_.clear();
    invalidate();
}

// ---- run / generate / clean / debug ----
void PcgGraphEditor::setAreaHalfCm(f32 half) {
    areaHalf_ = std::max(100.0f, half);
    setup_.bounds = world::PcgBox{Vec3{-areaHalf_, -areaHalf_, -100000.0f}, Vec3{areaHalf_, areaHalf_, 100000.0f}};
    invalidate();
}

const pcged::PreviewResult& PcgGraphEditor::run() {
    const PcgEditorHost& h = hostRef();
    landscapeHold_ = h.landscape ? h.landscape() : nullptr;
    setup_.landscape = landscapeHold_.get();
    setup_.graphLoader = h.graphLoader;
    run_ = pcged::runPreview(graph_, setup_);
    runStale_ = false;
    if (!run_.ran) status_ = "Not run: the graph has a problem.";
    else status_ = std::to_string(run_.instances) + " instance(s), " + std::to_string(run_.nodesRun) + " node(s) run" + (run_.ok ? "" : ", with errors");
    if (debugOn_) refreshDebug();
    return run_;
}

bool PcgGraphEditor::generate(std::string* rel, std::string* why) {
    const PcgEditorHost& h = hostRef();
    if (!h.publish) { if (why) *why = "no level is open to receive the result"; return false; }
    if (runStale_ || !run_.ran) run();
    if (!run_.ran || !run_.ok) { if (why) *why = "the graph does not run cleanly"; return false; }
    fmt::OcInstanceData table;
    std::string w;
    if (!world::pcgToInstanceData(run_.full, table, 6400.0f, {}, &w) || table.transforms.empty()) {
        if (why) *why = w.empty() ? "the graph produced no instances" : w;
        return false;
    }
    std::vector<std::string> materials;
    for (usize i = 0; i < table.groups.size(); ++i) materials.push_back(i < run_.full.groups.size() ? run_.full.groups[i].material : std::string());
    const std::string r = h.publish(table, materials, path_, &w);
    if (r.empty()) { if (why) *why = w.empty() ? "could not publish the result" : w; return false; }
    generatedRel_ = r;
    if (rel) *rel = r;
    return true;
}

bool PcgGraphEditor::cleanUp(std::string* why) {
    const PcgEditorHost& h = hostRef();
    if (!h.clean) { if (why) *why = "no level is open"; return false; }
    const bool ok = h.clean(path_, why);
    if (ok) generatedRel_.clear();
    return ok;
}

std::vector<Transform> PcgGraphEditor::debugPointsOfSelection() {
    if (selNode_.empty()) return {};
    for (const fmt::OcGraphNode& n : graph_.nodes) {
        if (n.id != selNode_) continue;
        for (const fmt::OcGraphPin& p : n.pins) if (p.isOutput) return pcged::debugPoints(graph_, n.id, p.name, setup_);
        return {};
    }
    return {};
}

void PcgGraphEditor::refreshDebug() {
    std::vector<Vec3>& segs = pcgDebugSegments();
    segs.clear();
    if (!debugOn_) return;
    pcged::debugCrossSegments(debugPointsOfSelection(), 40.0f, segs);
}

void PcgGraphEditor::setDebugPoints(bool on) {
    debugOn_ = on;
    if (on) {
        for (PcgGraphEditor* t : openTabs()) if (t != this && t->debugOn_) t->debugOn_ = false;   // one tab owns the overlay
        refreshDebug();
    } else {
        pcgDebugSegments().clear();
    }
}

void PcgGraphEditor::syncPositions() {
    bool needLayout = false;
    for (const fmt::OcGraphNode& n : graph_.nodes) {
        if (displayPos_.count(n.id)) continue;
        if (n.hasPosition) displayPos_[n.id] = Vec2(static_cast<f32>(n.x), static_cast<f32>(n.y));
        else needLayout = true;
    }
    if (!needLayout) return;
    // A hand-written graph has no positions: lay it out for display only, so an untouched save stays byte-identical.
    const auto auto_ = autoLayoutPositions(graph_, GraphLayoutStyle{}, 1.0f);
    for (const fmt::OcGraphNode& n : graph_.nodes) {
        if (displayPos_.count(n.id)) continue;
        const auto it = auto_.find(n.id);
        displayPos_[n.id] = it != auto_.end() ? it->second : Vec2(0.0f, 0.0f);
    }
}

#if AVER_WITH_IMGUI

namespace {

ImU32 categoryColor(const std::string& category) {
    u32 h = 2166136261u;
    for (char c : category) h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
    static const ImU32 palette[] = {IM_COL32(70, 110, 160, 255), IM_COL32(150, 95, 60, 255), IM_COL32(80, 135, 90, 255), IM_COL32(135, 85, 150, 255),
                                    IM_COL32(150, 130, 55, 255), IM_COL32(60, 130, 135, 255), IM_COL32(150, 70, 80, 255), IM_COL32(95, 95, 150, 255)};
    return palette[h % 8];
}

}  // namespace

void PcgGraphEditor::draw(Engine&) {
    if (!loaded_) {
        ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.3f, 1.0f), "Could not read %s", path_.c_str());
        ImGui::TextWrapped("%s", loadError_.c_str());
        return;
    }
    syncPositions();
    ImGuiIO& io = ImGui::GetIO();
    const PcgEditorHost& host = hostRef();

    // ---- shortcuts ----
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive()) {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) { if (io.KeyShift) redo(); else undo(); }
        else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) redo();
        else if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            if (selLink_ >= 0) { unlink(static_cast<usize>(selLink_)); selLink_ = -1; }
            else removeSelected();
        }
    }

    // Every size below is in logical px and scaled by the UI font: at 300% DPI the text is three times larger and
    // fixed sizes clipped the palette and inspector and drew node titles past their boxes.
    const f32 dpi = ImGui::GetFontSize() / 16.0f;

    // ---- toolbar ----
    ImGui::BeginDisabled(!history_.canUndo());
    if (ImGui::Button("Undo")) undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!history_.canRedo());
    if (ImGui::Button("Redo")) redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(ICON_PLAY " Run")) run();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Runs the graph over the preview box and counts points at every node.");
    ImGui::SameLine();
    ImGui::Checkbox("Auto", &autoRun_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f * dpi);
    int seed = setup_.seed;
    if (ImGui::InputInt("Seed", &seed, 0, 0)) setSeed(seed);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f * dpi);
    f32 halfM = areaHalf_ / 100.0f;
    if (ImGui::InputFloat("Half size (m)", &halfM, 0.0f, 0.0f, "%.0f")) setAreaHalfCm(halfM * 100.0f);
    ImGui::SameLine();
    f32 ext[4];
    ImGui::BeginDisabled(!(host.landscapeExtent && host.landscapeExtent(ext)));
    if (ImGui::Button("Fit landscape")) {
        setup_.bounds = world::PcgBox{Vec3{ext[0], ext[1], -100000.0f}, Vec3{ext[2], ext[3], 100000.0f}};
        invalidate();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!host.publish);
    if (ImGui::Button("Generate")) {
        std::string rel, why;
        status_ = generate(&rel, &why) ? "Generated into " + rel : "Generate failed: " + why;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Bakes the preview's instances into the open level (a cell-sorted .ocinst under Foliage, listed in the level).\nClean up removes it again.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!host.clean);
    if (ImGui::Button("Clean up")) {
        std::string why;
        status_ = cleanUp(&why) ? "Cleaned up." : "Clean up: " + why;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    bool dbg = debugOn_;
    if (ImGui::Checkbox("Debug points", &dbg)) setDebugPoints(dbg);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Draws a cross in the viewport at every point the selected node outputs.");
    if (!status_.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", status_.c_str()); }
    ImGui::Separator();

    if (autoRun_ && runStale_) run();
    const std::vector<pcged::Problem> probs = pcged::problems(graph_);
    const auto problemsOf = [&](const std::string& id) {
        int n = 0;
        for (const pcged::Problem& p : probs) if (p.node == id) ++n;
        for (const pcged::Problem& p : run_.diags) if (p.node == id && p.error) ++n;
        return n;
    };

    const f32 totalW = ImGui::GetContentRegionAvail().x;
    const f32 paletteW = 190.0f * dpi, inspectW = 290.0f * dpi;

    // ---- palette ----
    ImGui::BeginChild("##pcgpalette", ImVec2(paletteW, 0), true);
    ImGui::SetNextItemWidth(-1.0f);
    {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s", paletteSearch_.c_str());
        if (ImGui::InputTextWithHint("##search", "Search nodes...", buf, sizeof buf)) paletteSearch_ = buf;
    }
    std::string lastCat;
    for (const pcged::PaletteEntry& e : pcged::palette(paletteSearch_)) {
        if (e.category != lastCat) { ImGui::SeparatorText(e.category.c_str()); lastCat = e.category; }
        if (ImGui::Selectable(e.type.c_str())) {
            // Added at the middle of the visible canvas.
            CanvasTransform vwAdd = view_;
            vwAdd.zoom *= dpi;
            const Vec2 centre = screenToCanvas(vwAdd, Vec2((totalW - paletteW - inspectW) * 0.5f, 200.0f * dpi));
            addNode(e.type, centre.x, centre.y);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%d in, %d out%s", e.doc.c_str(), e.inputs, e.outputs, e.sink ? " (spawner: a result)" : "");
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // ---- canvas ----
    ImGui::BeginChild("##pcgcanvas", ImVec2(totalW - paletteW - inspectW - 16.0f * dpi, 0), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##canvasbtn", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(28, 28, 32, 255));

    // Framed to the canvas, and framed again whenever the canvas changes size until the user zooms, pans or drags: a new tab is often first
    // drawn squeezed (tiny on its first frame, or under the Content Browser drawer it was created from), and one framing then stuck.
    if (framed_ && !viewTouched_ && (std::fabs(size.x - framedFor_.x) > 1.0f || std::fabs(size.y - framedFor_.y) > 1.0f)) framed_ = false;
    if (!framed_ && !graph_.nodes.empty() && size.x > 200.0f * dpi && size.y > 150.0f * dpi) {
        Vec2 lo(1e9f, 1e9f), hi(-1e9f, -1e9f);
        for (const auto& kv : displayPos_) { lo = Vec2(std::min(lo.x, kv.second.x), std::min(lo.y, kv.second.y)); hi = Vec2(std::max(hi.x, kv.second.x + 160.0f), std::max(hi.y, kv.second.y + 80.0f)); }
        view_ = frameTransform(lo, hi, Vec2(size.x, size.y), 40.0f * dpi, 0.3f * dpi, 1.0f * dpi);
        view_.zoom /= dpi;
        framed_ = true;
        framedFor_ = Vec2(size.x, size.y);
    }
    // The layout is in logical canvas units (positions and node sizes alike); the UI scale is folded into the zoom here, so a 300% DPI
    // screen draws the same graph three times larger instead of growing the nodes into each other. view_.zoom stays the user's zoom.
    CanvasTransform vw = view_;
    vw.zoom *= dpi;
    const auto toScreen = [&](Vec2 c) { const Vec2 s = canvasToScreen(vw, c); return ImVec2(origin.x + s.x, origin.y + s.y); };

    // grid
    const f32 step = 100.0f * vw.zoom;
    if (step > 8.0f) {
        for (f32 x = std::fmod(view_.panPx.x, step); x < size.x; x += step) dl->AddLine(ImVec2(origin.x + x, origin.y), ImVec2(origin.x + x, origin.y + size.y), IM_COL32(255, 255, 255, 12));
        for (f32 y = std::fmod(view_.panPx.y, step); y < size.y; y += step) dl->AddLine(ImVec2(origin.x, origin.y + y), ImVec2(origin.x + size.x, origin.y + y), IM_COL32(255, 255, 255, 12));
    }

    // layouts in canvas space
    std::vector<GraphNodeLayout> layouts;
    layouts.reserve(graph_.nodes.size());
    const GraphLayoutStyle style;
    for (const fmt::OcGraphNode& n : graph_.nodes) {
        fmt::OcGraphNode tmp = n;
        const auto it = displayPos_.find(n.id);
        if (it != displayPos_.end()) { tmp.x = it->second.x; tmp.y = it->second.y; }
        layouts.push_back(computeNodeLayout(tmp, n.type, style, 1.0f));
    }

    // input
    const ImVec2 mp = ImGui::GetIO().MousePos;
    const Vec2 mouseCanvas = screenToCanvas(vw, Vec2(mp.x - origin.x, mp.y - origin.y));
    if (hovered) {
        if (io.MouseWheel != 0.0f) {
            viewTouched_ = true;
            const f32 z = std::min(1.5f, std::max(0.25f, view_.zoom * (1.0f + 0.1f * io.MouseWheel)));
            view_ = zoomAroundScreenPoint(vw, z * dpi, Vec2(mp.x - origin.x, mp.y - origin.y));
            view_.zoom = z;
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f)) { view_.panPx = view_.panPx + Vec2(io.MouseDelta.x, io.MouseDelta.y); viewTouched_ = true; }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const GraphHitResult hit = hitTest(graph_, layouts, mouseCanvas, style, 1.0f);
            selLink_ = -1;
            if (hit.kind == GraphHitKind::Pin) {
                linking_ = true; linkNode_ = hit.nodeId; linkPin_ = hit.pinName; linkFromOutput_ = hit.pinIsOutput;
                selNode_ = hit.nodeId;
            } else if (hit.kind == GraphHitKind::Node) {
                selNode_ = hit.nodeId;
                dragNode_ = hit.nodeId;
                dragMoved_ = false;
                beginDrag();
                if (debugOn_) refreshDebug();
            } else if (hit.kind == GraphHitKind::Link) {
                selLink_ = hit.linkIndex;
            } else {
                selNode_.clear();
                if (debugOn_) refreshDebug();
            }
        }
    }
    if (!dragNode_.empty()) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            const auto it = displayPos_.find(dragNode_);
            if (it != displayPos_.end() && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
                moveNode(dragNode_, it->second.x + io.MouseDelta.x / vw.zoom, it->second.y + io.MouseDelta.y / vw.zoom);
                dragMoved_ = true;
                viewTouched_ = true;
            }
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            // Decided by whether the node moved: at release the button is already up, so asking IsMouseDragging here
            // was always false and every move lost its undo entry.
            if (!dragMoved_) history_.cancelPush();   // a click, not a move
            dragNode_.clear();
        }
    }
    if (linking_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const GraphHitResult hit = hitTest(graph_, layouts, mouseCanvas, style, 1.0f);
        if (hit.kind == GraphHitKind::Pin && hit.pinIsOutput != linkFromOutput_ && hit.nodeId != linkNode_) {
            std::string why;
            const bool ok = linkFromOutput_ ? linkPins(linkNode_, linkPin_, hit.nodeId, hit.pinName, &why) : linkPins(hit.nodeId, hit.pinName, linkNode_, linkPin_, &why);
            if (!ok) status_ = "Link refused: " + why;
        }
        linking_ = false;
    }

    // links
    for (usize i = 0; i < graph_.links.size(); ++i) {
        const fmt::OcGraphLink& l = graph_.links[i];
        const GraphPinLayout* a = findPinLayout(layouts, l.sourceNode, l.sourcePin);
        const GraphPinLayout* b = findPinLayout(layouts, l.destNode, l.destPin);
        if (!a || !b) continue;
        const GraphBezier bz = linkBezier(a->pos, b->pos);
        dl->AddBezierCubic(toScreen(bz.p1), toScreen(bz.p2), toScreen(bz.p3), toScreen(bz.p4), static_cast<isize>(i) == selLink_ ? IM_COL32(255, 160, 60, 255) : pcged::pinColor(a->type), 2.0f);
    }
    if (linking_) {
        for (const GraphNodeLayout& L : layouts) {
            if (L.nodeId != linkNode_) continue;
            for (const GraphPinLayout& p : L.pins) {
                if (p.name != linkPin_ || p.isOutput != linkFromOutput_) continue;
                const GraphBezier bz = linkFromOutput_ ? linkBezier(p.pos, mouseCanvas) : linkBezier(mouseCanvas, p.pos);
                dl->AddBezierCubic(toScreen(bz.p1), toScreen(bz.p2), toScreen(bz.p3), toScreen(bz.p4), IM_COL32(255, 255, 255, 140), 2.0f);
            }
        }
    }

    // nodes: text follows the zoom like the boxes do, so a title stays inside its node at any zoom
    ImFont* font = ImGui::GetFont();
    const f32 fontPx = ImGui::GetFontSize() * view_.zoom;
    for (usize i = 0; i < graph_.nodes.size(); ++i) {
        const fmt::OcGraphNode& n = graph_.nodes[i];
        const GraphNodeLayout& L = layouts[i];
        const ImVec2 lo = toScreen(L.min), hi = toScreen(L.max);
        const world::PcgNodeDef* def = world::pcgFindNode(n.type);
        const pcged::NodeSpec* spec = pcged::specOf(n.type);
        const bool sel = n.id == selNode_;
        const int errs = problemsOf(n.id);
        dl->AddRectFilled(lo, hi, IM_COL32(44, 44, 50, 245), 4.0f);
        dl->AddRectFilled(lo, ImVec2(hi.x, lo.y + style.headerHeightPx * vw.zoom), categoryColor(spec ? spec->category : "Other"), 4.0f, ImDrawFlags_RoundCornersTop);
        dl->AddRect(lo, hi, errs ? IM_COL32(230, 70, 60, 255) : sel ? IM_COL32(255, 150, 50, 255) : (def && def->sink) ? IM_COL32(210, 170, 70, 200) : IM_COL32(90, 90, 100, 255), 4.0f, 0, sel ? 2.0f : 1.0f);
        if (view_.zoom > 0.45f) {
            dl->AddText(font, fontPx, ImVec2(lo.x + 8.0f * vw.zoom, lo.y + 4.0f * vw.zoom), IM_COL32(240, 240, 240, 255), n.type.c_str());
            const auto pc = run_.pointCount.find(n.id);
            if (pc != run_.pointCount.end()) {
                char b[32];
                std::snprintf(b, sizeof b, "%u pts", pc->second);
                dl->AddText(font, fontPx, ImVec2(lo.x, hi.y + 2.0f * vw.zoom), IM_COL32(150, 200, 150, 255), b);
            }
            if (errs) dl->AddText(font, fontPx, ImVec2(hi.x - 14.0f * vw.zoom, lo.y + 2.0f * vw.zoom), IM_COL32(255, 90, 80, 255), "!");
        }
        for (const GraphPinLayout& p : L.pins) {
            const ImVec2 c = toScreen(p.pos);
            dl->AddCircleFilled(c, style.pinRadiusPx * vw.zoom, pcged::pinColor(p.type));
            if (view_.zoom > 0.6f) {
                const ImVec2 ts = font->CalcTextSizeA(fontPx, FLT_MAX, 0.0f, p.name.c_str());
                const f32 off = 8.0f * vw.zoom;
                dl->AddText(font, fontPx, ImVec2(p.isOutput ? c.x - ts.x - off : c.x + off, c.y - ts.y * 0.5f), IM_COL32(200, 200, 205, 255), p.name.c_str());
            }
        }
    }
    dl->PopClipRect();
    ImGui::EndChild();
    ImGui::SameLine();

    // ---- inspector ----
    ImGui::BeginChild("##pcginspect", ImVec2(0, 0), true);
    fmt::OcGraphNode* sel = nullptr;
    for (fmt::OcGraphNode& n : graph_.nodes) if (n.id == selNode_) sel = &n;
    if (!sel) {
        ImGui::TextDisabled("Select a node.");
        ImGui::TextWrapped("Click a node in the palette to add it, drag from a pin to another to link, Delete removes the selection, middle or right drag pans, the wheel zooms.");
    } else {
        ImGui::Text("%s", sel->type.c_str());
        ImGui::TextDisabled("%s", sel->id.c_str());
        if (const world::PcgNodeDef* def = world::pcgFindNode(sel->type)) ImGui::TextWrapped("%s", def->doc ? def->doc : "");
        const auto pc = run_.pointCount.find(sel->id);
        if (pc != run_.pointCount.end()) ImGui::Text("%u point(s) on the first output", pc->second);
        ImGui::Separator();
        ImGui::TextDisabled("Parameters");
        const std::string nodeId = sel->id, nodeType = sel->type;
        const f32 labelCol = ImGui::GetContentRegionAvail().x * 0.45f;
        for (const GraphAttributeRow& row : computeAttributeRows(*sel, pcged::declaredKeys(nodeType))) {
            ImGui::PushID(row.key.c_str());
            const pcged::ParamSpec* spec = pcged::paramSpec(nodeType, row.key);
            const std::string shown = row.present ? row.value : (spec ? std::string(spec->def) : std::string());
            if (spec && spec->kind == pcged::ParamKind::Flag) {
                bool on = shown == "1" || shown == "true";
                if (ImGui::Checkbox(row.label.c_str(), &on)) setParam(nodeId, row.key, on ? "1" : "0");
            } else {
                // The name sits in its own column: as only the field's hint it vanished the moment the field held a value.
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(row.label.c_str());
                ImGui::SameLine(labelCol);
                std::string& buf = paramBuf_[nodeId + "." + row.key];
                if (!ImGui::IsAnyItemActive()) buf = shown;
                char text[512];
                std::snprintf(text, sizeof text, "%s", buf.c_str());
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::InputTextWithHint("##v", row.label.c_str(), text, sizeof text)) buf = text;
                if (ImGui::IsItemDeactivatedAfterEdit()) setParam(nodeId, row.key, buf);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%s", row.label.c_str(), spec && spec->def[0] ? (std::string("  (default ") + spec->def + ")").c_str() : "");
            }
            ImGui::PopID();
        }
        for (const pcged::Problem& p : probs) if (p.node == nodeId) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", p.text.c_str());
        for (const pcged::Problem& p : run_.diags) if (p.node == nodeId) ImGui::TextColored(p.error ? ImVec4(1.0f, 0.5f, 0.4f, 1.0f) : ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "%s", p.text.c_str());
    }
    ImGui::Separator();
    ImGui::TextDisabled("Graph");
    for (const pcged::Problem& p : probs) if (p.node.empty()) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", p.text.c_str());
    for (const pcged::Problem& p : run_.diags) if (p.node.empty()) ImGui::TextColored(p.error ? ImVec4(1.0f, 0.5f, 0.4f, 1.0f) : ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "%s", p.text.c_str());
    if (probs.empty() && run_.diags.empty()) ImGui::TextDisabled("No problems.");
    ImGui::EndChild();
}

#else

void PcgGraphEditor::draw(Engine&) {}

#endif

std::unique_ptr<AssetEditor> makePcgGraphEditor(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".ocgraph") return nullptr;
    // Claims only `DOMAIN pcg`: a gameplay or material graph falls through to the graph editor. A file that cannot be read is left to it too.
    std::ifstream in(path, std::ios::binary);
    if (!in) return nullptr;
    std::ostringstream ss;
    ss << in.rdbuf();
    fmt::OcGraphData g;
    if (!fmt::parseOcgraph(ss.str(), g, nullptr) || fmt::ocGraphDomainOf(g) != fmt::OcGraphDomain::Pcg) return nullptr;
    return std::make_unique<PcgGraphEditor>(path);
}

}  // namespace aver::editor
