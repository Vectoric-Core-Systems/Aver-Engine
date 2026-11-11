#include "aver/game/LevelHlod.hpp"

#if AVER_MODULE_SCENE
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcHlodBake.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/formats/OcMeshMerge.hpp"
#include "aver/game/GameContent.hpp"
#include "aver/scene/Components.hpp"
#if AVER_MODULE_TRIFACTOR
#  include "aver/trifactor/ClusterBuilder.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace aver::game {

namespace {
// Bump when the same cell would build a different proxy (part of every cache key).
constexpr u32 kHlodRuntimeVersion = 6;
// Main-thread upload time per tick, and at least one proxy.
constexpr f64 kUploadMs = 2.0;
// The worker's source-mesh cache: dropped whole past this many bytes (a city's meshes do not all fit; neighbours share most).
constexpr usize kMeshCacheBytes = 768ull << 20;

std::string slashes(std::string s) { std::replace(s.begin(), s.end(), '\\', '/'); return s; }

usize meshBytes(const fmt::OcMeshData& m) {
    return (m.positions.size() + m.normals.size() + m.uvs.size() + m.tangents.size() + m.colors.size() + m.uvs1.size()) * sizeof(f32) +
           m.indices.size() * sizeof(u32);
}

f64 cellDistance(i32 cx, i32 cy, f32 cellCm, const world::Vec3d& v) {
    const f64 c = static_cast<f64>(cellCm);
    const f64 x0 = cx * c, y0 = cy * c, x1 = x0 + c, y1 = y0 + c;
    const f64 dx = v.x < x0 ? x0 - v.x : (v.x > x1 ? v.x - x1 : 0.0);
    const f64 dy = v.y < y0 ? y0 - v.y : (v.y > y1 ? v.y - y1 : 0.0);
    return std::sqrt(dx * dx + dy * dy);
}
} // namespace

LevelHlod::Settings LevelHlod::settingsFor(const fmt::OcStreamSettings& stream) {
    Settings s;
    s.cellCm = stream.cellCm;
    s.loadCm = stream.loadCm;
    const char* env = std::getenv("AVER_HLOD");
    if ((env && env[0] == '0') || stream.hlodCm < 0.0f) return s;   // hlodCm 0: off
    s.hlodCm = stream.hlodCm > 0.0f ? stream.hlodCm : stream.loadCm * 4.0f;
    s.hlodEvictCm = s.hlodCm + stream.cellCm;
    return s;
}

f32 LevelHlod::errorCmAt(f32 distanceCm, const Settings& s) {
    const f64 halfTan = std::tan(static_cast<f64>(s.fovYDeg) * 0.5 * 3.14159265358979323846 / 180.0);
    const f64 pxCm = 2.0 * static_cast<f64>(distanceCm) * halfTan / std::max(static_cast<f64>(s.viewportPx), 1.0);
    return static_cast<f32>(pxCm * static_cast<f64>(s.pixelError));
}

u32 LevelHlod::levelFor(f64 distanceCm, f32 loadCm, i32 current) {
    // Level k holds from loadCm * 2^k. Coarser only once 10% past a threshold, finer only once 10% inside it, so a viewer standing
    // on one does not swap the mesh every tick.
    const f64 base = std::max(static_cast<f64>(loadCm), 100.0);
    u32 k = 0;
    while (k + 1 < kLevels && distanceCm >= base * static_cast<f64>(1u << (k + 1))) ++k;
    if (current >= 0 && static_cast<u32>(current) != k) {
        const u32 cur = static_cast<u32>(current);
        if (k > cur && distanceCm < base * static_cast<f64>(1u << k) * 1.1) k = cur;
        if (k < cur && distanceCm > base * static_cast<f64>(1u << cur) * 0.9) k = cur;
    }
    return k;
}

LevelHlod::~LevelHlod() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

