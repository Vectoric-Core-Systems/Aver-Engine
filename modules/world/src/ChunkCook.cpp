#include "aver/world/ChunkCook.hpp"

#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Time.hpp"
#include "aver/formats/OcLevelInst.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/world/ChunkCodec.hpp"
#include "aver/world/ChunkPayloadExtras.hpp"
#include "aver/world/LevelFlatten.hpp"
#include "aver/world/LevelTransform.hpp"
#include "aver/world/RegionFile.hpp"
#include "aver/world/WorldPos.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

namespace aver::world {
namespace {

// CMeshRenderer::flags bit 0 (scene::kMeshRendererVisible). Restated because the cook is scene-free; AuthoredCookTest asserts they agree.
constexpr u32 kMeshVisibleFlag = 0x1u;

enum RootFlag : u8 {
    kFlagClass      = 1,    // a className anywhere in the subtree
    kFlagVehicle    = 2,    // a vehiclePreset anywhere in the subtree
    kFlagSequence   = 4,    // a level-sequence track targets a member
    kFlagUnpackable = 8,    // a value a payload cannot carry (a string over 65,535 bytes)
    kFlagUnbounded  = 16,   // a mesh the cook cannot bound, or nothing in the subtree to bound
};

// ---- hashing ---------------------------------------------------------------------------------------------------------

// FNV-1a 64, incrementally (aver::fnv1a64 takes one buffer; a level is hashed field by field without building its text).
struct Hasher {
    u64 h = kFnv1a64OffsetBasis;
    void bytes(const void* p, usize n) {
        const u8* b = static_cast<const u8*>(p);
        for (usize i = 0; i < n; ++i) { h ^= b[i]; h *= kFnv1a64Prime; }
    }
    template <class T> void pod(const T& v) { bytes(&v, sizeof v); }
    void str(const std::string& s) {
        pod<u64>(static_cast<u64>(s.size()));
        bytes(s.data(), s.size());
    }
};

void hashPlacement(Hasher& H, const fmt::OcWorldPlacement& p) {
    H.str(p.asset);
    H.pod<u64>(p.objectId);
    H.str(p.material);
    H.pod<f64>(p.x); H.pod<f64>(p.y); H.pod<f64>(p.z);
    H.pod<f64>(p.yaw); H.pod<f64>(p.pitch); H.pod<f64>(p.roll);
    H.pod<f64>(p.sx); H.pod<f64>(p.sy); H.pod<f64>(p.sz);
    const u8 flags = static_cast<u8>((p.collide ? 1 : 0) | (p.visible ? 2 : 0) | (p.snapToGround ? 4 : 0) | (p.animOnce ? 8 : 0));
    H.pod<u8>(flags);
    H.str(p.className);
    H.str(p.name);
    H.str(p.animClip);
    H.pod<f32>(p.animSpeed);
    H.pod<f32>(p.animTime);
    H.str(p.vehiclePreset);
    H.pod<i32>(p.parent);
    H.pod<u64>(p.placementId);
    H.pod<u64>(static_cast<u64>(p.components.size()));
    for (const fmt::OcPlacedComponent& c : p.components) {
        H.str(c.type);
        H.pod<u64>(static_cast<u64>(c.kv.size()));
        for (const auto& kv : c.kv) { H.str(kv.first); H.str(kv.second); }
    }
}

// The reach, in cm from a root's position on X or Y, past which a root is WIDE. Never more than the streamer's pad covers.
f64 wideReachCm(const CookOptions& opt) {
    const f64 cap = static_cast<f64>(kAuthoredPadChunks) * static_cast<f64>(opt.chunkSizeCm);
    return std::min(opt.wideCm * 0.5, cap);
}

// `rest` is the level without its placements and LEVELINST records (a copy the caller made, since the slim level starts there).
u64 hashLevel(const fmt::OcWorldData& level, const fmt::OcWorldData& rest, LevelAssetLibrary* lib, const CookOptions& opt) {
    Hasher H;
    H.pod<u32>(kCookVersion);
    H.pod<u32>(kChunkCodecVersion);
    H.pod<i32>(opt.chunkSizeCm);
    H.pod<f64>(wideReachCm(opt));
    H.pod<u64>(static_cast<u64>(level.placements.size()));
    for (const fmt::OcWorldPlacement& p : level.placements) hashPlacement(H, p);
    // Without the STREAM record: the distances are runtime policy, the data path is LevelStreaming's and chunks= is this set's own name, so
    // none of them changes what the cook produces and moving a slider must not make the set stale.
    fmt::OcWorldData forHash = rest;
    forHash.stream = fmt::OcStreamSettings{};
    H.str(fmt::writeOcworld(forHash));
    // The writer renumbers a sequence target through the emitted placements, and `rest` has none, so the targets (which decide what
    // is level-global) are hashed by their index in the level's own list.
    for (const fmt::OcSequence& seq : level.sequences)
        for (const fmt::OcSeqTrack& tr : seq.tracks) {
            H.pod<i32>(tr.target);
            H.pod<u32>(static_cast<u32>(tr.kind));
        }
    std::string inst;
    fmt::appendOcLevelInstances(inst, level.levelInstances);
    H.str(inst);
    // What the instanced levels hold: their own hashes, which already fold in their nested instances.
    if (lib)
        for (const fmt::OcLevelInstance& rec : level.levelInstances) H.pod<u64>(lib->closureHash(rec.asset));
    return H.h;
}

fmt::OcWorldData restOf(const fmt::OcWorldData& level) {
    fmt::OcWorldData rest = level;
    rest.placements.clear();
    rest.placements.shrink_to_fit();
    rest.levelInstances.clear();
    return rest;
}

// ---- bounds ----------------------------------------------------------------------------------------------------------

struct Box {
    bool any = false;
    f64 lo[3] = {0, 0, 0};
    f64 hi[3] = {0, 0, 0};
    void add(const f64 p[3]) {
        if (!any) { for (int a = 0; a < 3; ++a) lo[a] = hi[a] = p[a]; any = true; return; }
        for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], p[a]); hi[a] = std::max(hi[a], p[a]); }
    }
};

