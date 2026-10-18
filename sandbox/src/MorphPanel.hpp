#pragma once
// The blend-shape sliders: a search box, one slider per target with the clip's live value drawn as a marker, Zero all /
// Reset / Release / Solo, a Poses tab for the face-rig poses, and a Key button that asks its host to key the value at
// the playhead. Docs/plans/wave9/MORPH_TARGETS.md 2.10.
//
// A widget over plain data: the host (the Anim editor, the Face Rig tab) tells it the target and pose names, feeds it the
// animated value at the playhead, and reads back what the sliders say. A slider the author touches becomes an OVERRIDE
// that wins over the animation until Release; nothing here knows about meshes, clips or the GPU.
#include "aver/core/Types.hpp"

#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace aver::editor {

// Raised by a Key button: key `name` (a morph target when !isPose, else a face pose) at the playhead with `value`.
struct MorphKeyRequest {
    std::string name;
    bool isPose = false;
    f32 value = 0.0f;
};

class MorphPanel {
public:
    static constexpr f32 kNone = std::numeric_limits<f32>::quiet_NaN();   // "no override" / "not driven"
    static bool isNone(f32 v) { return v != v; }

    // The previewed mesh's targets (names and default weights, parallel). A changed list resets every override.
    void setTargets(const std::vector<std::string>& names, const std::vector<f32>& defaults);
    // The face poses offered on the Poses tab. A changed list resets the pose overrides.
    void setPoses(const std::vector<std::string>& names);
    // What the animation drives at the playhead, parallel to the targets / poses; kNone = not driven.
    void setLiveTargets(const std::vector<f32>& v) { live_ = v; live_.resize(names_.size(), kNone); }
    void setLivePoses(const std::vector<f32>& v) { livePose_ = v; livePose_.resize(poseNames_.size(), kNone); }
    void setStats(std::string line) { stats_ = std::move(line); }
    // Show the Key buttons (a clip tab), or not (the Face Rig tab).
    void allowKeying(bool on) { keying_ = on; }

    bool hasTargets() const { return !names_.empty(); }
    u32 targetCount() const { return static_cast<u32>(names_.size()); }
    u32 poseCount() const { return static_cast<u32>(poseNames_.size()); }
    const std::string& targetName(u32 i) const { return names_[i]; }
    const std::string& poseName(u32 i) const { return poseNames_[i]; }
    f32 defaultOf(u32 i) const { return i < defaults_.size() ? defaults_[i] : 0.0f; }
    // The author's override, or kNone.
    f32 targetOverride(u32 i) const { return i < override_.size() ? override_[i] : kNone; }
    f32 poseOverride(u32 i) const { return i < poseOverride_.size() ? poseOverride_[i] : kNone; }
    bool anyOverride() const;

    // Draws the panel in the current ImGui window. Returns true when the author changed an override this frame.
    bool draw(const char* id);

    // Programmatic edits (the Face Rig tab's viseme buttons).
    void setTargetOverride(u32 i, f32 v) { if (i < override_.size()) override_[i] = v; }
    void setPoseOverride(u32 i, f32 v) { if (i < poseOverride_.size()) poseOverride_[i] = v; }
    void release();

    // True once per Key click; fills `out`.
    bool takeKeyRequest(MorphKeyRequest& out);

private:
    void drawTargets(bool& changed);
    void drawPoses(bool& changed);
    bool matches(const std::string& name) const;

    std::vector<std::string> names_, poseNames_;
    std::vector<f32> defaults_, live_, livePose_, override_, poseOverride_;
    std::string stats_;
    char filter_[64] = {};
    bool keying_ = true;
    bool keyPending_ = false;
    MorphKeyRequest key_;
};

} // namespace aver::editor