bool LevelHlod::begin(const fmt::OcWorldData& w, GameContent& content, rhi::IDevice& device, world::PlacementStreamer& streamer,
                      const Settings& s) {
    if (active_ || !(s.hlodCm > 0.0f) || !(s.cellCm >= 1.0f)) return false;
    fmt::OcHlodPlan plan;
    fmt::planHlodCells(w, s.cellCm, plan);
    if (plan.cells.empty()) return false;
    const fmt::ProjectDesc& proj = content.project();
    if (proj.dir.empty()) return false;

    content_ = &content;
    device_ = &device;
    s_ = s;
    // Level k is on screen from loadCm * 2^k (a cell's proxy hides once its content is resident, which loads inside loadCm).
    for (u32 k = 0; k < kLevels; ++k) errorCm_[k] = std::max(errorCmAt(s.loadCm * static_cast<f32>(1u << k), s), 0.5f);
    level_ = w.name.empty() ? std::string("level") : w.name;
    contentDir_ = proj.contentDir();
    cacheDir_ = (std::filesystem::path(proj.dir) / "Saved" / "DerivedDataCache" / "HLOD" / level_).string();
    records_ = w.placements;
    matrices_ = std::move(plan.matrices);
    cells_.clear();
    cells_.reserve(plan.cells.size());
    std::vector<world::PlacementStreamer::HlodCell> ring;
    ring.reserve(plan.cells.size());
    for (fmt::OcHlodPlan::Cell& pc : plan.cells) {
        Cell c;
        c.cx = pc.cx;
        c.cy = pc.cy;
        c.placements = std::move(pc.placements);
        cells_.push_back(std::move(c));
        ring.push_back(world::PlacementStreamer::HlodCell{pc.cx, pc.cy});
    }
    streamer.buildHlod(ring);
    stop_ = false;
    haveViewer_ = false;
    built_ = cached_ = failed_ = 0;
    reported_ = false;
    active_ = true;
    worker_ = std::thread([this] { work(); });
    AVER_INFO("[HLOD] {} cell(s) of {:.0f} m get merged proxies, shown out to {:.0f} m; {} levels at {:.1f}/{:.1f}/{:.1f}/{:.1f} cm "
              "({:.1f} px from {:.0f} m, doubling); cache {}", cells_.size(), s.cellCm / 100.0f, s.hlodCm / 100.0f, kLevels, errorCm_[0],
              errorCm_[1], errorCm_[2], errorCm_[3], s.pixelError, s.loadCm / 100.0f, slashes(cacheDir_));
    return true;
}

void LevelHlod::end(scene::World& world) {
    if (!active_) return;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    for (Cell& c : cells_) despawn(world, c);
    cells_.clear();
    entities_.clear();
    records_.clear();
    matrices_.clear();
    active_ = false;
}

