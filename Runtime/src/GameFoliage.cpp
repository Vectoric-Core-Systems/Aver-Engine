#include "aver/game/GameFoliage.hpp"
#include "aver/core/Types.hpp"   // AVER_PATH_SEP

#if AVER_MODULE_SCENE && AVER_MODULE_VOXI

#include "aver/game/GameContent.hpp"
#include "aver/game/SceneSubmission.hpp"
#include "aver/voxi/VoxiRenderer.hpp"
#include "aver/formats/OcInstances.hpp"
#include "aver/pbr/Material.hpp"
#include "aver/scene/scene_abi.h"
#include "aver/core/Hash.hpp"
#include "aver/core/HitchMarks.hpp"
#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"   // toNativeSeparators

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string_view>
#include <utility>
#include <vector>

namespace aver::game {

namespace {

// Resolves `contentDir`/`relPath` the same way GameContent::resolveMaterialGraph resolves a
// GRAPHREF: a plain join, forward slashes normalised to the native separator so a level authored
// on one convention still opens on the other.
std::string resolveContentPath(const std::string& contentDir, const std::string& relPath) {
    std::string path = contentDir + AVER_PATH_SEP + relPath;
    toNativeSeparators(path);
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
                        const std::string& assetName, const std::string& materialOverride = std::string()) {
    // [wave3/level-instances] An extra table group may name a material that replaces every part's own.
    const i32 overrideToken = materialOverride.empty() ? 0 : aver_scene_material(0, materialOverride.c_str());
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
            if (resolveFoliagePartLook(content, overrideToken ? overrideToken : mp.material, fp)) outParts.push_back(fp);
            else ++droppedParts;
        }
    } else {
        // NO SPLIT: one part, the whole mesh under its own default material -- the mesh's own
        // materialSlots[0], the identical token content.meshDefaultMaterial hands an ordinary
        // unoverridden placement (see GameContent.hpp's own comment on that function).
        voxi::VoxiRenderer::FoliagePart fp;
        fp.mesh = wholeMesh;
        const i32 token = overrideToken ? overrideToken : content.meshDefaultMaterial(objectId);
        if (resolveFoliagePartLook(content, token, fp)) outParts.push_back(fp);
        else ++droppedParts;
    }
}

} // namespace

bool LevelFoliage::load(const fmt::OcWorldData& w, GameContent& content, rhi::IDevice* device,
                        voxi::VoxiRenderer* voxi, const std::string& contentDir,
                        const std::function<void(f32 fraction)>& progress, const Vec3& viewerCm,
                        const std::vector<std::string>* tablePaths) {
    clearAll(false);   // [wave3/level-instances] extra tables outlive a load: a level instance sets them from inside the level's own load
    result_ = {};
    content_ = &content;
    device_ = device;
    voxi_ = voxi;
    if (!voxi) return true;

    if (w.foliageFiles.empty() && extras_.empty()) {
        voxi->clearFoliage();
        loaded_ = true;
        return true;
    }

    for (usize fi = 0; fi < w.foliageFiles.size(); ++fi) {
        File f;
        f.path = (tablePaths && fi < tablePaths->size() && !(*tablePaths)[fi].empty())
                     ? (*tablePaths)[fi] : resolveContentPath(contentDir, w.foliageFiles[fi]);
        std::string why;
        if (!fmt::loadOcInstances(f.path, f.data, &why)) {
            AVER_WARN("[Foliage] '{}' could not be read: {}", f.path, why);
            result_.error = why;
            if (progress) progress(static_cast<f32>(fi + 1) / static_cast<f32>(w.foliageFiles.size()));
            continue;
        }
        ++result_.files;
        const usize ng = f.data.groups.size();
        f.objectIds.resize(ng);
        for (usize g = 0; g < ng; ++g)
            f.objectIds[g] = fnv1a64(std::string_view(f.data.groups[g].asset));   // as a placement's objectId
        f.use.assign(ng, 0);
        f.held.assign(ng, 0);
        f.warned.assign(ng, 0);
        const u32 fileIndex = static_cast<u32>(files_.size());
        for (usize c = 0; c < f.data.cells.size(); ++c) cells_.push_back({fileIndex, static_cast<u32>(c)});
        files_.push_back(std::move(f));
        if (progress) progress(static_cast<f32>(fi + 1) / static_cast<f32>(w.foliageFiles.size()));
    }

    syncExtraFiles();   // [wave3/level-instances]
    extrasDirty_ = false;

    loadCm_ = w.stream.loadCm;
    evictCm_ = std::max(w.stream.evictCm, w.stream.loadCm);
    streamed_ = w.stream.enabled && device && !cells_.empty();
    loaded_ = true;
    accum_ = 0;

    if (streamed_) {
        resident_.assign(cells_.size(), 0);
        update(viewerCm, kCheckSeconds);   // first residency is immediate
    } else {
        const std::vector<char> all(cells_.size(), 1);
        if (device_) {
            applyResidency(all);   // acquires every emitted group's mesh
        } else {
            resident_ = all;
            residentCount_ = static_cast<u32>(cells_.size());
            rebuild();
        }
    }
    return result_.error.empty();
}