struct LocalBounds {
    bool ok = false;
    Vec3 lo{0, 0, 0}, hi{0, 0, 0};
};

// The meshes GameContent builds in: the cube and sphere, the drone, and the primitives of PrimitiveMeshes.hpp ("all are unit-sized: they
// fit in [-1, 1]^3"). Their real bounds are tighter for some, so using the unit box is conservative; a host with the exact bounds
// passes them through CookInput::meshBounds.
bool isBuiltinUnitMesh(const std::string& path) {
    static const char* const kNames[] = {"cube", "sphere", "drone", "plane", "cylinder", "cone", "capsule", "torus", "tube",
                                         "pyramid", "wedge", "banana"};
    for (const char* n : kNames)
        if (path == std::string("Meshes/") + n + ".ocmesh") return true;
    return false;
}

class BoundsCache {
public:
    explicit BoundsCache(const CookInput& in) : in_(in) {}

    const LocalBounds& get(const std::string& asset) {
        const auto it = cache_.find(asset);
        if (it != cache_.end()) return it->second;
        LocalBounds b;
        Vec3 lo{0, 0, 0}, hi{0, 0, 0};
        if (in_.meshBounds && in_.meshBounds(asset, lo, hi)) {
            b.ok = true;
        } else {
            std::string a = asset;
            std::replace(a.begin(), a.end(), '\\', '/');
            if (isBuiltinUnitMesh(a)) {   // the built-in meshes (GameContent::registerBuiltins), all inside [-1, 1]^3
                lo = Vec3{-1, -1, -1};
                hi = Vec3{1, 1, 1};
                b.ok = true;
            } else if (!in_.contentDir.empty()) {
                fmt::OcMeshData md;
                if (fmt::loadOcMesh((std::filesystem::path(in_.contentDir) / asset).string(), md)) {
                    lo = md.boundsMin;
                    hi = md.boundsMax;
                    b.ok = true;
                }
            }
        }
        if (b.ok) {
            const f32 v[6] = {lo.x, lo.y, lo.z, hi.x, hi.y, hi.z};
            for (f32 c : v) if (!std::isfinite(c)) b.ok = false;
        }
        if (b.ok) {
            b.lo = Vec3{std::min(lo.x, hi.x), std::min(lo.y, hi.y), std::min(lo.z, hi.z)};
            b.hi = Vec3{std::max(lo.x, hi.x), std::max(lo.y, hi.y), std::max(lo.z, hi.z)};
        } else {
            unknown.push_back(asset);
        }
        return cache_.emplace(asset, b).first->second;
    }

    std::vector<std::string> unknown;   // distinct assets with no bounds, first seen first

private:
    const CookInput& in_;
    std::unordered_map<std::string, LocalBounds> cache_;
};

