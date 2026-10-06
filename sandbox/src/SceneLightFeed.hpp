#pragma once
// Feeds the scene's CLight entities to the renderers every frame: Voxi's staged lamp list and the
// reference path tracer's next-event lights. Resolves a light's IES / cookie id to a file under the
// project's content folder, parses and uploads each once, and logs a bad file once.
// Include under AVER_MODULE_VOXI. docs/rendering/LIGHTS.md.
#include "aver/core/Log.hpp"
#include "aver/formats/IesProfile.hpp"
#include "aver/platform/Image.hpp"
#include "aver/pt/PtSceneView.hpp"
#include "aver/scene/LightGather.hpp"
#include "aver/voxi/VoxiRenderer.hpp"
#include "LightDetails.hpp"

#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aver::editor {

class SceneLightFeed {
public:
    // Once per frame, before the frame's draws are submitted. `pt` may be null (no reference view).
    void update(scene::World& world, voxi::VoxiRenderer& voxi, pt::PtSceneView* pt, const std::string& contentDir) {
        ++frame_;
        if (contentDir != dir_) {
            dir_ = contentDir; paths_.clear(); scanned_ = false; failedAt_.clear(); warned_.clear(); known_.clear();
        }

        gathered_.clear();
        scene::gatherLights(world, gathered_);
        voxiOut_.clear();
        ptOut_.clear();
        for (const scene::WorldLight& w : gathered_) {
            voxi::SceneLight s;
            s.kind = w.kind;
            for (int i = 0; i < 3; ++i) {
                s.pos[i] = w.pos[i]; s.axis[i] = w.axis[i]; s.right[i] = w.right[i]; s.colour[i] = w.colour[i];
            }
            s.intensityCd = w.intensityCd;
            s.rangeCm = w.rangeCm;
            s.innerCos = w.innerCos; s.outerCos = w.outerCos;
            s.widthCm = w.widthCm; s.heightCm = w.heightCm;
            s.radiusCm = w.sourceRadiusCm;
            s.castShadows = w.castShadows;
            s.iesPeak = w.iesPeak;

            voxi::SceneLightAssets ptAssets;
            const Asset* ies = w.iesProfile ? asset(static_cast<u64>(w.iesProfile), true, voxi, pt) : nullptr;
            const Asset* cookie = w.cookie ? asset(static_cast<u64>(w.cookie), false, voxi, pt) : nullptr;
            if (ies)    { s.iesId = static_cast<u64>(w.iesProfile); ptAssets.iesIndex = indexOrNone(ies->ptIndex);
                          ptAssets.iesPeakOverMean = ies->peakOverMean; }
            if (cookie) { s.cookieId = static_cast<u64>(w.cookie);  ptAssets.cookieIndex = indexOrNone(cookie->ptIndex); }
            voxiOut_.push_back(s);
            if (pt) {
                const voxi::PackedLight p = voxi::packSceneLight(s, ptAssets);
                pt::PtLight l;
                static_assert(sizeof(l) == sizeof(p), "PtLight and PackedLight are the same 80-byte record");
                std::memcpy(&l, &p, sizeof l);
                ptOut_.push_back(l);
            }
        }
        voxi.setSceneLights(voxiOut_.data(), static_cast<u32>(voxiOut_.size()));
        if (pt) pt->setLights(ptOut_.data(), static_cast<u32>(ptOut_.size()));
    }

private:
    struct Asset {
        bool ies = false;
        f32  peakOverMean = 1.0f;
        u32  ptIndex = 0xFFFFFFFFu;
    };

    static f32 indexOrNone(u32 i) { return i == 0xFFFFFFFFu ? -1.0f : static_cast<f32>(i); }

    // Loads and registers an asset on first sight; null when the id names no readable file.
    const Asset* asset(u64 id, bool ies, voxi::VoxiRenderer& voxi, pt::PtSceneView* pt) {
        if (const auto it = known_.find(id); it != known_.end()) return &it->second;
        if (const auto f = failedAt_.find(id); f != failedAt_.end() && frame_ - f->second < 300) return nullptr;

        std::string rel = pathFor(id);
        if (rel.empty() && frame_ - lastScan_ > 120) {   // a file added since the last scan
            scanned_ = false;
            rel = pathFor(id);
        }
        if (rel.empty()) return fail(id, ies, "no file with that id under the project's content folder");

        const std::string abs = dir_ + "\\" + rel;
        Asset a;
        a.ies = ies;
        std::string err;
        if (ies) {
            fmt::IesProfile prof;
            fmt::IesTable table;
            if (!fmt::loadIes(abs, prof, &err) || !fmt::bakeIesTable(prof, table))
                return fail(id, true, rel + ": " + (err.empty() ? "profile has no emission" : err));
            const std::vector<u16> half = fmt::iesTableToHalf(table);
            a.peakOverMean = table.peakOverMean;
            voxi.registerIesProfile(id, half.data(), table.peakOverMean);
            if (pt) a.ptIndex = pt->lightTexture(id, fmt::kIesTableH, fmt::kIesTableV, true, half.data());
        } else {
            ImageData img;
            if (!decodeImage(abs, img, &err) || !img.valid())
                return fail(id, false, rel + ": " + err);
            voxi.registerCookie(id, img.width, img.height, img.pixels.data());
            if (pt) a.ptIndex = pt->lightTexture(id, img.width, img.height, false, img.pixels.data());
        }
        AVER_INFO("[Lights] loaded {} {}", ies ? "IES profile" : "cookie", rel);
        return &known_.emplace(id, a).first->second;
    }

    const Asset* fail(u64 id, bool ies, const std::string& why) {
        failedAt_[id] = frame_;   // retried after a few seconds: the file may have just been added
        if (warned_.insert(id).second)
            AVER_WARN("[Lights] {} 0x{:016x} not used: {}", ies ? "IES profile" : "cookie", id, why);
        return nullptr;
    }

    std::string pathFor(u64 id) {
        if (!scanned_) {
            paths_.clear();
            for (const LightAssetChoice& c : listLightAssets(dir_, true)) paths_.emplace(c.id, c.label);
            for (const LightAssetChoice& c : listLightAssets(dir_, false)) paths_.emplace(c.id, c.label);
            scanned_ = true;
            lastScan_ = frame_;
        }
        const auto it = paths_.find(id);
        return it == paths_.end() ? std::string() : it->second;
    }

    u64 frame_ = 0, lastScan_ = 0;
    std::string dir_;
    bool scanned_ = false;
    std::unordered_map<u64, std::string> paths_;     // id -> content-relative path
    std::unordered_map<u64, Asset> known_;
    std::unordered_map<u64, u64> failedAt_;          // id -> frame it failed on; retried after 300 frames
    std::unordered_set<u64> warned_;                 // said once per id
    std::vector<scene::WorldLight> gathered_;
    std::vector<voxi::SceneLight> voxiOut_;
    std::vector<pt::PtLight> ptOut_;
};

} // namespace aver::editor
