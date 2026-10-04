#include "aver/game/GameFoliage.hpp"

#if AVER_MODULE_SCENE && AVER_MODULE_VOXI

#include "aver/game/GameContent.hpp"
#include "aver/game/SceneSubmission.hpp"
#include "aver/voxi/VoxiRenderer.hpp"
#include "aver/formats/OcInstances.hpp"
#include "aver/pbr/Material.hpp"
#include "aver/scene/scene_abi.h"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"

#include <chrono>
#include <string_view>
#include <utility>
#include <vector>

namespace aver::game {

namespace {

// Resolves `contentDir`/`relPath` the same way GameContent::resolveMaterialGraph resolves a
// GRAPHREF: a plain join, forward slashes normalised to the native separator so a level authored
// on one convention still opens on the other.
std::string resolveContentPath(const std::string& contentDir, const std::string& relPath) {
    std::string path = contentDir + "\\" + relPath;
    for (char& c : path) if (c == '/') c = '\\';
    return path;
}

// Interns nothing: `token` already IS an interned surface name (GameContent::buildMeshParts and
// meshSlot0Material_ both intern a mesh's own materialSlots entry through aver_scene_material at
// mesh-load time, exactly as an authored placement's own material override would). Ensures the
// AUTHORED HANDLE actually exists behind it the same way GameLevel.cpp's opt.bindMaterial and
// GameStreaming.cpp's restore.bindMaterial do for a placement's or a scattered species' own
// override -- content.materialForSurface(name) then content.bindSurfaceMaterial(token, h) -- except
// a foliage group carries no override string of its own to hand over, only the token a mesh part
// was already interned under, so the name is recovered from the token itself
// (aver_scene_material_name) rather than threaded in from the caller. A no-op once bound (or for
// token 0, "this slot named nothing"), so repeat instances of the same prototype's parts cost one
// cheap authoredFor() check each, not a re-resolve.
void ensureFoliageMaterialBound(GameContent& content, i32 token) {
    if (!token || content.authoredFor(token)) return;
    const char* name = aver_scene_material_name(token);
    if (!name || !*name) return;
    const pbr::MaterialHandle h = content.materialForSurface(name);
    if (h) content.bindSurfaceMaterial(token, h);
}

// Resolves one part's look EXACTLY the way game::drawWorld's resolveDrawLook does
// (Runtime/src/GameRender.cpp ~209-330) -- authored > dead-handle > named-look > flat-fallback,
// through the same shared aver::game::resolveSurfaceLook (SceneSubmission.hpp) that lambda calls.
// Replicated by hand rather than called directly: resolveDrawLook is a closure private to
// GameRender.cpp (it closes over that function's own `content`/`materials`/`options`), not a free
// function this file could link against, and it is not a file this package owns to factor one out
// of. `out.mesh` is set by the caller; this only fills material/color/metallic/roughness.
//
// Returns false for a TRANSLUCENT part -- the caller drops it (droppedParts++) rather than passing
// it to Voxi, per the shared foliage contract: blended/translucent parts are never in a
// FoliagePrototype (foliage is TLAS-only, and a blended instance has no meaning there).
bool resolveFoliagePartLook(GameContent& content, i32 token, voxi::VoxiRenderer::FoliagePart& out) {
    ensureFoliageMaterialBound(content, token);

    SurfaceInputs in;
    const pbr::MaterialHandle authored = token ? content.authoredFor(token) : 0;
    in.authored = authored != 0;
    const pbr::MaterialDesc* d = authored ? pbr::MaterialLibrary::get().desc(authored) : nullptr;
    in.authoredLive = d != nullptr;
    in.translucent = d != nullptr && pbr::isTranslucent(*d);
    if (in.translucent) return false;

    if (const GameContent::SurfaceLook* builtin = content.lookFor(token)) {
        in.haveLook = true;
        in.lookCol[0] = builtin->col[0];
        in.lookCol[1] = builtin->col[1];
        in.lookCol[2] = builtin->col[2];
        in.lookMetallic = builtin->metallic;
        in.lookRoughness = builtin->roughness;
    }
    const SurfaceLook look = resolveSurfaceLook(in);

    out.material = authored;
    out.color[0] = look.col[0]; out.color[1] = look.col[1];
    out.color[2] = look.col[2]; out.color[3] = look.col[3];
    out.metallic = look.metallic;
    out.roughness = look.roughness;
    return true;
}

// FoliagePrototype's own stated invariant ("at most 16 parts" -- VoxiRenderer.hpp). A mesh's own
// .ocmesh materialSlots table is what could ever produce more than a couple of parts, and nothing
// in this tree authors anywhere near sixteen; this is a backstop against a future one that does,
// not a limit anything here is expected to hit.
constexpr usize kMaxFoliageParts = 16;

// One group's parts, mirroring the no-parts/has-parts split resolveDrawLook's own caller
// (game::drawWorld's per-entity walk) makes through GameContent::partsFor. `droppedParts` is
// incremented by the caller for a part this drops.
void buildFoliageParts(GameContent& content, u64 objectId, rhi::MeshHandle wholeMesh,
                        std::vector<voxi::VoxiRenderer::FoliagePart>& outParts, u32& droppedParts,
                        const std::string& assetName) {
    if (const std::vector<GameContent::MeshPart>* parts = content.partsFor(objectId);
        parts && !parts->empty()) {
        if (parts->size() > kMaxFoliageParts)
            AVER_WARN("[Foliage] '{}' names {} material slots; only the first {} become foliage parts",
                      assetName, parts->size(), kMaxFoliageParts);
        for (const GameContent::MeshPart& mp : *parts) {
            if (outParts.size() >= kMaxFoliageParts) break;
            if (!mp.mesh) continue;   // a slot that named nothing carries no geometry of its own
            voxi::VoxiRenderer::FoliagePart fp;
            fp.mesh = mp.mesh;
            if (resolveFoliagePartLook(content, mp.material, fp)) outParts.push_back(fp);
            else ++droppedParts;
        }
    } else {
        // NO SPLIT: one part, the whole mesh under its own default material -- the mesh's own
        // materialSlots[0], the identical token content.meshDefaultMaterial hands an ordinary
        // unoverridden placement (see GameContent.hpp's own comment on that function).
        voxi::VoxiRenderer::FoliagePart fp;
        fp.mesh = wholeMesh;
        const i32 token = content.meshDefaultMaterial(objectId);
        if (resolveFoliagePartLook(content, token, fp)) outParts.push_back(fp);
        else ++droppedParts;
    }
}

} // namespace

FoliageLoadResult loadLevelFoliage(const fmt::OcWorldData& w, GameContent& content,
                                    voxi::VoxiRenderer* voxi, const std::string& contentDir,
                                    const std::function<void(f32 fraction)>& progress) {
    FoliageLoadResult result;
    if (!voxi) return result;

    if (w.foliageFiles.empty()) {
        voxi->clearFoliage();
        return result;
    }

    const auto started = std::chrono::steady_clock::now();
    std::vector<voxi::VoxiRenderer::FoliagePrototype> prototypes;
    std::vector<voxi::VoxiRenderer::FoliageInstance> instances;

    for (usize fi = 0; fi < w.foliageFiles.size(); ++fi) {
        const std::string& rel = w.foliageFiles[fi];
        const std::string path = resolveContentPath(contentDir, rel);

        fmt::OcInstanceData data;
        std::string why;
        if (!fmt::loadOcInstances(path, data, &why)) {
            AVER_WARN("[Foliage] '{}' could not be read: {}", path, why);
            result.error = why;
            if (progress) progress(static_cast<f32>(fi + 1) / static_cast<f32>(w.foliageFiles.size()));
            continue;
        }
        ++result.files;

        for (const fmt::OcInstanceGroup& g : data.groups) {
            // THE SAME objectId A PLACEMENT NAMING THIS ASSET WOULD CARRY -- OcWorld.cpp interns
            // every PLACE/PLACEG's own objectId as fnv1a64(asset), and content_'s mesh/parts tables
            // are keyed by that identical id, so a species already used as an ordinary placement
            // resolves through the same cache entry here.
            const u64 objectId = fnv1a64(std::string_view(g.asset));
            const rhi::MeshHandle wholeMesh = content.meshFor(objectId);
            if (!wholeMesh) {
                AVER_WARN("[Foliage] '{}' names asset '{}', which is not loaded -- group skipped",
                          path, g.asset);
                continue;   // none of this group's instance range is consumed
            }

            voxi::VoxiRenderer::FoliagePrototype proto;
            buildFoliageParts(content, objectId, wholeMesh, proto.parts, result.droppedParts, g.asset);
            const u32 prototypeIndex = static_cast<u32>(prototypes.size());
            prototypes.push_back(std::move(proto));
            ++result.groups;

            const u64 end = static_cast<u64>(g.first) + g.count;
            for (u64 k = static_cast<u64>(g.first); k < end; ++k) {
                const usize base = static_cast<usize>(k) * 12;
                if (base + 12 > data.transforms.size()) {
                    AVER_WARN("[Foliage] '{}' group '{}' runs past its transform table; the rest of "
                              "it is skipped", path, g.asset);
                    break;
                }
                voxi::VoxiRenderer::FoliageInstance inst;
                // STRAIGHT COPY, NO MATHS: data.transforms and FoliageInstance::world share the
                // identical 12-float row-vector convention (see this function's own header comment
                // and OcInstances.hpp's).
                for (int c = 0; c < 12; ++c) inst.world[c] = data.transforms[base + static_cast<usize>(c)];
                inst.prototype = prototypeIndex;
                instances.push_back(inst);
            }
        }

        if (progress) progress(static_cast<f32>(fi + 1) / static_cast<f32>(w.foliageFiles.size()));
    }

    result.instances = static_cast<u32>(instances.size());
    voxi->setFoliage(std::move(prototypes), std::move(instances));

    const f64 ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - started).count();
    AVER_INFO("[Foliage] {} file(s), {} group(s), {} instance(s), {} translucent part(s) dropped, "
              "{:.1f} ms", result.files, result.groups, result.instances, result.droppedParts, ms);
    return result;
}

} // namespace aver::game

#endif // AVER_MODULE_SCENE && AVER_MODULE_VOXI