void LevelFoliage::update(const Vec3& viewerCm, f32 dt) {
    if (!loaded_) return;
    // [wave3/level-instances begin] A changed extra-table set on a level that does not stream cells.
    if (!streamed_) {
        if (!extrasDirty_ || !voxi_) return;
        accum_ += dt;
        if (accum_ < kCheckSeconds) return;
        accum_ = 0;
        if (extrasReady()) applyExtras();
        return;
    }
    // [wave3/level-instances end]
    accum_ += dt;
    if (accum_ < kCheckSeconds) return;
    accum_ = 0;
    if (extrasDirty_) syncExtraFiles();   // [wave3/level-instances] the tail files follow the table set

    std::vector<char> next(cells_.size(), 0), soon(cells_.size(), 0);
    bool changed = false;
    for (usize i = 0; i < cells_.size(); ++i) {
        const File& cf = files_[cells_[i].file];
        const fmt::OcInstanceCell& c = cf.data.cells[cells_[i].cell];
        // Cell bounds are in the table's frame, the viewer in the scene's (zero apart at origin zero).
        const f32 sx = static_cast<f32>(frame_.x - cf.frameX), sy = static_cast<f32>(frame_.y - cf.frameY);
        const f32 dx = std::max({c.min[0] - sx - viewerCm.x, 0.0f, viewerCm.x - (c.max[0] - sx)});
        const f32 dy = std::max({c.min[1] - sy - viewerCm.y, 0.0f, viewerCm.y - (c.max[1] - sy)});
        const f32 d = std::sqrt(dx * dx + dy * dy);
        next[i] = d <= (resident_[i] ? evictCm_ : loadCm_) ? 1 : 0;   // hysteresis
        soon[i] = d <= loadCm_ * kPrefetchFactor ? 1 : 0;
        if (next[i] != resident_[i]) changed = true;
    }
    if (extrasDirty_) changed = true;   // [wave3/level-instances]
    // Prototype meshes of cells about to load are read on GameContent's workers ahead of time, nearest
    // first; a residency change waits until every mesh it adds is held, so nothing loads on this thread.
    // Each read mesh is taken at once (within kAcquireMs) rather than when the whole set is read: the
    // reads wait in GameContent's shared, capped pool, and a set larger than the pool (a jump into dense
    // foliage) would otherwise fill it with reads nobody takes and stall level streaming with it.
    bool ready = true;
    const auto takeStart = std::chrono::steady_clock::now();
    for (usize i = 0; i < cells_.size(); ++i) {
        if (!soon[i]) continue;
        File& f = files_[cells_[i].file];
        const fmt::OcInstanceCell& c = f.data.cells[cells_[i].cell];
        const u64 re = static_cast<u64>(c.firstRun) + c.runCount;
        for (u64 r = c.firstRun; r < re && r < f.data.runs.size(); ++r) {
            const u32 g = f.data.runs[static_cast<usize>(r)].group;
            if (g >= f.held.size() || f.held[g]) continue;
            content_->prefetchMesh(f.objectIds[g], next[i] ? 0.0f : loadCm_);
            if (!next[i]) continue;
            if (rebuilds_ > 0 && content_->meshReady(f.objectIds[g]) &&
                std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - takeStart).count() < kAcquireMs) {
                content_->acquireTag = "foliage";
                if (content_->acquireMesh(*device_, f.objectIds[g])) f.held[g] = 1;
                content_->acquireTag = nullptr;
            }
            if (!f.held[g]) ready = false;
        }
    }
    if (extrasDirty_ && !extrasReady()) ready = false;   // [wave3/level-instances]
    if (!changed && rebuilds_ > 0) return;
    if (!ready && rebuilds_ > 0) { accum_ = kCheckSeconds; return; }   // look again next frame
    // The meshes a change adds go to the GPU within kAcquireMs a frame (one frame for all was 45-75 ms);
    // the change applies once every one is held.
    if (rebuilds_ > 0) {
        const auto start = std::chrono::steady_clock::now();
        bool first = true;
        for (usize i = 0; i < cells_.size(); ++i) {
            if (!next[i] || resident_[i]) continue;
            File& f = files_[cells_[i].file];
            const fmt::OcInstanceCell& c = f.data.cells[cells_[i].cell];
            const u64 re = static_cast<u64>(c.firstRun) + c.runCount;
            for (u64 r = c.firstRun; r < re && r < f.data.runs.size(); ++r) {
                const u32 g = f.data.runs[static_cast<usize>(r)].group;
                if (g >= f.held.size() || f.held[g]) continue;
                if (!first && std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count() >=
                                  kAcquireMs) {
                    accum_ = kCheckSeconds;
                    return;
                }
                first = false;
                content_->acquireTag = "foliage";
                if (content_->acquireMesh(*device_, f.objectIds[g])) f.held[g] = 1;
                content_->acquireTag = nullptr;
            }
        }
        // Their ray-tracing structures next, built by the renderer within its own budget a frame
        // (all of a new area's at once was ~200 ms of CPU and ~130 ms of GPU).
        bool built = true;
        for (usize i = 0; i < cells_.size(); ++i) {
            if (!next[i] || resident_[i]) continue;
            File& f = files_[cells_[i].file];
            const fmt::OcInstanceCell& c = f.data.cells[cells_[i].cell];
            const u64 re = static_cast<u64>(c.firstRun) + c.runCount;
            for (u64 r = c.firstRun; r < re && r < f.data.runs.size(); ++r) {
                const u32 g = f.data.runs[static_cast<usize>(r)].group;
                if (g >= f.held.size() || !f.held[g]) continue;
                const rhi::MeshHandle wholeMesh = content_->meshFor(f.objectIds[g]);
                if (!wholeMesh) continue;
                voxi::VoxiRenderer::FoliagePrototype proto;
                u32 dropped = 0;
                buildFoliageParts(*content_, f.objectIds[g], wholeMesh, proto.parts, dropped, f.data.groups[g].asset);
                if (!voxi_->prepareFoliagePrototype(proto)) built = false;
                if (!built && std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count() >=
                                  kAcquireMs) {
                    accum_ = kCheckSeconds;
                    return;
                }
            }
        }
        if (!built) { accum_ = kCheckSeconds; return; }
    }
    applyResidency(next);
    extrasDirty_ = false;   // [wave3/level-instances]
    releaseDroppedExtras();
}