// Builds (or finds in the cache) every level of a cell; fills c.files. Worker thread; reads only what begin() fixed.
bool LevelHlod::buildCell(u32 index, bool& fromCache) {
    fromCache = false;
    Cell& c = cells_[index];
    // Key: what went into the proxy. Source meshes by path, size and write time (reading them to hash would cost the build).
    std::string key = "v" + std::to_string(kHlodRuntimeVersion) + "|" + std::to_string(s_.cellCm) + "|";
    for (u32 k = 0; k < kLevels; ++k) key += std::to_string(errorCm_[k]) + ",";
    char buf[96];
    std::error_code ec;
    for (const u32 i : c.placements) {
        const fmt::OcWorldPlacement& p = records_[i];
        const std::filesystem::path src = std::filesystem::path(contentDir_) / p.asset;
        const auto size = std::filesystem::file_size(src, ec);
        const auto when = ec ? 0 : std::filesystem::last_write_time(src, ec).time_since_epoch().count();
        std::snprintf(buf, sizeof buf, "%" PRIu64 ",%" PRId64 "|", static_cast<uint64_t>(ec ? 0 : size), static_cast<int64_t>(when));
        key += p.asset; key += '|'; key += p.material; key += '|'; key += buf;
        for (int k = 0; k < 12; ++k) { std::snprintf(buf, sizeof buf, "%.2f,", matrices_[i * 12 + k]); key += buf; }
        key += ';';
    }
    const u64 hash = fnv1a64(std::string_view(key));
    std::string files[kLevels];
    bool all = true;
    for (u32 k = 0; k < kLevels; ++k) {
        std::snprintf(buf, sizeof buf, "c_%d_%d_%016" PRIx64 "_L%u.ocmesh", c.cx, c.cy, static_cast<uint64_t>(hash), k);
        files[k] = (std::filesystem::path(cacheDir_) / buf).string();
        all = all && std::filesystem::exists(files[k], ec);
    }
    if (all) {
        std::lock_guard<std::mutex> lk(mutex_);
        for (u32 k = 0; k < kLevels; ++k) c.files[k] = files[k];
        fromCache = true;
        return true;
    }

    static thread_local std::unordered_map<std::string, std::unique_ptr<fmt::OcMeshData>> meshes;
    static thread_local usize meshesBytes = 0;
    std::vector<fmt::OcMergeItem> items;
    items.reserve(c.placements.size());
    for (const u32 i : c.placements) {
        const fmt::OcWorldPlacement& p = records_[i];
        auto it = meshes.find(p.asset);
        if (it == meshes.end()) {
            auto m = std::make_unique<fmt::OcMeshData>();
            if (!fmt::loadOcMesh((std::filesystem::path(contentDir_) / p.asset).string(), *m)) m.reset();
            if (m) meshesBytes += meshBytes(*m);
            it = meshes.emplace(p.asset, std::move(m)).first;
        }
        if (!it->second || it->second->hasSkin()) continue;
        fmt::OcMergeItem item;
        item.mesh = it->second.get();
        std::copy_n(&matrices_[i * 12], 12, item.m);
        item.material = p.material;
        items.push_back(std::move(item));
    }
    bool ok = false;
    if (!items.empty()) {
        fmt::OcHlodOptions opt;
        // Interiors and props inside another item's bounds are not seen from a proxy's distance, and are much of a city's
        // triangles. What is smaller than the finest error goes too.
        opt.dropEnclosed = true;
        opt.dropSmallerCm = errorCm_[0];
        opt.maxErrorCm = errorCm_[0];
        static std::atomic<u32> simplifyWarnings{0};
#if AVER_MODULE_TRIFACTOR
        opt.simplifyError = [](fmt::OcMeshData& m, f32 e) {
            std::string why;
            const bool ok = trifactor::simplifyToError(m, e, &why);
            if (!ok && simplifyWarnings.fetch_add(1) < 4) AVER_WARN("[HLOD] a proxy kept every triangle: {}", why);
            return ok;
        };
#endif
        fmt::OcHlodCellBuild b;
        std::string why;
        ok = fmt::buildHlodProxy(items, static_cast<f64>(c.cx) * s_.cellCm, static_cast<f64>(c.cy) * s_.cellCm, s_.cellCm, s_.hlodCm,
                                 opt, b, &why) && !b.mesh.indices.empty();
        std::filesystem::create_directories(cacheDir_, ec);
        // Each coarser level simplifies the one before it: the error bounds double, so the work halves as it goes.
        usize tris[kLevels] = {};
        for (u32 k = 0; ok && k < kLevels; ++k) {
#if AVER_MODULE_TRIFACTOR
            if (k > 0) {
                fmt::OcMeshData coarser = b.mesh;
                if (opt.simplifyError(coarser, errorCm_[k]) && !coarser.indices.empty()) b.mesh = std::move(coarser);
                b.mesh.computeBounds();
            }
#endif
            tris[k] = b.mesh.indices.size() / 3;
            ok = fmt::saveOcMesh(files[k], b.mesh);
        }
        static std::atomic<u32> sampleLogs{0};
        if (ok && sampleLogs.fetch_add(1) < 6)
            AVER_INFO("[HLOD] cell {},{}: {} placement(s), {} triangles merged -> {}/{}/{}/{} by level, {} material(s)", c.cx, c.cy,
                      c.placements.size(), b.trianglesIn, tris[0], tris[1], tris[2], tris[3], b.mesh.materialSlots.size());
        if (ok) {
            std::lock_guard<std::mutex> lk(mutex_);
            for (u32 k = 0; k < kLevels; ++k) c.files[k] = files[k];
        }
    }
    if (meshesBytes > kMeshCacheBytes) { meshes.clear(); meshesBytes = 0; }
    return ok;
}

