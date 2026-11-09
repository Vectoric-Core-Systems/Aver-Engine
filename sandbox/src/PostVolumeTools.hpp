#pragma once
// The editor side of post-process volumes: Add > Post Process Volume, the volume boxes as editor lines, the
// Details body that edits a volume's fields, and the blend that previews them in the viewport. The records
// live in the level header (SandboxApp::lvl_.levelHeader_.postVolumes), so they save and load with the level and
// need no entity. Blend and weights are the runtime's (Runtime/include/aver/game/GamePostVolumes.hpp), so the
// editor shows what the game renders. docs/rendering/POST_VOLUMES.md.
#include "aver/core/Math.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/game/GamePostVolumes.hpp"
#include "aver/rhi/RHI.hpp"

#include <string>
#include <vector>

namespace aver::editor {

// A new volume: a 10 m cube around `at` with a 2 m blend and no overrides (it changes nothing until the author
// ticks a field).
inline fmt::OcPostVolume makeNewPostVolume(const Vec3& at, const std::string& name = "PostProcessVolume") {
    fmt::OcPostVolume v;
    v.name = name;
    const f64 h = 500.0;
    v.bounds[0] = at.x - h; v.bounds[1] = at.y - h; v.bounds[2] = at.z - h;
    v.bounds[3] = at.x + h; v.bounds[4] = at.y + h; v.bounds[5] = at.z + h;
    v.blendCm = 200.0;
    return v;
}

class PostVolumeTools {
public:
    // Appends a new volume at the camera and selects it. Returns its index.
    int addAtCamera(std::vector<fmt::OcPostVolume>& vols, const Vec3& cam);

    int  selected() const { return selected_; }
    void select(int i) { selected_ = i; }

    // The Details body: the volume list and the selected volume's fields. `base` seeds a newly ticked override
    // with the value it will replace. True when anything changed (the caller marks the level unsaved).
    bool drawDetails(rhi::IDevice* dev, const std::string& contentDir, std::vector<fmt::OcPostVolume>& vols,
                     const rhi::PostSettings& base, const Vec3& cam);

    // Once per frame in the overlay stage: the selected volume's box bright, the rest dim when `showAll`.
    // Infinite volumes have no box.
    void drawOverlay(rhi::IDevice& dev, const std::vector<fmt::OcPostVolume>& vols, bool showAll);

    // `base` with the level's volumes applied at `eye`, ready for setPostProcess. LUT textures load on first use.
    rhi::PostSettings blend(rhi::IDevice& dev, const std::string& contentDir,
                            const std::vector<fmt::OcPostVolume>& vols, const rhi::PostSettings& base, const Vec3& eye);

    // A level closed: the selection and the LUT textures go (the cache is per project content folder).
    void reset(rhi::IDevice* dev);

private:
    void refreshLuts(const std::string& contentDir);

    int selected_ = -1;
    game::PostLutCache luts_;
    std::vector<std::string> cubeFiles_;     // content-relative .cube paths, for the LUT picker
    bool cubeScanned_ = false;
    rhi::LineHandle mesh_ = 0;
    std::vector<rhi::LineVertex> verts_;
};

} // namespace aver::editor