void LevelFoliage::applyResidency(const std::vector<char>& next) {
    for (File& f : files_) std::fill(f.use.begin(), f.use.end(), f.data.cells.empty() ? 1u : 0u);   // cell-less files are always resident
    u32 count = 0;
    for (usize i = 0; i < cells_.size(); ++i) {
        if (!next[i]) continue;
        ++count;
        File& f = files_[cells_[i].file];
        const fmt::OcInstanceCell& c = f.data.cells[cells_[i].cell];
        const u64 re = static_cast<u64>(c.firstRun) + c.runCount;
        for (u64 r = c.firstRun; r < re && r < f.data.runs.size(); ++r) {
            const u32 g = f.data.runs[static_cast<usize>(r)].group;
            if (g < f.use.size()) ++f.use[g];
        }
    }
    HitchMarks hm("foliage residency", 0.25);
    // Acquire before the rebuild so the new meshes exist; release after it so the old BLASes are gone first.
    for (File& f : files_)
        for (usize g = 0; g < f.use.size(); ++g)
            if (f.use[g] && !f.held[g]) {
                content_->acquireTag = "foliage";
                if (content_->acquireMesh(*device_, f.objectIds[g])) f.held[g] = 1;
                content_->acquireTag = nullptr;
            }
    hm.mark("acquire");
    resident_ = next;
    residentCount_ = count;
    rebuild();
    hm.mark("rebuild");
    for (File& f : files_)
        for (usize g = 0; g < f.use.size(); ++g)
            if (!f.use[g] && f.held[g]) { content_->releaseMesh(*device_, f.objectIds[g]); f.held[g] = 0; }
    hm.mark("release");
}