void LevelHlod::work() {
    for (;;) {
        u32 pick = ~0u;
        bool load = false;
        std::string file;
        i32 level = -1;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            for (;;) {
                if (stop_) return;
                // A level the main thread is waiting for comes first: it is on screen (or about to be).
                f64 best = 3.0e38;
                for (u32 i = 0; i < cells_.size(); ++i) {
                    const Cell& c = cells_[i];
                    if (c.loadLevel < 0 || c.state != State::Built || c.loaded) continue;
                    const f64 d = haveViewer_ ? cellDistance(c.cx, c.cy, s_.cellCm, viewer_) : 0.0;
                    if (d < best) { best = d; pick = i; }
                }
                if (pick != ~0u) {
                    load = true;
                    level = cells_[pick].loadLevel;
                    file = cells_[pick].files[level];
                    break;
                }
                if (haveViewer_) {
                    // Then the nearest unbuilt cell within the hide distance (a little past it, so a proxy is ready before it is asked for).
                    best = static_cast<f64>(std::max(s_.hlodEvictCm, s_.hlodCm)) * 1.25;
                    for (u32 i = 0; i < cells_.size(); ++i) {
                        if (cells_[i].state != State::Unbuilt) continue;
                        const f64 d = cellDistance(cells_[i].cx, cells_[i].cy, s_.cellCm, viewer_);
                        if (d < best) { best = d; pick = i; }
                    }
                }
                if (pick != ~0u) { cells_[pick].state = State::Building; break; }
                wake_.wait_for(lk, std::chrono::milliseconds(250));
            }
        }
        if (load) {
            auto mesh = std::make_unique<fmt::OcMeshData>();
            const bool ok = fmt::loadOcMesh(file, *mesh);
            std::lock_guard<std::mutex> lk(mutex_);
            Cell& c = cells_[pick];
            if (ok && c.loadLevel == level) { c.loaded = std::move(mesh); c.loadedLevel = level; }
            else if (!ok) c.state = State::Empty;
            continue;
        }
        bool fromCache = false;
        const bool ok = buildCell(pick, fromCache);
        std::lock_guard<std::mutex> lk(mutex_);
        cells_[pick].state = ok ? State::Built : State::Empty;
        if (ok) (fromCache ? cached_ : built_)++;
        else    ++failed_;
    }
}

void LevelHlod::show(scene::World& world, Cell& c, fmt::OcMeshData& mesh, i32 level, world::OriginCm origin) {
    char rel[112];
    std::snprintf(rel, sizeof rel, "@hlod/%s/%d_%d_L%d", level_.c_str(), c.cx, c.cy, level);
    const u64 id = fnv1a64(std::string_view(rel));
    if (!content_->uploadBuiltMesh(*device_, id, mesh, rel)) {
        AVER_WARN("[HLOD] the level-{} proxy of cell {},{} did not upload", level, c.cx, c.cy);
        return;
    }
    if (c.entity == scene::kInvalidEntity || !world.valid(c.entity)) {
        Transform xf;
        // Proxy vertices are relative to the cell's minimum corner (z 0), placed here in the scene frame.
        xf.position = Vec3{static_cast<f32>(static_cast<f64>(c.cx) * s_.cellCm - static_cast<f64>(origin.x)),
                           static_cast<f32>(static_cast<f64>(c.cy) * s_.cellCm - static_cast<f64>(origin.y)), 0.0f};
        char name[64];
        std::snprintf(name, sizeof name, "@hlod %d,%d", c.cx, c.cy);
        c.entity = world.create(name, scene::kInvalidEntity, xf);
        if (c.entity == scene::kInvalidEntity) { content_->releaseBuiltMesh(id); return; }
        entities_[static_cast<u32>(c.entity)] = static_cast<u32>(&c - cells_.data());
        if (auto* mr = static_cast<scene::CMeshRenderer*>(world.addComponent(c.entity, scene::kComponentMeshRenderer))) {
            mr->flags |= scene::kMeshRendererVisible;
            // Out of the voxel GI bake: proxies come and go with distance, and the bake is for the content near the volume.
            mr->renderFlags = scene::kRenderGiDisabled << scene::kRenderGiShift;
        }
    }
    if (auto* mr = world.component<scene::CMeshRenderer>(c.entity, scene::kComponentMeshRenderer)) {
        mr->mesh = id;
        const Vec3 lo = mesh.boundsMin, hi = mesh.boundsMax;
        mr->aabbMin[0] = lo.x; mr->aabbMin[1] = lo.y; mr->aabbMin[2] = lo.z;
        mr->aabbMax[0] = hi.x; mr->aabbMax[1] = hi.y; mr->aabbMax[2] = hi.z;
        mr->dirty = 1;
    }
    if (c.meshId && c.meshId != id) content_->releaseBuiltMesh(c.meshId);   // the level it replaces
    c.meshId = id;
    c.shownLevel = level;
}