// The eight corners of a mesh's local box under a world pose, as an f64 world box.
void boxOf(const Pose64& w, const Vec3& lo, const Vec3& hi, Box& out) {
    for (int k = 0; k < 8; ++k) {
        const f64 c[3] = {static_cast<f64>((k & 1) ? hi.x : lo.x) * static_cast<f64>(w.scale.x),
                          static_cast<f64>((k & 2) ? hi.y : lo.y) * static_cast<f64>(w.scale.y),
                          static_cast<f64>((k & 4) ? hi.z : lo.z) * static_cast<f64>(w.scale.z)};
        f64 r[3];
        rotateD(w.rot, c, r);
        const f64 p[3] = {w.pos[0] + r[0], w.pos[1] + r[1], w.pos[2] + r[2]};
        out.add(p);
    }
}

// Whether every value of a placement fits the payload's u16 fields (setPlacementExtras would refuse it otherwise).
bool packable(const fmt::OcWorldPlacement& p) {
    constexpr usize kMax = 0xFFFF;
    if (p.name.size() > kMax || p.animClip.size() > kMax || p.asset.size() > kMax || p.material.size() > kMax) return false;
    if (p.components.size() > kMaxExtrasPerEntity / 2) return false;
    for (const fmt::OcPlacedComponent& c : p.components) {
        if (c.type.size() > kMax || c.kv.size() > kMax) return false;
        usize bytes = 4 + c.type.size();
        for (const auto& kv : c.kv) {
            if (kv.first.size() > kMax || kv.second.size() > kMax) return false;
            bytes += 4 + kv.first.size() + kv.second.size();
        }
        if (bytes > kMaxExtraBytes) return false;
    }
    return true;
}

SplitPos ownerSplit(const f64 pos[3], i32 chunkSizeCm) {
    return chunkSizeCm == kWorldChunkCm ? worldPosOfAbs(pos[0], pos[1], pos[2]) : splitCm(pos[0], pos[1], pos[2], chunkSizeCm);
}

// A chunk-owned root: the region and chunk that own it, and its flat index.
struct OwnedRoot {
    RegionCoord reg;
    ChunkCoord c;
    u32 root;
};

// Flags each root from what its subtree holds, and boxes the chunk-owned ones: box[r] is the union over the subtree.
void flagRoots(const fmt::OcWorldData& level, const std::vector<FlatPlacement>& flat, const std::vector<u32>& rootOf,
               const std::vector<Pose64>& wpose, BoundsCache& bounds, std::vector<u8>& flags, std::vector<Box>& box) {
    const usize n = flat.size();
    for (usize i = 0; i < n; ++i) {
        const fmt::OcWorldPlacement& p = *flat[i].src;
        const u32 r = rootOf[i];
        if (!p.className.empty()) flags[r] |= kFlagClass;
        if (!p.vehiclePreset.empty()) flags[r] |= kFlagVehicle;
        if (!packable(p)) flags[r] |= kFlagUnpackable;
        if (!p.className.empty() || p.asset.empty()) continue;   // a class has no mesh of its own; an empty asset adds nothing to bound
        const LocalBounds& lb = bounds.get(p.asset);
        if (!lb.ok) { flags[r] |= kFlagUnbounded; continue; }
        boxOf(wpose[i], lb.lo, lb.hi, box[r]);
    }
    for (usize r = 0; r < n; ++r)
        if (flat[r].parent < 0 && !box[r].any) flags[r] |= kFlagUnbounded;   // nothing bounded in the subtree
    // Level-sequence tracks name placements by index into the level's own list, which is the head of the flat list.
    for (const fmt::OcSequence& seq : level.sequences)
        for (const fmt::OcSeqTrack& tr : seq.tracks)
            if (tr.target >= 0 && static_cast<usize>(tr.target) < level.placements.size())
                flags[rootOf[static_cast<usize>(tr.target)]] |= kFlagSequence;
    // A track bound by NAME finds its actor through SeqBindings::byName (the placement `name`, first wins) or World::find, so the actor must
    // be resident whenever the sequence runs: every subtree holding a placement of that name stays level-global too. (A sub-sequence
    // that lives in another asset is not visible here; its bind names are the author's to keep level-global with a className.)
    std::unordered_set<std::string> boundNames;
    for (const fmt::OcSequence& seq : level.sequences)
        for (const fmt::OcSeqTrack& tr : seq.tracks)
            if (!tr.bind.empty()) boundNames.insert(tr.bind);
    if (!boundNames.empty())
        for (usize i = 0; i < n; ++i)
            if (!flat[i].src->name.empty() && boundNames.count(flat[i].src->name)) flags[rootOf[i]] |= kFlagSequence;
}