void LevelFoliage::rebuild() {
    const auto started = std::chrono::steady_clock::now();
    std::vector<voxi::VoxiRenderer::FoliagePrototype> prototypes;
    std::vector<voxi::VoxiRenderer::FoliageInstance> instances;
    std::vector<std::vector<i32>> protoOf(files_.size());   // -1 = not built yet, -2 = unusable
    for (usize fi = 0; fi < files_.size(); ++fi) protoOf[fi].assign(files_[fi].data.groups.size(), -1);
    u32 droppedParts = 0;

    auto emit = [&](usize fi, u32 group, u32 first, u32 count) {
        File& f = files_[fi];
        if (group >= f.data.groups.size()) return;
        i32& slot = protoOf[fi][group];
        const std::string& asset = f.data.groups[group].asset;
        const f32 shiftX = static_cast<f32>(frame_.x - f.frameX), shiftY = static_cast<f32>(frame_.y - f.frameY);
        if (slot == -1) {
            const rhi::MeshHandle wholeMesh = content_->meshFor(f.objectIds[group]);
            if (!wholeMesh) {
                if (!f.warned[group]) {
                    f.warned[group] = 1;
                    AVER_WARN("[Foliage] '{}' names asset '{}', which is not loaded -- group skipped",
                              f.path, asset);
                }
                slot = -2;
            } else {
                voxi::VoxiRenderer::FoliagePrototype proto;
                buildFoliageParts(*content_, f.objectIds[group], wholeMesh, proto.parts, droppedParts, asset,
                                  group < f.groupMaterial.size() ? f.groupMaterial[group] : std::string());
                slot = static_cast<i32>(prototypes.size());
                prototypes.push_back(std::move(proto));
            }
        }
        if (slot < 0) return;
        const u64 end = static_cast<u64>(first) + count;
        for (u64 k = first; k < end; ++k) {
            const usize base = static_cast<usize>(k) * 12;
            if (base + 12 > f.data.transforms.size()) {
                AVER_WARN("[Foliage] '{}' group '{}' runs past its transform table; the rest of "
                          "it is skipped", f.path, asset);
                break;
            }
            voxi::VoxiRenderer::FoliageInstance inst;
            // Straight copy: same 12-float row-vector convention as FoliageInstance::world.
            for (int c = 0; c < 12; ++c) inst.world[c] = f.data.transforms[base + static_cast<usize>(c)];
            // Floating origin: the translation row (9..11) into the scene frame, a plain f32 subtraction like the scene's roots.
            if (shiftX != 0.0f) inst.world[9] -= shiftX;
            if (shiftY != 0.0f) inst.world[10] -= shiftY;
            inst.prototype = static_cast<u32>(slot);
            instances.push_back(inst);
        }
    };

    for (usize fi = 0; fi < files_.size(); ++fi) {
        const fmt::OcInstanceData& d = files_[fi].data;
        if (!d.cells.empty()) continue;   // cell files are walked below
        for (usize g = 0; g < d.groups.size(); ++g)
            emit(fi, static_cast<u32>(g), d.groups[g].first, d.groups[g].count);
    }
    for (usize i = 0; i < cells_.size(); ++i) {
        if (!resident_[i]) continue;
        const fmt::OcInstanceData& d = files_[cells_[i].file].data;
        const fmt::OcInstanceCell& c = d.cells[cells_[i].cell];
        const u64 re = static_cast<u64>(c.firstRun) + c.runCount;
        for (u64 r = c.firstRun; r < re && r < d.runs.size(); ++r) {
            const fmt::OcInstanceRun& run = d.runs[static_cast<usize>(r)];
            emit(cells_[i].file, run.group, run.first, run.count);
        }
    }

    result_.groups = static_cast<u32>(prototypes.size());
    result_.instances = static_cast<u32>(instances.size());
    result_.droppedParts = droppedParts;
    HitchMarks hm("foliage setFoliage", 0.25);
    voxi_->setFoliage(std::move(prototypes), std::move(instances));
    hm.mark("setFoliage");
    ++rebuilds_;

    lastRebuildMs_ = static_cast<f32>(std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - started).count());
    AVER_INFO("[Foliage] {} file(s), {}/{} cell(s) resident, {} group(s), {} instance(s), {} translucent "
              "part(s) dropped, {:.1f} ms", result_.files, residentCount_, cells_.size(), result_.groups,
              result_.instances, result_.droppedParts, lastRebuildMs_);
}

void LevelFoliage::clear() { clearAll(true); }

// The scene origin moved (or was named). Level tables are absolute and extras keep the frame they were set in, so only the
// shift each table is read with changes: push the resident rows again.
void LevelFoliage::setFrameOrigin(const rhi::ShiftOrigin& o) {
    if (o.x == frame_.x && o.y == frame_.y) return;
    frame_ = o;
    if (!loaded_ || !voxi_ || !content_ || rebuilds_ == 0) return;   // nothing pushed yet: the next push is already in the new frame
    HitchMarks hm("foliage origin shift", 0.25);
    rebuild();
    hm.mark("rebuild");
}

