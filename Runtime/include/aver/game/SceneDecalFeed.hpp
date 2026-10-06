#pragma once
// Feeds the scene's CDecal entities to Voxi every frame, for either host. Gathers the paintable decals
// (scene::gatherDecals), asks the host's loader for each image id the first time it is seen, registers
// it with the renderer, and hands the converted list to VoxiRenderer::setSceneDecals.
//
// The loader is the host's: the editor resolves an id to a file under the project's content folder
// (sandbox/src/DecalAssets.hpp); a packaged game resolves it from its own content. An id the loader
// cannot supply is retried every few seconds and warned about once, and the decal naming it is not
// drawn rather than drawn as a flat box. Include under AVER_MODULE_VOXI and AVER_MODULE_SCENE.
// docs/rendering/DECALS.md.
#include "aver/core/Log.hpp"
#include "aver/platform/Image.hpp"
#include "aver/scene/DecalGather.hpp"
#include "aver/voxi/VoxiRenderer.hpp"

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aver::game {

// Decodes the image an ObjectId names into RGBA8. `kind` says how the renderer will use it (Colour is
// sRGB), which a loader may use to pick a sibling file. False when the id names nothing readable.
using DecalImageLoader = std::function<bool(u64 id, voxi::DecalImageKind kind, ImageData& out)>;

class SceneDecalFeed {
public:
    // Once per frame, before the frame's draws are submitted.
    void update(scene::World& world, voxi::VoxiRenderer& voxi, const DecalImageLoader& load) {
        ++frame_;
        gathered_.clear();
        scene::gatherDecals(world, gathered_);
        out_.clear();
        out_.reserve(gathered_.size());
        for (const scene::WorldDecal& w : gathered_) {
            voxi::SceneDecal s;
            std::copy(w.world, w.world + 16, s.world);
            for (int i = 0; i < 3; ++i) { s.halfExtentsCm[i] = w.halfExtentsCm[i]; s.tint[i] = w.tint[i]; }
            s.opacity = w.opacity;
            s.normalStrength = w.normalStrength;
            s.roughness = w.roughness;
            s.metallic = w.metallic;
            s.edgeFade = w.edgeFade;
            s.angleFadeStartDeg = w.angleFadeStartDeg;
            s.angleFadeEndDeg = w.angleFadeEndDeg;
            s.fadeDistanceCm = w.fadeDistanceCm;
            s.sortOrder = w.sortOrder;
            s.colour = w.colour; s.normal = w.normal; s.roughnessMetal = w.roughnessMetal;
            s.uvScale[0] = w.uvScale[0]; s.uvScale[1] = w.uvScale[1];
            s.uvOffset[0] = w.uvOffset[0]; s.uvOffset[1] = w.uvOffset[1];
            bool ok = true;
            ok = image(w.baseTexture, voxi::DecalImageKind::Colour, voxi, load, s.baseId) && ok;
            ok = image(w.normalTexture, voxi::DecalImageKind::Normal, voxi, load, s.normalId) && ok;
            ok = image(w.ormTexture, voxi::DecalImageKind::Data, voxi, load, s.ormId) && ok;
            if (ok) out_.push_back(s);
        }
        voxi.setSceneDecals(out_.data(), static_cast<u32>(out_.size()));
    }

    // Drops what the feed remembers about failures, e.g. after the content folder changed.
    void forget() { failedAt_.clear(); warned_.clear(); }

private:
    // Registers `id` once; false when the decal cannot be drawn without it. id 0 is "no image".
    bool image(i64 id, voxi::DecalImageKind kind, voxi::VoxiRenderer& voxi, const DecalImageLoader& load, u64& outId) {
        if (id == 0) return true;
        const u64 file = static_cast<u64>(id);
        // The renderer's id is the file's, salted by the role: one file can be a colour image here and
        // a data image there, and a registered id keeps the first role it was given.
        const u64 key = file ^ ((static_cast<u64>(kind) + 1u) * 0x9E3779B97F4A7C15ull);
        if (voxi.hasDecalTexture(key)) { outId = key; return true; }
        const auto f = failedAt_.find(key);
        if (f != failedAt_.end() && frame_ - f->second < 300) return false;
        ImageData img;
        if (!load || !load(file, kind, img) || !img.valid()) {
            failedAt_[key] = frame_;   // retried after a few seconds: the file may have just been added
            if (warned_.insert(key).second)
                AVER_WARN("[Decals] image 0x{:016x} not used: the host could not load it", file);
            return false;
        }
        if (!voxi.registerDecalTexture(key, img.width, img.height, img.pixels.data(), kind)) return false;
        outId = key;
        return true;
    }

    u64 frame_ = 0;
    std::unordered_map<u64, u64> failedAt_;
    std::unordered_set<u64> warned_;
    std::vector<scene::WorldDecal> gathered_;
    std::vector<voxi::SceneDecal> out_;
};

} // namespace aver::game