// Splits the roots three ways: a level-global root is only counted; a chunk-owned root goes to `owned`, and one whose subtree
// reaches past wideReachCm from its position is also listed in `wide` (sorted here).
void classifyRoots(const std::vector<FlatPlacement>& flat, const std::vector<u8>& flags, const std::vector<Box>& box,
                   const CookOptions& opt, CookReport& rep, std::vector<OwnedRoot>& owned, std::vector<WideItem>& wide) {
    const f64 reach = wideReachCm(opt);
    for (usize r = 0; r < flat.size(); ++r) {
        if (flat[r].parent >= 0) continue;
        const u8 f = flags[r];
        if (f != 0) {
            if (f & kFlagClass) ++rep.globalClass;
            else if (f & kFlagVehicle) ++rep.globalVehicle;
            else if (f & kFlagSequence) ++rep.globalSequence;
            else if (f & kFlagUnpackable) ++rep.globalUnpackable;
            else ++rep.globalUnbounded;
            continue;
        }
        const SplitPos sp = ownerSplit(flat[r].pos, opt.chunkSizeCm);
        owned.push_back(OwnedRoot{regionOf(sp.chunk), sp.chunk, static_cast<u32>(r)});

        if (std::fabs(flat[r].pos[0]) > static_cast<f64>(kSubMmCliffCm) || std::fabs(flat[r].pos[1]) > static_cast<f64>(kSubMmCliffCm))
            ++rep.beyondSidecarLimit;

        const Box& b = box[r];
        const f64 reachX = std::max(std::fabs(b.lo[0] - flat[r].pos[0]), std::fabs(b.hi[0] - flat[r].pos[0]));
        const f64 reachY = std::max(std::fabs(b.lo[1] - flat[r].pos[1]), std::fabs(b.hi[1] - flat[r].pos[1]));
        if (std::max(reachX, reachY) > reach) {
            WideItem w;
            w.owner = sp.chunk;
            for (int a = 0; a < 3; ++a) {
                w.minCm[a] = static_cast<i64>(std::floor(b.lo[a]));
                w.maxCm[a] = static_cast<i64>(std::ceil(b.hi[a]));
            }
            wide.push_back(w);
        }
    }
    rep.wide = static_cast<u32>(wide.size());
    std::sort(wide.begin(), wide.end(), [](const WideItem& a, const WideItem& b) {
        if (a.owner.x != b.owner.x) return a.owner.x < b.owner.x;
        if (a.owner.y != b.owner.y) return a.owner.y < b.owner.y;
        if (a.owner.z != b.owner.z) return a.owner.z < b.owner.z;
        for (int k = 0; k < 3; ++k) if (a.minCm[k] != b.minCm[k]) return a.minCm[k] < b.minCm[k];
        for (int k = 0; k < 3; ++k) if (a.maxCm[k] != b.maxCm[k]) return a.maxCm[k] < b.maxCm[k];
        return false;
    });
}

// Sorts the owned roots by (region, chunk, root) and writes the plan's chunk and region tables. Chunk i owns the roots
// chunkRoots[chunkStart[i] .. chunkStart[i + 1]).
void groupByChunk(std::vector<OwnedRoot>& owned, CookPlan& plan, std::vector<u32>& chunkStart, std::vector<u32>& chunkRoots) {
    std::sort(owned.begin(), owned.end(), [](const OwnedRoot& a, const OwnedRoot& b) {
        if (a.reg.x != b.reg.x) return a.reg.x < b.reg.x;
        if (a.reg.y != b.reg.y) return a.reg.y < b.reg.y;
        if (a.reg.z != b.reg.z) return a.reg.z < b.reg.z;
        if (a.c.x != b.c.x) return a.c.x < b.c.x;
        if (a.c.y != b.c.y) return a.c.y < b.c.y;
        if (a.c.z != b.c.z) return a.c.z < b.c.z;
        return a.root < b.root;
    });
    for (usize k = 0; k < owned.size(); ++k) {
        if (k == 0 || !(owned[k].c == owned[k - 1].c)) {
            if (k == 0 || !(owned[k].reg == owned[k - 1].reg)) {
                if (!plan.regions.empty()) plan.regions.back().end = plan.chunks.size();
                CookPlan::Region reg;
                reg.coord = owned[k].reg;
                reg.first = plan.chunks.size();
                plan.regions.push_back(reg);
            }
            plan.chunks.push_back(owned[k].c);
            chunkStart.push_back(static_cast<u32>(chunkRoots.size()));
        }
        chunkRoots.push_back(owned[k].root);
    }
    chunkStart.push_back(static_cast<u32>(chunkRoots.size()));
    if (!plan.regions.empty()) plan.regions.back().end = plan.chunks.size();
}