void LevelFoliage::clearAll(bool dropExtras) {
    if (device_ && content_)
        for (File& f : files_)
            for (usize g = 0; g < f.held.size(); ++g)
                if (f.held[g]) content_->releaseMesh(*device_, f.objectIds[g]);
    if (voxi_ && loaded_) voxi_->clearFoliage();
    files_.clear();
    cells_.clear();
    resident_.clear();
    residentCount_ = 0;
    streamed_ = false;
    loaded_ = false;
    accum_ = 0;
    rebuilds_ = 0;
    lastRebuildMs_ = 0;
    // [wave3/level-instances begin]
    if (dropExtras) { extras_.clear(); extrasDirty_ = false; }
    extraRelease_.clear();
    // [wave3/level-instances end]
}

// [wave3/level-instances begin]
void LevelFoliage::setExtraTable(u64 owner, fmt::OcInstanceData rows, std::vector<std::string> groupMaterial) {
    for (Extra& e : extras_)
        if (e.owner == owner) {
            e.frameX = frame_.x; e.frameY = frame_.y;
            e.data = std::move(rows);
            e.groupMaterial = std::move(groupMaterial);
            extrasDirty_ = true;
            return;
        }
    Extra e;
    e.frameX = frame_.x; e.frameY = frame_.y;
    e.owner = owner;
    e.data = std::move(rows);
    e.groupMaterial = std::move(groupMaterial);
    extras_.push_back(std::move(e));
    extrasDirty_ = true;
}

void LevelFoliage::clearExtraTable(u64 owner) {
    const auto it = std::remove_if(extras_.begin(), extras_.end(), [owner](const Extra& e) { return e.owner == owner; });
    if (it == extras_.end()) return;
    extras_.erase(it, extras_.end());
    extrasDirty_ = true;
}

void LevelFoliage::syncExtraFiles() {
    // Extra files are always the tail of files_, so the cell references into the level's own files stay valid.
    while (!files_.empty() && files_.back().extra) {
        File& f = files_.back();
        for (usize g = 0; g < f.held.size(); ++g)
            if (f.held[g]) extraRelease_.push_back(f.objectIds[g]);
        files_.pop_back();
    }
    for (const Extra& e : extras_) {
        File f;
        f.extra = true;
        f.frameX = e.frameX; f.frameY = e.frameY;
        f.data = e.data;
        f.groupMaterial = e.groupMaterial;
        f.path = "(level instance table)";
        const usize ng = f.data.groups.size();
        f.objectIds.resize(ng);
        for (usize g = 0; g < ng; ++g) f.objectIds[g] = fnv1a64(std::string_view(f.data.groups[g].asset));
        f.use.assign(ng, 0);
        f.held.assign(ng, 0);
        f.warned.assign(ng, 0);
        files_.push_back(std::move(f));
    }
}

bool LevelFoliage::extrasReady() {
    if (!content_ || !device_) return true;
    bool ready = true;
    for (const Extra& e : extras_)
        for (const fmt::OcInstanceGroup& g : e.data.groups) {
            const u64 id = fnv1a64(std::string_view(g.asset));
            content_->prefetchMesh(id, 0.0f);
            if (!content_->meshReady(id)) ready = false;
        }
    return ready;
}

void LevelFoliage::releaseDroppedExtras() {
    if (device_ && content_) for (const u64 id : extraRelease_) content_->releaseMesh(*device_, id);
    extraRelease_.clear();
}

void LevelFoliage::applyExtras() {
    syncExtraFiles();
    extrasDirty_ = false;
    if (device_) applyResidency(std::vector<char>(cells_.size(), 1));   // acquires the new tables' meshes, then rebuilds
    else rebuild();
    releaseDroppedExtras();
}
// [wave3/level-instances end]

FoliageLoadResult loadLevelFoliage(const fmt::OcWorldData& w, GameContent& content,
                                    voxi::VoxiRenderer* voxi, const std::string& contentDir,
                                    const std::function<void(f32 fraction)>& progress) {
    LevelFoliage lf;   // no device: everything resident, nothing acquired -- one push, as before
    lf.load(w, content, nullptr, voxi, contentDir, progress);
    return lf.result();
}

} // namespace aver::game

#endif // AVER_MODULE_SCENE && AVER_MODULE_VOXI
