#pragma once
// The `.ocinst` instance-set editor tab: open a baked foliage/instance table, look at it in a viewport
// of its own, select / move / rotate / scale / add / delete / duplicate instances, save it back.
//
// THREE LAYERS, so almost all of it is testable without a window:
//   * instset:: maths   -- TRS <-> 12-float matrix, 4x4 inverse, a pixel -> world ray. Header-only-ish,
//                          ImGui-free, no GPU.
//   * InstanceSetDoc    -- the file + InstanceSetModel + bounds + the "is this change on disk my own
//                          save?" stamp. Load, save, reload. ImGui-free, no GPU.
//   * the tab           -- InstanceSetEditor.cpp, behind AVER_WITH_IMGUI: its own ActorPreview (never
//                          sharedPreview()), the viewport interaction and the side panel.
//
// The model (InstanceSetModel.hpp) and the gizmo maths (PreviewGizmo.hpp) already existed; this file is
// the glue that gives them a tab.
#include "AssetEditor.hpp"
#include "FileStamp.hpp"
#include "InstanceSetModel.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace aver::editor {

namespace instset {

// A decomposed instance matrix. rotDeg is (yaw, pitch, roll) about Z, Y, X -- the same Z*Y*X order
// composeEditorTransform (EditorTransform.hpp) builds, so a value typed here lands the way the level
// editor would place it.
struct Trs {
    f32 pos[3]    = {0, 0, 0};
    f32 rotDeg[3] = {0, 0, 0};
    f32 scale[3]  = {1, 1, 1};
};

// .ocinst 12-float row-vector matrix -> position, Euler, per-axis scale. A degenerate (zero-length)
// row reports scale 0 and its world axis for the rotation.
void decompose(const f32 m12[12], Trs& out);
// The inverse of decompose for any non-sheared matrix.
void compose(const Trs& t, f32 out12[12]);

// General 4x4 inverse (row-major). False when singular.
bool invert4(const f32 m[16], f32 out[16]);

// A world ray through the image point (ndcX, ndcY) (each -1..1, y UP) of a row-vector view-proj with
// D3D 0..1 depth. `eye` is the camera position; dir comes back unit length. False on a singular
// matrix or a point at infinity.
bool viewRay(const f32 viewProj[16], f32 ndcX, f32 ndcY, const f32 eye[3], f32 dir[3]);

// The point where a ray meets the plane z = planeZ; false when parallel or behind the origin.
bool rayPlaneZ(const f32 origin[3], const f32 dir[3], f32 planeZ, f32 out[3]);

} // namespace instset

// One open `.ocinst`: the parsed file in an InstanceSetModel, and what a tab needs around it.
class InstanceSetDoc {
public:
    // Loads the file into the model. False (and loaded() false) with `why` on failure.
    bool load(const std::string& path, std::string* why = nullptr);
    // Re-reads the same path, discarding the model's edits and history.
    bool reload(std::string* why = nullptr) { return load(path_, why); }
    // model.exportTo -> fmt::saveOcInstances (atomic) -> markSaved, then remembers the file's size and
    // mtime so the watcher's echo of this very write can be told from someone else's.
    bool save(std::string* why = nullptr);

    bool loaded() const { return loaded_; }
    const std::string& path() const { return path_; }
    InstanceSetModel& model() { return model_; }
    const InstanceSetModel& model() const { return model_; }

    // True when the file on disk is exactly what this doc last read or wrote.
    bool diskMatchesLastIo() const;

    // World bounds of every live instance (as the model's per-asset bounds currently stand).
    // recomputeBounds() walks them all (millions of rows: call it on load and when asset bounds
    // change, not per frame); growBounds() unions in the selection after an edit.
    void recomputeBounds();
    void growBounds();
    bool haveBounds() const { return haveBounds_; }
    const f32* boundsLo() const { return lo_; }
    const f32* boundsHi() const { return hi_; }

private:
    std::string path_;
    InstanceSetModel model_;
    bool loaded_ = false;
    FileStamp stamp_;
    f32 lo_[3] = {0, 0, 0}, hi_[3] = {0, 0, 0};
    bool haveBounds_ = false;
};

// What the tab needs from the application and cannot reach itself.
struct InstanceSetEditorHooks {
    // True when the open level lists this absolute .ocinst path as one of its foliage files.
    std::function<bool(const std::string& absInstPath)> levelReferences;
    // The level's Regenerate Streaming Data (saves the level, rebakes, reopens it). `report` gets the
    // outcome text either way.
    std::function<bool(std::string& report)> regenerateStreaming;
};
void setInstanceSetEditorHooks(InstanceSetEditorHooks hooks);

// Frees every tab's GPU state (and anything a closed tab left to be freed a frame later). Call before
// the device goes, beside shutdownActorEditors().
void shutdownInstanceSetEditors();

// The factory AssetEditorHost wants: the editor for a `.ocinst`, else nullptr.
std::unique_ptr<AssetEditor> makeInstanceSetEditor(const std::string& path);

} // namespace aver::editor