// CSR over the flat list: the members of root r (the root first, then its descendants, ascending) are
// memberIdx[memberStart[r] .. memberStart[r + 1]).
void buildMembers(const std::vector<u32>& rootOf, std::vector<u32>& memberStart, std::vector<u32>& memberIdx) {
    const usize n = rootOf.size();
    memberStart.assign(n + 1, 0);
    for (usize i = 0; i < n; ++i) ++memberStart[rootOf[i] + 1];
    for (usize r = 0; r < n; ++r) memberStart[r + 1] += memberStart[r];
    memberIdx.assign(n, 0);
    std::vector<u32> cursor(memberStart.begin(), memberStart.end() - 1);
    for (usize i = 0; i < n; ++i) memberIdx[cursor[rootOf[i]]++] = static_cast<u32>(i);
}

// Completes the slim level: the level-global placements (their sequence targets renumbered) and the flat level's lights, decals and
// prefab instances. `slim` arrives as the level without its placements and LEVELINST records.
void fillSlim(fmt::OcWorldData& slim, const fmt::OcWorldData& level, const FlatLevel& fl, const std::vector<FlatPlacement>& flat,
              const std::vector<u32>& rootOf, const std::vector<u8>& flags, CookReport& rep) {
    std::vector<i32> newIndex(flat.size(), -1);
    for (usize i = 0; i < flat.size(); ++i) {
        if (flags[rootOf[i]] == 0) continue;
        const FlatPlacement& fp = flat[i];
        fmt::OcWorldPlacement rec = *fp.src;
        newIndex[i] = static_cast<i32>(slim.placements.size());
        rec.parent = fp.parent >= 0 ? newIndex[static_cast<usize>(fp.parent)] : -1;
        if (fp.topIndex < 0) {   // from an instance: the pose is the composed one for a root, and the id is the flat one
            if (fp.composed) {
                rec.x = fp.pos[0]; rec.y = fp.pos[1]; rec.z = fp.pos[2];
                rec.roll = fp.roll; rec.pitch = fp.pitch; rec.yaw = fp.yaw;
                rec.sx = fp.scale[0]; rec.sy = fp.scale[1]; rec.sz = fp.scale[2];
            }
            rec.placementId = fp.id;
        }
        slim.placements.push_back(std::move(rec));
    }
    for (fmt::OcSequence& seq : slim.sequences)
        for (fmt::OcSeqTrack& tr : seq.tracks)
            if (tr.target >= 0) {
                const i32 t = static_cast<usize>(tr.target) < level.placements.size() ? newIndex[static_cast<usize>(tr.target)] : -1;
                if (t < 0) rep.warnings.push_back("a sequence track targets a placement that is not level-global");
                tr.target = t;
            }
    slim.lights.insert(slim.lights.end(), fl.lights.begin(), fl.lights.end());
    slim.decals.insert(slim.decals.end(), fl.decals.begin(), fl.decals.end());
    slim.prefabInstances.insert(slim.prefabInstances.end(), fl.prefabInstances.begin(), fl.prefabInstances.end());
}

} // namespace

// ---- the plan's state ------------------------------------------------------------------------------------------------

struct CookPlan::State {
    FlatLevel flat;
    i32 size = kDefaultChunkSizeCm;
    // The owned roots of chunk i are chunkRoots[chunkStart[i] .. chunkStart[i + 1]), in flat order.
    std::vector<u32> chunkStart;
    std::vector<u32> chunkRoots;
    // The members of root r (the root first, then its descendants, ascending flat index) are memberIdx[memberStart[r] .. memberStart[r + 1]).
    std::vector<u32> memberStart;
    std::vector<u32> memberIdx;
};

std::string cookDirFor(const std::string& projectDir, const std::string& levelStem) {
    return (std::filesystem::path(projectDir) / "Binaries" / "Chunks" / levelStem).string();
}