void LevelHlod::despawn(scene::World& world, Cell& c) {
    if (c.entity != scene::kInvalidEntity) {
        entities_.erase(static_cast<u32>(c.entity));
        if (world.valid(c.entity)) world.destroy(c.entity);
        c.entity = scene::kInvalidEntity;
    }
    if (c.meshId) { content_->releaseBuiltMesh(c.meshId); c.meshId = 0; }
    c.shownLevel = -1;
}

void LevelHlod::tick(scene::World& world, world::PlacementStreamer& streamer, const std::vector<world::Vec3d>& viewers,
                     world::OriginCm origin) {
    if (!active_) return;
    std::vector<u32> toShow, toHide;
    streamer.updateHlod(viewers, toShow, toHide);
    for (const u32 i : toHide) {
        if (i >= cells_.size()) continue;
        cells_[i].wanted = false;
        despawn(world, cells_[i]);
    }
    for (const u32 i : toShow) if (i < cells_.size()) cells_[i].wanted = true;

    // Each wanted cell asks for the level its distance allows; what the worker has read goes up, nearest first, within kUploadMs.
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    std::vector<std::pair<f64, u32>> due;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!viewers.empty()) { viewer_ = viewers.front(); haveViewer_ = true; }
        for (u32 i = 0; i < cells_.size(); ++i) {
            Cell& c = cells_[i];
            if (!c.wanted) {
                c.loadLevel = -1;
                c.loaded.reset();
                continue;
            }
            if (c.state != State::Built) continue;
            const f64 d = viewers.empty() ? 0.0 : cellDistance(c.cx, c.cy, s_.cellCm, viewers.front());
            const i32 want = static_cast<i32>(levelFor(d, s_.loadCm, c.shownLevel));
            if (want == c.shownLevel) { c.loadLevel = -1; c.loaded.reset(); continue; }
            if (c.loaded && c.loadedLevel == want) due.emplace_back(d, i);
            else if (c.loadLevel != want) { c.loadLevel = want; c.loaded.reset(); }
        }
    }
    wake_.notify_one();
    std::sort(due.begin(), due.end());
    u32 uploaded = 0;
    for (const auto& [d, i] : due) {
        if (uploaded > 0 && std::chrono::duration<f64, std::milli>(Clock::now() - start).count() >= kUploadMs) break;
        std::unique_ptr<fmt::OcMeshData> mesh;
        i32 level = -1;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            mesh = std::move(cells_[i].loaded);
            level = cells_[i].loadedLevel;
            cells_[i].loadLevel = -1;
        }
        if (!mesh) continue;
        show(world, cells_[i], *mesh, level, origin);
        ++uploaded;
    }
    if (!reported_) {
        std::lock_guard<std::mutex> lk(mutex_);
        bool idle = true;
        for (const Cell& c : cells_)
            if ((c.state == State::Unbuilt || c.state == State::Building) && !viewers.empty() &&
                cellDistance(c.cx, c.cy, s_.cellCm, viewers.front()) < static_cast<f64>(s_.hlodEvictCm)) { idle = false; break; }
        if (idle && (built_ + cached_ + failed_) > 0) {
            reported_ = true;
            u32 levels[kLevels] = {};
            for (const Cell& c : cells_) if (c.shownLevel >= 0) ++levels[c.shownLevel];
            AVER_INFO("[HLOD] proxies within {:.0f} m ready: {} built, {} from the cache, {} empty; {} shown (levels {}/{}/{}/{})",
                      s_.hlodEvictCm / 100.0f, built_, cached_, failed_, entities_.size(), levels[0], levels[1], levels[2], levels[3]);
        }
    }
}

} // namespace aver::game

#endif // AVER_MODULE_SCENE
