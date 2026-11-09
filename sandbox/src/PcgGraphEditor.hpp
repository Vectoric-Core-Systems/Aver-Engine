#pragma once
// The PCG Graph tab: a `DOMAIN pcg` .ocgraph opened as an asset (WAVE2_UE W3 step 6). Left, the node palette (search, categories); middle, the
// canvas (pan, zoom, drag nodes, drag a pin to link, typed pin colours, point-count and problem badges); right, the selected node's inspector
// (parameters with their schema defaults, point count, diagnostics). The toolbar runs the preview over a box around the origin, Generate bakes the
// result into the level through the host, Clean up removes it again, and Debug Points draws the selected node's output in the viewport.
//
// All rules are PcgEditModel.hpp; the canvas geometry is GraphEditorGeometry. The file is read and written with the original text kept, so a
// graph opened and saved without edits is byte-identical and records this editor does not model survive (the way GraphEditor does it).
// Hook: assetEditors_.registerFactory(&editor::makePcgGraphEditor) BEFORE makeGraphEditor (first match wins; it only claims `DOMAIN pcg`).
#include "AssetEditor.hpp"
#include "PcgEditModel.hpp"
#include "SnapshotUndo.hpp"

#include "aver/formats/OcInstances.hpp"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aver::editor {

// What the tab needs from the application. Every member may be empty (a headless test, or a build without a level open).
struct PcgEditorHost {
    // The loaded landscape as a query, or null.
    std::function<std::shared_ptr<const world::ILandscapeQuery>()> landscape;
    // The loaded landscape's extent in world cm {minX, minY, maxX, maxY}; false when none.
    std::function<bool(f32 outExtent[4])> landscapeExtent;
    // Writes the baked table next to the project's other foliage, registers it with the open level and shows it. Returns the content-relative path.
    std::function<std::string(const fmt::OcInstanceData& table, const std::vector<std::string>& groupMaterials, const std::string& graphPath, std::string* why)> publish;
    // Removes what publish made for this graph. False with `why` when there was nothing or it failed.
    std::function<bool(const std::string& graphPath, std::string* why)> clean;
    // Content-relative graph name -> graph, for Subgraph / Loop nodes.
    std::function<const fmt::OcGraphData*(const std::string&)> graphLoader;
};
void setPcgEditorHost(PcgEditorHost host);

class PcgGraphEditor final : public AssetEditor {
public:
    explicit PcgGraphEditor(std::string path);
    ~PcgGraphEditor() override;

    const std::string& path() const override { return path_; }
    std::string title() const override;
    bool dirty() const override { return dirty_; }
    void draw(Engine& e) override;
    bool save(std::string* why) override;
    void onFileChanged() override;

    // ---- the document, for the widgets and for tests ----
    bool loaded() const { return loaded_; }
    const std::string& loadError() const { return loadError_; }
    const fmt::OcGraphData& graph() const { return graph_; }
    const std::string& selectedNode() const { return selNode_; }
    void select(const std::string& id) { selNode_ = id; }

    // Each pushes one undo entry; false (and no entry) when refused.
    bool addNode(const std::string& type, f64 x, f64 y, std::string* id = nullptr);
    bool removeSelected();
    bool linkPins(const std::string& srcNode, const std::string& srcPin, const std::string& dstNode, const std::string& dstPin, std::string* why = nullptr);
    bool unlink(usize index);
    bool moveNode(const std::string& id, f64 x, f64 y);   // no undo entry: a drag pushes one when it starts (beginDrag)
    void beginDrag() { pushUndo(); }
    bool setParam(const std::string& nodeId, const std::string& key, const std::string& value);
    void undo();
    void redo();
    bool canUndo() const { return history_.canUndo(); }
    bool canRedo() const { return history_.canRedo(); }

    // ---- run, generate, clean up, debug ----
    pcged::PreviewSetup& setup() { return setup_; }
    void setSeed(i32 s) { setup_.seed = s; invalidate(); }
    void setAreaHalfCm(f32 half);
    f32 areaHalfCm() const { return areaHalf_; }
    // Runs the preview now (the widgets run it when the graph changed and Auto Run is on).
    const pcged::PreviewResult& run();
    const pcged::PreviewResult& lastRun() const { return run_; }
    // Bakes the last run's instances through the host. True with the content-relative path in `rel`.
    bool generate(std::string* rel, std::string* why);
    bool cleanUp(std::string* why);
    // The selected node's first output pin points, as transforms (empty without a selection or a graph that runs).
    std::vector<Transform> debugPointsOfSelection();
    bool debugPointsOn() const { return debugOn_; }
    void setDebugPoints(bool on);

private:
    void loadFromDisk();
    void pushUndo() { history_.push(graph_); }
    void invalidate() { runStale_ = true; }
    void syncPositions();
    void refreshDebug();

    std::string path_;
    std::string originalText_;
    fmt::OcGraphData graph_;
    bool loaded_ = false, dirty_ = false;
    std::string loadError_;
    SnapshotUndo<fmt::OcGraphData> history_;
    std::string selNode_;
    std::unordered_map<std::string, Vec2> displayPos_;   // where each node is drawn; a hand-written graph has no positions until a node is moved

    pcged::PreviewSetup setup_;
    f32 areaHalf_ = 5000.0f;
    pcged::PreviewResult run_;
    bool runStale_ = true, autoRun_ = true, debugOn_ = false;
    std::string status_;
    std::string generatedRel_;
    std::shared_ptr<const world::ILandscapeQuery> landscapeHold_;

#if AVER_WITH_IMGUI
    CanvasTransform view_;
    bool framed_ = false;
    std::string paletteSearch_;
    // drag state
    std::string dragNode_;
    bool linking_ = false;
    std::string linkNode_, linkPin_;
    bool linkFromOutput_ = true;
    isize selLink_ = -1;
    std::unordered_map<std::string, std::string> paramBuf_;   // "<node>.<key>" -> text being typed
#endif
};

std::unique_ptr<AssetEditor> makePcgGraphEditor(const std::string& path);

// The segments (two Vec3 per line, world cm) the open tab with Debug Points on wants drawn: a cross per point of its selected node's output.
// Empty when none. PcgDebugDraw.cpp turns them into the line mesh the other overlays use.
inline std::vector<Vec3>& pcgDebugSegments() {
    static std::vector<Vec3> v;
    return v;
}
// Draws pcgDebugSegments() once per frame after the scene. Does nothing when it is empty or the editor chrome is hidden.
void drawPcgDebugPoints(Engine& e, bool hideChrome);

}  // namespace aver::editor