ChunkPayload CookPlan::payload(usize ci) const {
    ChunkPayload p;
    p.coord = chunks[ci];
    p.chunkSizeCm = chunkSizeCm;
    const State& st = *state_;
    for (u32 k = st.chunkStart[ci]; k < st.chunkStart[ci + 1]; ++k) {
        const u32 root = st.chunkRoots[k];
        const u32 mb = st.memberStart[root], me = st.memberStart[root + 1];
        const i32 base = static_cast<i32>(p.entities.size());
        for (u32 m = mb; m < me; ++m) {
            const u32 fi = st.memberIdx[m];
            const FlatPlacement& fp = st.flat.placements[fi];
            const fmt::OcWorldPlacement& src = *fp.src;

            PayloadEntity e;
            // The entity's name IS the asset path (instantiate's convention); the outliner label travels as LABL.
            e.name = src.asset;
            e.objectId = src.objectId;
            if (fp.parent < 0) {
                e.parent = -1;
            } else {
                // Members are ascending, so the parent (one of them, lower than this one) is found by search.
                const u32* b = st.memberIdx.data() + mb;
                const u32* en = st.memberIdx.data() + me;
                e.parent = base + static_cast<i32>(std::lower_bound(b, en, static_cast<u32>(fp.parent)) - b);
            }

            // The rotation is instantiate()'s: quatFromEulerDeg of the f32 casts. EULR carries those same three floats.
            e.local.rotation = quatFromEulerDeg(Vec3{fp.roll, fp.pitch, fp.yaw});
            e.local.scale = Vec3{fp.scale[0], fp.scale[1], fp.scale[2]};
            if (fp.parent < 0) {
                // A root is chunk-local: the f64 world position split once, and only the small part narrowed.
                e.local.position = ownerSplit(fp.pos, st.size).local;
            } else {
                e.local.position = Vec3{static_cast<f32>(fp.pos[0]), static_cast<f32>(fp.pos[1]), static_cast<f32>(fp.pos[2])};
            }

            e.hasMesh = !src.asset.empty();
            if (e.hasMesh) {
                e.mesh = src.objectId;
                e.material = src.material;
                e.meshFlags = src.visible ? kMeshVisibleFlag : 0u;
            }

            PlacementExtras x;
            x.hasPlacementId = true;
            x.placementId = fp.id;
            if (!src.animClip.empty()) {
                x.hasAnim = true;
                x.animClip = src.animClip;
                x.animSpeed = src.animSpeed;
                x.animTime = src.animTime;
                x.animOnce = src.animOnce;
            }
            if (!src.collide) {   // absent means collide, the record's own default
                x.hasCollide = true;
                x.collide = false;
            }
            x.components = src.components;
            if (!src.name.empty()) {
                x.hasLabel = true;
                x.label = src.name;
            }
            x.snapToGround = fp.parent < 0 && src.snapToGround;
            if (fp.roll != 0.0f || fp.pitch != 0.0f || fp.yaw != 0.0f) {
                x.hasEuler = true;
                x.euler[0] = fp.roll; x.euler[1] = fp.pitch; x.euler[2] = fp.yaw;
            }
            setPlacementExtras(e, x);   // cannot refuse: packable() kept anything it would refuse out of the chunks
            p.entities.push_back(std::move(e));
        }
    }
    return p;
}

// ---- planning --------------------------------------------------------------------------------------------------------

