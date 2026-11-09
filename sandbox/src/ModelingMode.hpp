#pragma once
// EditorMode::Modeling (WAVE2_UE W5 step 6): the tool panels over ModelingModel, the viewport click / marquee handling over ModelingPick.hpp, and the
// wireframe overlay the mode draws. Everything with a rule is in the two headers; this class is the glue and the widgets. No SandboxApp, no scene
// and no GPU in here: the application passes a projector and the mouse, and draws the lines it gets back through the line-mesh route the other
// overlays use.
//
// The mesh is shown as lines (edges grey, selected elements orange, vertices as small crosses in vertex mode) at `origin` in the world, not as a
// shaded mesh: showing the live edit mesh shaded needs the content system to take a mesh per edit, which is a renderer step this mode does not take.
// Save writes the authored .ocmesh (with its edit sidecar); placing it in a level is the Content Browser's job.
#include "ModelingPick.hpp"

#include "aver/core/Math.hpp"

#include <functional>
#include <string>
#include <vector>

namespace aver {
class Engine;
}

namespace aver::editor {

struct ModelingLine {
    Vec3 a, b;
    f32 r = 0.7f, g = 0.7f, bl = 0.7f;
};

struct ModelingMouse {
    f32 x = 0, y = 0;
    bool overScene = false;    // the cursor is over the viewport image and nothing covers it
    bool down = false, clicked = false, released = false;
    bool ctrl = false, shift = false;
};

class ModelingMode;
// Draws the document as a wireframe when `active`; destroys last frame's mesh either way. Call once per frame after the scene.
void drawModelingLines(Engine& e, const ModelingMode& mode, bool active, bool hideChrome);

class ModelingMode {
public:
    modeling::ModelingModel model;
    Vec3 origin{0, 0, 0};      // where the mesh's local origin sits in the world, cm

    // World point -> pixels. False when behind the camera.
    using WorldProjector = std::function<bool(const Vec3& world, f32& sx, f32& sy)>;

    // Per frame while the mode is active: a click picks, a drag past the threshold is a marquee, Ctrl adds, Shift also adds. Returns true when the
    // mouse event was used (so the application does not also pick a level object).
    bool handleMouse(const ModelingMouse& mouse, const WorldProjector& project, const Vec3& eye);
    bool marqueeActive() const { return marqueeActive_; }

    // The tool panels, drawn into the Mode panel. `contentDir` is where the default save path points.
    void drawPanel(const std::string& contentDir, f32 dpi);

    // Edges, selection and vertices as world-space segments. Empty when there is no document.
    void collectLines(std::vector<ModelingLine>& out) const;

    // Ctrl+Z / Ctrl+Y while the mode is active.
    bool undo() { return model.undo(); }
    bool redo() { return model.redo(); }

    // Keys 1 / 2 / 3 select vertex / edge / face mode (the application calls this with the pressed key); Delete deletes the selection.
    void setSelectMode(int index) {
        model.setMode(index == 0 ? modeling::SelectMode::Vertex : index == 1 ? modeling::SelectMode::Edge : modeling::SelectMode::Face);
    }

private:
    modeling::Projector localProjector(const WorldProjector& project, const Vec3& eye) const;

    bool marqueeArmed_ = false, marqueeActive_ = false;
    f32 x0_ = 0, y0_ = 0;
    // panel state
    int primitive_ = 0;
    f32 size_[3] = {100, 100, 100};
    int detail_ = 16;
    f32 move_[3] = {0, 0, 0};
    int rotAxis_ = 2;
    f32 rotDeg_ = 15.0f;
    f32 scale_[3] = {1, 1, 1};
    std::string savePath_;
    std::string booleanPath_;
    std::string pathBuf_;
};

}  // namespace aver::editor
