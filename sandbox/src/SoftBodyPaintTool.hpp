#pragma once
// The soft-body paint tool: pins and skin reach on a mesh's sim particles, plus the rule rows (above a height,
// near a bone, in a sphere, from a mask texture) with a live count of what each pins. Vertex colour is not
// available (.ocmesh has no colour stream), so this is how an author says which parts of a banner or a cape are
// held. docs/SOFTBODY.md "Authoring".
//
// THE MODEL IS PURE (no ImGui, no scene, no GPU) so tests/editor can drive every rule: the brush selects
// particles in SIM space, which are the welds of the render vertices, so a UV seam paints as one. Painted values
// live per welded particle, keyed by the mesh key; a stale key means the mesh changed since painting. One undo
// entry per stroke or rule edit (SnapshotUndo, as the other asset editors). The ImGui panel and the viewport
// feed are guarded by AVER_WITH_IMGUI.
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/softbody/SimMesh.hpp"
#include "aver/softbody/SoftAsset.hpp"

#include "SnapshotUndo.hpp"

#include <string>
#include <vector>

namespace aver::editor {

// A line segment for the viewport feed (world space, colours 0..1); the renderer turns these into line vertices.
struct PaintLine {
    Vec3 a, b;
    f32 r = 1.0f, g = 1.0f, bl = 1.0f;
};

class SoftBodyPaintTool {
public:
    enum class Mode : u8 { Pin = 0, Reach = 1, Erase = 2 };
    struct Brush {
        Mode mode = Mode::Pin;
        f32 radiusCm = 10.0f;
        f32 reach01 = 0.5f;      // Reach mode: 0 = hard skin .. 1 = twice the default reach
    };
    Brush brush;

    // The scene entity being painted and its world matrix; the caller keeps them current each frame.
    u32 entity = 0;
    Mat4 entityWorld = Mat4::identity();

    // Builds the picking cage from the mesh (mesh-local space, welded) and loads the asset's paint. `assetPath` is
    // where save() writes ("" = none yet). False when the mesh cannot be welded.
    bool load(const fmt::OcMeshData& mesh, const softbody::SoftAsset& asset, const std::string& assetPath);
    void close();
    bool active() const { return active_; }

    const softbody::SoftAsset& asset() const { return asset_; }
    const std::string& assetPath() const { return path_; }
    void setAssetPath(const std::string& p) { path_ = p; }
    u32 particleCount() const { return static_cast<u32>(sim_.cage.particles.size()); }
    bool stale() const { return staleOnLoad_; }          // the loaded paint was for a different mesh
    bool dirty() const { return dirty_; }

    // ---- picking and selecting (mesh-local space) ----
    // Nearest hit of a ray on the cage triangles. `triangle` (optional) receives the sim triangle.
    bool pick(const Vec3& origin, const Vec3& dir, Vec3& hit, u32* triangle = nullptr) const;
    // Sim particles within `radius` cm of `centre`: welded, so both sides of a UV seam come together.
    std::vector<u32> select(const Vec3& centre, f32 radius) const;

    // ---- painting: a stroke is begin/paint*/end, one undo entry ----
    void beginStroke();
    u32 paint(const std::vector<u32>& particles);        // returns how many changed
    void endStroke();
    bool undo();
    bool redo();

    // ---- state ----
    bool pinned(u32 particle) const { return particle < pin_.size() && pin_[particle] == 255; }
    u8 reachByte(u32 particle) const { return particle < reach_.size() ? reach_[particle] : 255; }
    u32 pinnedCount() const;
    u32 reachCount() const;                              // particles with a reach limit (byte != 255)

    // ---- rules ----
    const std::vector<softbody::PaintRule>& rules() const { return asset_.paint.rules; }
    void addRule(const softbody::PaintRule& r);
    void removeRule(usize index);
    // How many particles a single rule pins on this mesh, with the optional mask/bone providers.
    u32 ruleCount(const softbody::PaintRule& r) const;
    void setProviders(softbody::MaskSampler mask, softbody::BoneWeightQuery bone) { mask_ = std::move(mask); bone_ = std::move(bone); }

    // Writes pin/reach/meshKey/rules into the asset and saves it. False with `why` when there is no path or the
    // write failed.
    bool save(std::string* why = nullptr);
    // The asset as it would be saved now (arrays omitted when nothing is painted).
    softbody::SoftAsset assetToSave() const;

    // ---- viewport feed ----
    // The cursor ray in WORLD space. Paints under the brush while `down` (one stroke per press) and remembers
    // where the ray hit for the brush circle. True when the ray hit the mesh.
    bool viewportPaint(const Vec3& rayOriginWorld, const Vec3& rayDirWorld, bool down);
    const Vec3* hover() const { return hasHit_ ? &hitLocal_ : nullptr; }
    // Pins as yellow crosses, reach as green-to-red crosses, the brush circle at `hit`; mesh-local points are
    // carried through `entityWorld` (row vector).
    void collectLines(const Mat4& entityWorld, const Vec3* brushHitLocal, std::vector<PaintLine>& out) const;

    // The colour a painted reach byte draws with: green (hard skin) to red (loosest); 255 = free draws grey.
    static void reachColour(u8 byte, f32& r, f32& g, f32& b);

#if AVER_WITH_IMGUI
    // The panel body: brush, rule rows with live counts, save, undo/redo. Returns true when the asset changed.
    bool drawPanel();
#endif

private:
    struct State {
        std::vector<u8> pin, reach;
        std::vector<softbody::PaintRule> rules;
    };
    State snapshot() const;
    void restore(const State& s);
    void markDirty() { dirty_ = true; }

    bool active_ = false;
    bool hasHit_ = false;
    Vec3 hitLocal_{0, 0, 0};
    bool dirty_ = false;
    bool staleOnLoad_ = false;
    bool inStroke_ = false;
    bool strokeChanged_ = false;
    State strokeBefore_;
    std::string path_;
    softbody::SoftAsset asset_;
    fmt::OcMeshData mesh_;
    softbody::SimMesh sim_;
    std::vector<u8> pin_, reach_;
    softbody::MaskSampler mask_;
    softbody::BoneWeightQuery bone_;
    SnapshotUndo<State> undo_;
};

// The one tool the Details sections and the viewport share.
SoftBodyPaintTool& softBodyPaintTool();

}  // namespace aver::editor