bool planAuthoredCook(const fmt::OcWorldData& level, const CookInput& in, const CookOptions& opt, CookPlan& plan,
                      CookReport& rep, std::string* why) {
    Clock clock;
    const auto fail = [why](const std::string& m) { if (why) *why = m; return false; };
    plan = CookPlan{};
    rep = CookReport{};
    if (!chunkSizeValid(opt.chunkSizeCm)) return fail("chunk size " + std::to_string(opt.chunkSizeCm) + " cm is outside the precision budget");
    if (opt.wideCm <= 0.0) return fail("wideCm must be positive");

    LevelAssetLibrary localLib;
    LevelAssetLibrary* lib = in.library;
    if (!lib) {
        localLib.setLoader(LevelAssetLibrary::diskLoader(in.contentDir));
        lib = &localLib;
    }

    auto st = std::make_shared<CookPlan::State>();
    st->size = opt.chunkSizeCm;
    std::string err;
    if (!flattenLevelInstances(level, in.levelAsset, *lib, st->flat, &err)) return fail(err);
    const std::vector<FlatPlacement>& flat = st->flat.placements;
    const usize n = flat.size();
    rep.placementsIn = static_cast<u32>(level.placements.size());
    rep.flatPlacements = static_cast<u32>(n);
    rep.instances = st->flat.instances;
    rep.warnings = st->flat.warnings;

    // The slim level starts as a copy of everything but the placements; the hash is taken before anything is changed in it.
    fmt::OcWorldData slim = restOf(level);
    plan.sourceHash = hashLevel(level, slim, lib, opt);
    rep.sourceHash = plan.sourceHash;
    plan.chunkSizeCm = opt.chunkSizeCm;

    // ---- world poses, boxes and flags (f64) ----
    std::vector<u32> rootOf(n);
    for (usize i = 0; i < n; ++i) rootOf[i] = flat[i].parent < 0 ? static_cast<u32>(i) : rootOf[static_cast<usize>(flat[i].parent)];

    std::vector<Pose64> wpose(n);
    for (usize i = 0; i < n; ++i) {
        const FlatPlacement& fp = flat[i];
        Pose64 local;
        for (int a = 0; a < 3; ++a) local.pos[a] = fp.pos[a];
        local.rot = quatFromEulerDeg(Vec3{fp.roll, fp.pitch, fp.yaw});
        local.scale = Vec3{fp.scale[0], fp.scale[1], fp.scale[2]};
        wpose[i] = fp.parent >= 0 ? composePose(wpose[static_cast<usize>(fp.parent)], local) : local;
    }

    BoundsCache bounds(in);
    std::vector<u8> flags(n, 0);
    std::vector<Box> box(n);   // meaningful at roots: the union over the subtree
    flagRoots(level, flat, rootOf, wpose, bounds, flags, box);
    wpose.clear();
    wpose.shrink_to_fit();

    // ---- classify, then group the chunk-owned roots by chunk ----
    std::vector<OwnedRoot> owned;
    owned.reserve(n);
    classifyRoots(flat, flags, box, opt, rep, owned, plan.wide);
    groupByChunk(owned, plan, st->chunkStart, st->chunkRoots);
    buildMembers(rootOf, st->memberStart, st->memberIdx);

    // ---- report: counts, the largest chunk ----
    for (usize i = 0; i < n; ++i) {
        if (flags[rootOf[i]] == 0) ++rep.chunkOwned; else ++rep.levelGlobal;
    }
    rep.chunkOwnedRoots = static_cast<u32>(owned.size());
    rep.chunks = static_cast<u32>(plan.chunks.size());
    rep.regions = static_cast<u32>(plan.regions.size());
    for (usize ci = 0; ci < plan.chunks.size(); ++ci) {
        u32 count = 0;
        for (u32 k = st->chunkStart[ci]; k < st->chunkStart[ci + 1]; ++k) {
            const u32 root = st->chunkRoots[k];
            count += st->memberStart[root + 1] - st->memberStart[root];
        }
        if (count > rep.largestChunkEntities) { rep.largestChunkEntities = count; rep.largestChunk = plan.chunks[ci]; }
    }
    rep.wideItems = plan.wide;
    for (const std::string& a : bounds.unknown) {
        if (rep.unknownBoundsAssets.size() >= 32) break;
        rep.unknownBoundsAssets.push_back(a);
    }
    if (!bounds.unknown.empty())
        rep.warnings.push_back(std::to_string(bounds.unknown.size()) + " mesh(es) have no bounds; their placements stay level-global");
    if (rep.beyondSidecarLimit)
        rep.warnings.push_back(std::to_string(rep.beyondSidecarLimit) + " chunk-owned placement(s) lie beyond +-1,048,576 cm of world (0, 0): "
                               "fine for chunks, outside the f32 sidecars' valid range (landscape, lanes, navigation, foliage tables)");

    // ---- the slim level ----
    fillSlim(slim, level, st->flat, flat, rootOf, flags, rep);
    plan.slim = std::move(slim);

    plan.state_ = std::move(st);
    rep.elapsedMs = clock.restart() * 1000.0;
    return true;
}

u64 cookSourceHash(const fmt::OcWorldData& level, const CookInput& in, const CookOptions& opt) {
    LevelAssetLibrary localLib;
    LevelAssetLibrary* lib = in.library;
    if (!lib && !level.levelInstances.empty()) {
        localLib.setLoader(LevelAssetLibrary::diskLoader(in.contentDir));
        lib = &localLib;
    }
    const fmt::OcWorldData rest = restOf(level);
    return hashLevel(level, rest, lib, opt);
}

// ---- writing ---------------------------------------------------------------------------------------------------------

