#pragma once
// The editor's decal image loader: resolves the ObjectId a CDecal stores to an image file under the
// project's content folder (the same id space and image listing as light cookies) and decodes it.
// Plugs into aver::game::SceneDecalFeed. docs/rendering/DECALS.md.
#include "LightDetails.hpp"
#include "aver/platform/Image.hpp"
#include "aver/game/SceneDecalFeed.hpp"

#include <string>
#include <unordered_map>

namespace aver::editor {

class DecalImageResolver {
public:
    void setContentDir(const std::string& dir) {
        if (dir == dir_) return;
        dir_ = dir;
        paths_.clear();
        scanned_ = false;
    }

    // A loader bound to this resolver, for SceneDecalFeed::update.
    game::DecalImageLoader loader() {
        return [this](u64 id, voxi::DecalImageKind, ImageData& out) { return load(id, out); };
    }

    bool load(u64 id, ImageData& out) {
        const std::string rel = pathFor(id);
        if (rel.empty()) return false;
        std::string err;
        return decodeImage(dir_ + "\\" + rel, out, &err) && out.valid();
    }

    // Forces a rescan on the next lookup (a file was added to the project).
    void rescan() { scanned_ = false; }

private:
    std::string pathFor(u64 id) {
        if (!scanned_) {
            paths_.clear();
            for (const LightAssetChoice& c : listLightAssets(dir_, false)) paths_.emplace(c.id, c.label);
            scanned_ = true;
        }
        const auto it = paths_.find(id);
        return it == paths_.end() ? std::string() : it->second;
    }

    std::string dir_;
    bool scanned_ = false;
    std::unordered_map<u64, std::string> paths_;
};

} // namespace aver::editor