bool cookAuthoredLevel(const fmt::OcWorldData& level, const CookInput& in, const CookOptions& opt, CookReport& rep,
                       std::string* why) {
    Clock clock;
    const auto fail = [&](const std::string& m) { if (why) *why = m; return false; };
    CookPlan plan;
    if (!planAuthoredCook(level, in, opt, plan, rep, why)) return false;

    if (in.levelStem.empty() && opt.outDir.empty()) return fail("the cook needs a level stem (it names the cook directory)");
    namespace fs = std::filesystem;
    const fs::path outDir = !opt.outDir.empty() ? fs::path(opt.outDir) : fs::path(cookDirFor(in.projectDir, in.levelStem));
    const fs::path tmpDir = fs::path(outDir.string() + ".tmp");
    std::error_code ec;
    fs::remove_all(tmpDir, ec);
    fs::create_directories(tmpDir, ec);
    if (ec) return fail("could not create " + tmpDir.string() + ": " + ec.message());

    const auto abandon = [&](const std::string& m) {
        std::error_code e2;
        fs::remove_all(tmpDir, e2);
        return fail(m);
    };

    RegionIndex ix;
    ix.levelId = fnv1a64(std::string_view(in.levelStem));
    ix.chunkSizeCm = opt.chunkSizeCm;
    ix.worldSeed = 0;
    ix.generatorVersion = 0;
    ix.kind = kIndexKindCooked;
    ix.sourceHash = plan.sourceHash;
    ix.wide = plan.wide;

    u64 bytes = 0;
    for (const CookPlan::Region& reg : plan.regions) {
        // One region at a time: its payloads are built, written and dropped, so memory peaks at the largest region.
        std::vector<std::pair<ChunkLocal, ChunkPayload>> chunks;
        chunks.reserve(reg.end - reg.first);
        for (usize ci = reg.first; ci < reg.end; ++ci) {
            ChunkPayload p = plan.payload(ci);
            chunks.emplace_back(localOf(p.coord), std::move(p));
        }
        RegionWriteDesc d;
        d.coord = reg.coord;
        d.chunkSizeCm = opt.chunkSizeCm;
        d.levelId = ix.levelId;
        d.worldSeed = 0;
        d.generatorVersion = 0;
        d.serial = 1;
        d.packed = true;
        const std::string name = regionFileName(reg.coord);
        const std::string path = (tmpDir / name).string();
        std::string werr;
        if (!writeRegion(path, d, std::move(chunks), &werr)) return abandon("could not write " + name + ": " + werr);

        RegionFile f;
        if (!f.open(path, &werr)) return abandon("could not read back " + name + ": " + werr);
        RegionEntry e;
        e.coord = reg.coord;
        e.contentHash = f.header().contentHash;
        e.chunkCount = static_cast<u32>(reg.end - reg.first);
        e.relativePath = name;
        ix.add(e);
        f.close();
        const auto sz = fs::file_size(path, ec);
        if (!ec) bytes += static_cast<u64>(sz);
    }

    std::string ierr;
    const std::string indexPath = (tmpDir / kCookIndexName).string();
    if (!writeIndex(indexPath, ix, &ierr)) return abandon(ierr);
    const auto isz = fs::file_size(indexPath, ec);
    if (!ec) bytes += static_cast<u64>(isz);

    const std::string levelPath = (tmpDir / kCookLevelName).string();
    if (!fmt::saveOcworld(levelPath, plan.slim, &ierr)) return abandon("could not write " + std::string(kCookLevelName) + ": " + ierr);
    const auto lsz = fs::file_size(levelPath, ec);
    if (!ec) bytes += static_cast<u64>(lsz);

    // The new set replaces the old one only now that every file of it exists.
    fs::remove_all(outDir, ec);
    if (ec) return abandon("could not remove the previous cooked set at " + outDir.string() + " (is a game running on it?): " + ec.message());
    fs::rename(tmpDir, outDir, ec);
    if (ec) return abandon("could not move the cooked set into " + outDir.string() + ": " + ec.message());

    rep.bytes = bytes;
    rep.dir = outDir.string();
    rep.elapsedMs += clock.restart() * 1000.0;
    AVER_INFO("[Cook] {}: {} chunk-owned placement(s) in {} chunk(s) / {} region(s), {} level-global, {} wide, {:.1f} MB, {:.0f} ms",
              in.levelStem, rep.chunkOwned, rep.chunks, rep.regions, rep.levelGlobal, rep.wide,
              static_cast<f64>(rep.bytes) / (1024.0 * 1024.0), rep.elapsedMs);
    return true;
}

} // namespace aver::world
