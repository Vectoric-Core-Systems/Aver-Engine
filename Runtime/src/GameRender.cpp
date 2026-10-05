// The world draw walk: every live entity with a visible CMeshRenderer.
//
// Lifted from the editor's entity loop (formerly SandboxApp.cpp:1386-1510; the 2026-09-16 split
// moved it to sandbox/src/SandboxRender.cpp's onRender, which now calls this instead of carrying
// a copy). Deliberately absent: selection latch/outline, grid, gizmo, skin-scene-test recolour,
// objects_ placeholder pass, capture/gate harness -- editor-only concerns.
//
// DrawWorldOptions' hooks let the editor call this walk TWICE a frame (depth prepass, then
// colour) instead of keeping its own copy, while it alone links hierarchical-Z occlusion culling
// (Aver.Occlusion, linked into Sandbox alone), Trifactor LOD/GPU cluster dispatch, selection
// outlines, a PlayerStart icon and the path-traced scene view. Every hook defaults to null =
// "this walk's original answer"; a default-constructed DrawWorldOptions must reproduce the
// pre-hook command stream exactly (frame-capture verified).
//
// F4 fix ("unified direct route", aver/game/SceneSubmission.hpp): a frustum-culled or
// ownerHideRoot-hidden entity still reaches DrawWorldOptions::voxiRenderer (when one is attached)
// so shadows, GI voxelisation and the RT TLAS stay independent of raster visibility; only
// drawMesh() is skipped.
//
// Per-material mesh splits (GameContent::partsFor) draw one PlannedDraw per part on both routes,
// via the shared authored > look > fallback ladder, now INCLUDED from SceneSubmission.hpp rather
// than hand-kept -- this file used to carry its own PlannedDraw, kMaxPlannedDraws, planEntityDraws
// and ladder, and they had drifted (its `if (authored)` had no liveness test, so a stale handle
// drew as a white metal mirror). Header lives in this library's public include dir (Runtime/include,
// cmake/AvModule.cmake:15) so the editor and this walk agree.
#include "aver/game/GameRender.hpp"

#if AVER_MODULE_SCENE

#include "aver/game/GameContent.hpp"
#include "aver/game/SceneSubmission.hpp"
#include "aver/game/PlayMobility.hpp"
// CpuLap/CpuNest split drawWorld's "6.8ms in the rest" number into per-phase CpuSpan buckets
// (WalkEntity, WalkLookup, WalkFrustum, WalkDecide, the three WalkEmit*, WalkResolveLook,
// WalkOther) -- see aver/core/CpuTiming.hpp for why this is separate from the GPU side's
// ScopedGpuStat. Unguarded by an AVER_MODULE_* macro: it needs nothing beyond this file's own
// AVER_MODULE_SCENE guard and compiles itself out via AVER_CPU_TIMING instead.
#include "aver/core/CpuTiming.hpp"
#include "aver/core/Log.hpp"
#include "aver/rhi/RHI.hpp"
#if AVER_MODULE_PBR
#  include "aver/pbr/MaterialSystem.hpp"
#endif
#include "aver/render/SkinnedScene.hpp"
#if AVER_MODULE_VOXI
#  include "aver/voxi/VoxiRenderer.hpp"
#endif
#include "aver/scene/World.hpp"
#include "aver/scene/scene_abi.h"
#include "GameMath.hpp"

#include <atomic>
#include <cmath>
#include <unordered_set>
#include <vector>

namespace aver::game {

namespace {

// Mixes the mesh id's bits (MurmurHash3's fmix64) before slotting it below, rather than trusting
// the low bits under the mask alone. mr->mesh is fnv1a64(project-relative path) -- verified as the
// only scheme that hands out mesh ids (GameContent.cpp: registerBuiltins' `add`, loadProjectMeshes)
// -- so it is already a hash, but built-in and level asset names are short and share long common
// prefixes ("Meshes/sphere.ocmesh" vs "Meshes/cube.ocmesh"), an input shape that stresses a
// multiplicative hash's low bits. Re-mixing costs two multiplies and three shifts and removes the
// question entirely, so it is paid unconditionally. Never hashes raw bytes itself -- that stays
// GameContent's job (fnv1a64).
constexpr u64 mixMeshId(u64 x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

// Power of two, so a mesh id's home slot is one mask away from its mixed hash, not a modulo
// (verified by the static_assert below).
//
// 8192, up from 64, and open-addressed (linear probe) rather than direct-mapped. Two earlier sizes
// were each right for the scene they were measured on and wrong for a city: a single "last mesh"
// slot is 100% effective on 16,000 copies of one cube but useless on a forest of a handful of
// INTERLEAVED tree species (entities are visited in spawn/load order, not sorted by mesh), and 64
// direct-mapped slots hit ~67% on the NeonDistrict level (42,345 mesh entities over ~2,940
// DISTINCT meshes), every miss being up to four GameContent hash probes. This table is nearly
// three times wider than the largest distinct-mesh count seen so far, and two ids that share a home
// slot probe to the next free one instead of evicting each other -- a full-window collision (the
// probe limit below) still evicts the home slot, which is exactly the old direct-mapped behaviour,
// so an oversized level degrades to a lower hit rate and never to a wrong answer.
//
// THE TABLE IS REUSED ACROSS CALLS, and is emptied by a generation stamp instead of by zeroing it:
// a slot whose `generation` is not the current call's reads as empty, so the per-call reset costs
// one increment however large the table is. See WalkCacheLease for who owns it and why a
// re-entrant or concurrent call gets no cache rather than a shared one.
constexpr u32 kMeshLookupCacheSlots = 8192;
static_assert((kMeshLookupCacheSlots & (kMeshLookupCacheSlots - 1)) == 0,
              "kMeshLookupCacheSlots must be a power of two for the '& (kMeshLookupCacheSlots - 1)' "
              "mask below to be equivalent to '% kMeshLookupCacheSlots'");
// How far a probe walks from the home slot before giving up and evicting it. Short on purpose: at
// the load this table runs at (a third or less) a longer window only ever costs cache lines.
constexpr u32 kMeshLookupProbeLimit = 8;
static_assert(kMeshLookupProbeLimit <= kMeshLookupCacheSlots, "the probe window must fit in the table");

// One slot's answer, for the mesh id it currently holds, to all four of WalkLookup's probes --
// meshFor, boundsFor, meshDefaultMaterial, partsFor (all verified pure: a find() plus a return,
// const, no lazy upload, no side effect). EMPTY IS `generation != the call's own generation`, not
// meshId == 0: the table outlives a call now, so a slot written by an earlier call still holds a
// real, non-zero id and a real (possibly stale) answer, and the stamp is what says it is not this
// call's. A default-constructed slot has generation 0, which no call ever uses (the first lease
// stamps 1), so a slot nobody has written also reads as empty.
//
// Fields resolve lazily and independently because their ORIGINAL probes were gated differently:
// boundsFor only ran under `!skinned`, meshDefaultMaterial only when `mr->material == 0`, and
// either could be skipped by an earlier `continue`. Resolving eagerly would run a lookup the
// entity's own path never would have. `handle`/`parts` have no such gate but get the same
// `*Resolved` shape anyway, so all four sites share one pattern.
struct MeshLookupCacheSlot {
    u64 meshId = 0;
    u32 generation = 0;                      // which drawWorld() call wrote this slot; see above

    bool handleResolved = false;
    rhi::MeshHandle handle = 0;              // content.meshFor(meshId)

    bool boundsResolved = false;
    bool haveBounds = false;                 // boundsFor's own verdict: did it find an entry
    // Copies of what boundsFor found, not the pointer (`&it->second`, into GameContent::meshBounds_)
    // -- 24 bytes, copies for free, can never dangle. `parts` below is kept as a pointer instead;
    // see DrawWorldOptions::useMeshLookupCache for why.
    Vec3 boundsMin{};
    Vec3 boundsMax{};

    bool defaultMaterialResolved = false;
    i32 defaultMaterial = 0;                 // content.meshDefaultMaterial(meshId)

    bool partsResolved = false;
    // content.partsFor(meshId)'s own pointer, not copied: copying the vector would be a heap
    // allocation per distinct mesh, every frame -- exactly what this cache avoids. Safe because
    // std::unordered_map only invalidates a pointer to an element by ERASING it (never by inserting
    // elsewhere or rehashing), and nothing reachable from either host's per-entity hooks erases from
    // GameContent::meshParts_ mid-walk.
    const std::vector<GameContent::MeshPart>* parts = nullptr;
};

// One planned draw's resolved look, as drawWorld's resolveDrawLook ladder returns it. Hoisted out of
// that lambda only so the memo below can store it.
struct DrawLook {
    // The ladder's verdict, unedited. `look.blended` can only be true for a LIVE authored
    // .ocmat -- a built-in SurfaceLook (registerBuiltins) or the flat-gray fallback has no
    // BLEND record, so no other rung can set it.
    SurfaceLook look;
    rhi::BindingSetHandle matSet = 0;
    const void* matConstants = nullptr;
    u32 matBytes = 0;
};

// Power of two, for the same one-mask reason as kMeshLookupCacheSlots. 1024 slots hold several
// times the distinct material tokens a walk has been seen to name (Jungle Ruins: ~157 resident
// materials); a token that finds its probe window full is simply built every time, as it was before
// the memo existed, so an oversized level costs speed and never correctness.
constexpr u32 kDrawLookMemoBits = 10;
constexpr u32 kDrawLookMemoSlots = 1u << kDrawLookMemoBits;
constexpr u32 kDrawLookProbeLimit = 8;

// One memoised resolveDrawLook answer. Same emptiness rule as MeshLookupCacheSlot: EMPTY IS
// `generation != the call's own generation`, and generation 0 is never a call's.
struct DrawLookMemoSlot {
    u32 generation = 0;
    i32 material = 0;
    DrawLook look;
};

// The tables drawWorld reuses from one call to the next, so a walk starts with a bumped counter
// instead of a zeroed table (the mesh table alone is several hundred KB; zeroing it twice a frame,
// depth prepass then colour, for a change that saves work per ENTITY would eat part of the win).
// `looks` is sized on first use, `meshSlots` only if some call asks for the mesh cache.
struct WalkCacheStore {
    std::vector<MeshLookupCacheSlot> meshSlots;
    std::vector<DrawLookMemoSlot> looks;
    u32 generation = 0;
    // Held for the duration of one drawWorld() call. See WalkCacheLease.
    std::atomic<bool> busy{false};
};

WalkCacheStore& walkCacheStore() {
    static WalkCacheStore store;
    return store;
}

// Finds this walk's cache slot for `meshId`: the slot already holding it (the cache hit -- left
// untouched), else the first slot in the probe window that this call has not written yet, claimed
// for `meshId` with all four fields reset to unresolved. A window with no match and no free slot
// evicts the HOME slot, resetting it the same way -- so a hit never reads a previous occupant's
// fields under the new key, whether that occupant was another mesh in this call or ANY mesh in an
// earlier one.
//
// Nothing is ever deleted inside a call, so a slot that was free when an id was first claimed stays
// occupied for the rest of the call: an id that is present is always found inside its own window
// before the first free slot, which is what makes "claim the first free slot" safe against putting
// one id in two places.
//
// Called once per entity, not once per probe: drawWorld's `meshSlot` local is computed at the
// first WalkLookup call site and threaded through the other three by pointer, so it is not
// re-derived (and re-risking eviction) at each one.
MeshLookupCacheSlot& findMeshLookupSlot(WalkCacheStore& store, u64 meshId) {
    MeshLookupCacheSlot* const slots = store.meshSlots.data();
    const u32 gen = store.generation;
    const u32 home = static_cast<u32>(mixMeshId(meshId) & (kMeshLookupCacheSlots - 1));
    u32 at = home;
    for (u32 probe = 0; probe < kMeshLookupProbeLimit; ++probe) {
        MeshLookupCacheSlot& slot = slots[at];
        if (slot.generation != gen) {
            slot = MeshLookupCacheSlot{meshId, gen};
            return slot;
        }
        if (slot.meshId == meshId) return slot;
        at = (at + 1) & (kMeshLookupCacheSlots - 1);
    }
    MeshLookupCacheSlot& slot = slots[home];
    slot = MeshLookupCacheSlot{meshId, gen};
    return slot;
}

// drawWorld's claim on the reused tables for the length of one call.
//
// WHAT "ONE CALL" MEANS FOR INVALIDATION IS UNCHANGED FROM WHEN THESE WERE STACK LOCALS: the
// generation is bumped as the lease is taken, so nothing a previous call learned is visible to this
// one. A content reload, a mesh hot-reload, a chunk streamed in or out between two calls (the editor
// runs this walk twice a frame) is therefore seen by the first probe of each id in the next call,
// exactly as it was when every call began with a fresh zeroed array. WITHIN a call the contract is
// the one DrawWorldOptions::useMeshLookupCache documents. Both tables are keyed by mesh id / material
// token, never by entity, so entities appearing and disappearing under streaming cannot make an
// entry wrong.
//
// A SECOND CLAIMANT GETS NOTHING, NOT A SHARED TABLE. A hook that re-entered drawWorld, or a second
// thread walking a world of its own, would bump the generation under the first walk's feet and turn
// its `meshSlot` into another id's answers -- the silent wrong-mesh failure the generation exists to
// prevent. `busy` refuses that: the loser's meshCacheOn() is false and its findLook() returns null,
// which drawWorld reads as "cache off" (every probe direct to GameContent, every look built) -- the
// same path DrawWorldOptions::useMeshLookupCache=false already takes, so it is correct by
// construction, only slower. Nothing calls drawWorld re-entrantly today (both hosts call it from
// their frame function).
class WalkCacheLease {
public:
    // wantMeshCache false (DrawWorldOptions::useMeshLookupCache off) never sizes the mesh table.
    explicit WalkCacheLease(bool wantMeshCache) {
        WalkCacheStore& s = walkCacheStore();
        if (s.busy.exchange(true, std::memory_order_acquire)) return;
        store_ = &s;
        if (s.looks.empty()) s.looks.resize(kDrawLookMemoSlots);
        if (wantMeshCache && s.meshSlots.empty()) s.meshSlots.resize(kMeshLookupCacheSlots);
        // 0 is what a never-written slot holds, so a call must never stamp it. A wrap after four
        // billion calls is the one moment an old stamp could equal the new one, so clear the tables
        // then rather than trust it.
        if (++s.generation == 0) {
            for (MeshLookupCacheSlot& slot : s.meshSlots) slot = MeshLookupCacheSlot{};
            for (DrawLookMemoSlot& slot : s.looks) slot = DrawLookMemoSlot{};
            s.generation = 1;
        }
        meshCache_ = wantMeshCache;
    }
    ~WalkCacheLease() {
        if (store_) store_->busy.store(false, std::memory_order_release);
    }
    WalkCacheLease(const WalkCacheLease&) = delete;
    WalkCacheLease& operator=(const WalkCacheLease&) = delete;

    // Whether the mesh table may be used this call: asked for AND not lost to another claimant.
    bool meshCacheOn() const { return store_ != nullptr && meshCache_; }
    MeshLookupCacheSlot& findMesh(u64 meshId) { return findMeshLookupSlot(*store_, meshId); }

    // The memo slot for `material`. Returns null when there is nothing to memoise into (no lease, or
    // the probe window is full of other tokens): build the look and do not commit it. Otherwise
    // `hit` says whether the slot already holds this call's answer; on a miss it is the first free
    // slot in the window, for commitLook to fill once the look has been built.
    DrawLookMemoSlot* findLook(i32 material, bool& hit) {
        hit = false;
        // Off with the mesh cache too: DrawWorldOptions::useMeshLookupCache=false (--no-walk-cache) is
        // documented as "none of the caching machinery touched", and it is the A/B switch for all of it.
        if (!store_ || !meshCache_) return nullptr;
        DrawLookMemoSlot* const slots = store_->looks.data();
        const u32 gen = store_->generation;
        u32 at = (static_cast<u32>(material) * 2654435761u) >> (32u - kDrawLookMemoBits);
        for (u32 probe = 0; probe < kDrawLookProbeLimit; ++probe) {
            DrawLookMemoSlot& slot = slots[at];
            if (slot.generation != gen) return &slot;
            if (slot.material == material) { hit = true; return &slot; }
            at = (at + 1) & (kDrawLookMemoSlots - 1);
        }
        return nullptr;
    }
    // Stamped only once the look exists, so a slot is never live while holding a half-built answer.
    void commitLook(DrawLookMemoSlot& slot, i32 material, const DrawLook& look) {
        slot.generation = store_->generation;
        slot.material = material;
        slot.look = look;
    }

private:
    WalkCacheStore* store_ = nullptr;
    bool meshCache_ = false;
};

} // namespace

void drawWorld(rhi::IDevice& device, const Mat4& viewProj, GameContent& content, SceneDrawStats& stats,
               pbr::MaterialSystem* materials, render::SkinnedScene* skinning,
               const DrawWorldOptions& options) {
    scene::World& w = scene::World::instance();
    // One CpuLap for the whole walk, threaded through every phase via `.to()` (see CpuLap's own
    // comment: one Lap per measured scope). Opens on WalkEntity, not SceneWalk itself -- SceneWalk
    // is the COMPUTED sum of its children (kCpuSpanParent's own comment says why that sum must be
    // exact) and CpuLap's constructor asserts against being handed it -- which folds the pre-loop
    // setup (frustum planes, etc.) into WalkEntity's own "loop overhead" bucket. `lap` lives until
    // the function returns, so the logging tail below the loop is charged to whichever phase was
    // open when the last entity finished.
    CpuLap lap(CpuSpan::WalkEntity);
    // The depth-prepass walk is the same walk emitting depth-only draws (see DrawWorldPass). Cached
    // in a local since it's tested per entity and per planned draw and cannot change mid-call.
    const bool depthPass = options.pass == DrawWorldPass::DepthPrepass;
    int drawn = 0, culled = 0, ownerHidden = 0;

    // WalkLookup's mesh cache and the per-material look memo for this call alone -- see
    // MeshLookupCacheSlot, DrawLookMemoSlot and DrawWorldOptions::useMeshLookupCache. The storage is
    // REUSED across calls (a generation stamp, not a zeroed array, empties it), but what a call can
    // SEE of it is exactly what a fresh stack local would have shown: nothing a previous call
    // learned. The editor's two per-frame calls (depth prepass, colour) each start empty. See
    // WalkCacheLease for the re-entrancy rule. Constructed unconditionally, cache on or off, so call
    // sites below read `useLookupCache` with a plain `if` rather than an `#if`.
    WalkCacheLease cacheLease(options.useMeshLookupCache);
    const bool useLookupCache = cacheLease.meshCacheOn();
    // Hits/misses across all four WalkLookup sites, published to `stats` (colour pass only, same
    // "describes a frame" rule as drawn/culled/ownerHidden) -- see SceneDrawStats on why these are
    // never accumulated across calls.
    int meshLookupHits = 0, meshLookupMisses = 0;
    // The planned draws of ONE entity at a time, declared once for the whole walk instead of being
    // default-constructed (kMaxPlannedDraws slots of PlannedDraw{0, 0}) for every entity.
    // planEntityDraws writes out[0..n-1] and returns n, and all three delivery loops below read only
    // [0, pdrawCount), so a slot past this entity's count is never read and whatever an earlier
    // entity left there cannot matter; the slots that ARE read were all written for this entity first.
    PlannedDraw pdraws[kMaxPlannedDraws];

    // Six frustum planes from viewProj. Engine convention is row-vector, so a clip coordinate is a
    // dot with a COLUMN and each plane is a sum/difference of two columns. Derived per frame, not
    // cached (a stale frustum culls visible things), and left unnormalised -- only the sign of the
    // distance is read.
    f32 pl[6][4];
    {
        const Mat4& m = viewProj;
        for (int i = 0; i < 4; ++i) {
            pl[0][i] = m.m[i][3] + m.m[i][0];   // left
            pl[1][i] = m.m[i][3] - m.m[i][0];   // right
            pl[2][i] = m.m[i][3] + m.m[i][1];   // bottom
            pl[3][i] = m.m[i][3] - m.m[i][1];   // top
            pl[4][i] = m.m[i][2];               // near   ([0,1] depth, so no m[i][3] term)
            pl[5][i] = m.m[i][3] - m.m[i][2];   // far
        }
    }

    // Resolves one planned draw's material token via the shared authored > dead-handle >
    // named-look > flat-fallback ladder (resolveSurfaceLook, aver/game/SceneSubmission.hpp) plus
    // the MaterialSystem binding. Mirrors SandboxApp::resolveSurface (sandbox/src/SandboxRender.cpp)
    // field for field, but runs once PER PLANNED DRAW rather than once per entity, so a multi-part
    // entity resolves each part's own token. SandboxApp::resolveSurface survives for two readers
    // this walk cannot serve: its warning sentences (colourWarn) and the GPU cluster path's
    // entity-level resolve (colourDecide).
    //
    // Hoisted out of the entity loop: three delivery paths call it now (raster, direct, depth-only),
    // up from two, and it captures nothing per-entity -- only `content`, `materials`, `options`.
    //
    // A bug fix, not a dedup: this lambda used to take the authored branch on `if (authored)`
    // alone, no liveness test, so a dead handle (content.authoredFor returns it but
    // MaterialLibrary::desc() no longer resolves it) drew the identity 1/1/1/1 with metallic and
    // roughness both 1 -- a white mirror that reads as confident lighting rather than missing
    // content (SandboxAssets.cpp's warnDeadMaterialHandle makes the same argument for the editor).
    // resolveSurfaceLook now requires authored AND authoredLive, else falls through to the named
    // look or flat fallback.
    //
    // THE LADDER ITSELF, run once per material token per call: resolveDrawLook below memoises it, and
    // this is the body it runs on a miss. Split out rather than edited so the first sight of a token
    // does exactly what it always did (warn-once, lazy MaterialSystem entry build) and every later
    // draw of that token skips the roughly seven hash lookups that reach the same answer.
    auto buildDrawLook = [&](i32 m) -> DrawLook {
        // WalkResolveLook for this call's whole body: a CpuNest so cost lands in the same bucket
        // regardless of which of the three delivery branches called this lambda, without
        // disturbing the caller's own WalkEmit* phase. Brackets the two GameContent lookups it
        // makes itself (authoredFor, lookFor) too -- not among WalkLookup's four named probes.
        // Opened here, so only a memo MISS pays for it and is counted in that bucket: a repeat of an
        // already-resolved token is a table read charged to the WalkEmit* phase that asked.
        CpuNest resolveLookNest(CpuSpan::WalkResolveLook);
        DrawLook dl;

        SurfaceInputs in;
        u32 authored = 0;
#if AVER_MODULE_PBR && AVER_MODULE_SCENE
        authored = content.authoredFor(m);
        in.authored = authored != 0;
        // The host's half, not a second copy of the rule: SceneSubmission.hpp knows nothing of
        // pbr::MaterialDesc or pbr::isTranslucent (a pure header, unit-testable with no device/RHI),
        // so each host looks these up itself and hands over plain booleans. Read straight from
        // MaterialLibrary, not a GameContent-cached value, since the library is the one place an
        // .ocmat's alphaMode can change after load (material editor hot-reload) -- a cached copy
        // would survive a hot-reload the mesh itself did not. The same desc pointer answers both
        // liveness and translucency, so the liveness test costs no extra lookup.
        const pbr::MaterialDesc* d =
            authored ? pbr::MaterialLibrary::get().desc(authored) : nullptr;
        in.authoredLive = d != nullptr;
        in.translucent = d != nullptr && pbr::isTranslucent(*d);
#endif
        // Asked UNCONDITIONALLY, not only when nothing is authored, because a dead authored
        // handle falls through to exactly this rung -- the whole point of the fix. One extra
        // hash lookup per authored draw, which is what SandboxApp::resolveSurface already pays.
        if (const GameContent::SurfaceLook* builtin = content.lookFor(m)) {
            in.haveLook = true;
            in.lookCol[0] = builtin->col[0];
            in.lookCol[1] = builtin->col[1];
            in.lookCol[2] = builtin->col[2];
            in.lookMetallic = builtin->metallic;
            in.lookRoughness = builtin->roughness;
        }
        dl.look = resolveSurfaceLook(in);

        if (dl.look.warnDeadHandle) {
            // Each host words its own sentence from the shared flag (GameTick.hpp:19 "Both pass
            // their own host tag", GameCamera.hpp:11 use the same split) -- the editor's
            // warnDeadMaterialHandle (sandbox/src/SandboxAssets.cpp:602) says "[Editor]" and names a
            // library only it writes through. Only the DECISION is shared, not the message. The
            // throttle stays here, not the host's: function-local-static, so it's one set per
            // process across however many hosts/walks share it. Not known to fire -- a latent hazard
            // closed on inspection, not a reproduced bug; if this line appears in a log, that's new
            // information.
            static std::unordered_set<i32> warnedDeadMaterialHandles;
            if (warnedDeadMaterialHandles.insert(m).second) {
                if (options.onSurfaceWarn) {
                    options.onSurfaceWarn(m, SurfaceWarning::DeadMaterialHandle, options.user);
                } else {
                    AVER_WARN("[Game] surface '{}' holds a material handle that no longer resolves "
                              "in pbr::MaterialLibrary; falling through to its built-in look or the "
                              "flat gray fallback. Trusted at face value it would have drawn as a "
                              "bright white mirror",
                              aver_scene_material_name(m));
                }
            }
        }
        if (dl.look.usedFallback && m != 0) {
            // Neither an authored .ocmat nor a built-in SurfaceLook claimed this surface; it is
            // about to draw the flat 0.80/0.80/0.85 gray fallback. This is the exact failure a
            // prior parity gap caused: three names (M_Foliage, M_Bark, M_Rock) were registered in
            // SandboxApp.cpp but not here, so an editor-authored level fell through to gray in the
            // packaged game. That gap is closed (GameContent.cpp's registerBuiltins), but the next
            // unmirrored look reproduces it identically -- this warning is the backstop.
            //
            // Gated on a non-zero token: material 0 is "named no material", a legitimate
            // placeholder state, so warning about it would train readers to ignore the line that
            // matters. resolveSurfaceLook can't tell "unnamed" from "named and missing", so the
            // gate belongs here, as SandboxApp::resolveSurface also keeps its own.
            //
            // One-shot, function-local static (initialised once ever, not once per call). Keyed on
            // the interned token (one hash-set lookup, not a string compare); reported by NAME via
            // aver_scene_material_name since the token's value is process-startup-order dependent
            // (scene_abi.h) and means nothing in a log.
            static std::unordered_set<i32> warnedUnresolvedMaterials;
            if (warnedUnresolvedMaterials.insert(m).second) {
                if (options.onSurfaceWarn) {
                    options.onSurfaceWarn(m, SurfaceWarning::UnresolvedSurface, options.user);
                } else {
                    AVER_WARN("[Game] surface '{}' has no authored .ocmat and no built-in look; "
                              "rendering the flat gray fallback (0.80, 0.80, 0.85) instead",
                              aver_scene_material_name(m));
                }
            }
        }
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
        // Guarded on ready(): binding a descriptor table the material system hasn't built yet is a
        // GPU hang here, not a wrong colour (this project already lost a session to an unbound root
        // CBV presenting as "slow geometry shaders").
        //
        // Binds the raw authored handle even when resolveSurfaceLook just rejected it as dead --
        // deliberate parity with SandboxApp::resolveSurface, which sets ResolvedSurface::authored
        // from content_.authoredFor with no liveness test and hands MaterialSystem that same
        // untested handle. MaterialSystem is a different table from MaterialLibrary too (a dropped
        // library handle may still have a built descriptor set here); the colour drawn either way
        // comes from dl.look, which never trusted this handle.
        if (materials && materials->ready()) {
            dl.matSet = materials->bindingSet(authored);
            dl.matConstants = &materials->constants(authored);
            dl.matBytes = sizeof(pbr::MaterialConstants);
        }
#else
        (void)materials;
        // PBR compiled out: `authored` is unused (its only reader is the binding block above) --
        // silence /W4 (CMakeLists.txt:202) rather than let it flag an unwanted local.
        (void)authored;
#endif
        return dl;
    };

    // PER-CALL MEMO OF buildDrawLook, keyed by material token. Every input to the ladder -- the
    // token's authored handle and built-in look in `content`, the MaterialLibrary desc, the
    // MaterialSystem entry and its binding -- is fixed for the length of one walk (MaterialSystem::
    // update() runs from VoxiRenderer::prePass, a frame-level callback, not from anything a walk or
    // its hooks call, and the hooks neither edit nor destroy a material), so the second draw of a
    // token can only reproduce the first's answer; it just used to pay roughly seven hash lookups and
    // two clock reads to say so, per planned draw.
    //
    // WHAT THE FIRST SIGHT OF A TOKEN STILL DOES, AND ALL IT DOES: the ladder, so the warn-once
    // bookkeeping (onSurfaceWarn / AVER_WARN, throttled per process by their own function-local sets)
    // and MaterialSystem's lazy entry build fire on the same first draw they always did. `matConstants`
    // points into MaterialSystem's own storage (an unordered_map node or fallbackConstants_), which
    // only update()'s eviction erases, so the pointer memoised here is the pointer a fresh call
    // would return. The memo dies with the call: the depth prepass and the colour walk each build
    // their own, so nothing carries across the update() between frames.
    auto resolveDrawLook = [&](i32 m) -> DrawLook {
        bool hit = false;
        DrawLookMemoSlot* const memo = cacheLease.findLook(m, hit);
        if (hit) return memo->look;
        const DrawLook built = buildDrawLook(m);
        // Null = no lease or a full probe window: correct, just unmemoised (see findLook).
        if (memo) cacheLease.commitLook(*memo, m, built);
        return built;
    };

    // A level sequence's emissive multiplier, applied to a stack copy of the constants: the memoised
    // look is shared by every entity of the material and must stay untouched. The copy is only valid
    // until the next call, which is fine, every sink copies the bytes before returning.
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
    pbr::MaterialConstants emissiveCopy;
    auto constantsFor = [&](scene::Entity e, const DrawLook& dl) -> const void* {
        f32 rgb[3];
        if (!options.emissiveScale || !dl.matConstants || !options.emissiveScale(e, rgb, options.user))
            return dl.matConstants;
        emissiveCopy = *static_cast<const pbr::MaterialConstants*>(dl.matConstants);
        emissiveCopy.emissiveFactor[0] *= rgb[0];
        emissiveCopy.emissiveFactor[1] *= rgb[1];
        emissiveCopy.emissiveFactor[2] *= rgb[2];
        return &emissiveCopy;
    };
#else
    auto constantsFor = [](scene::Entity, const DrawLook& dl) -> const void* { return dl.matConstants; };
#endif

    const u32 n = w.count();
    // Visit order is the caller's; hooks are told `oi`, the position in it, since the editor's
    // occlusion pass-1/pass-2 boundary is an index into THIS sequence, not the world's (see
    // DrawWorldOptions::visitOrder). Null means 0..n-1, the order this walk has always used and the
    // only order a game host has ever wanted.
    const u32 visitCount = options.visitOrder ? options.visitOrderCount : n;
    for (u32 oi = 0; oi < visitCount; ++oi) {
        // Re-opens WalkEntity, closing whichever phase the previous iteration left open (a
        // WalkEmit* route, WalkOther, or -- on the first iteration -- WalkEntity itself, from
        // `lap`'s constructor). One call per visited index, so WalkEntity's `calls` count means
        // "entities looked at", not "entities drawn" -- a skipped entity still opens this phase and
        // gets counted via the next `.to()` or (on the last entity) `lap`'s destructor.
        lap.to(CpuSpan::WalkEntity);
        const u32 i = options.visitOrder ? options.visitOrder[oi] : oi;
        // Guards World::at, which indexes its dense array with no bound test. Unreachable on the
        // default path (i == oi < n); one compare on a reordered visitOrder.
        if (i >= n) continue;
        const scene::Entity ent = w.at(i);
        // Fires for every visited index before any filter, deliberately: the caller needs a point
        // in the COMMAND STREAM, not a list of entities (see DrawWorldVisitFn). Below this can
        // `continue`.
        if (options.onVisit) options.onVisit(oi, ent, options.user);
        if (w.destroyPending(ent)) continue;
        const scene::CMeshRenderer* mr =
            w.component<scene::CMeshRenderer>(ent, scene::kComponentMeshRenderer);
        // No renderer, or nothing assigned, is the ordinary case and says nothing: most entities in
        // a level carry no mesh at all.
        if (!mr || mr->mesh == 0) continue;
        // Split out of the composite guard purely to be reported: same three conditions (no
        // renderer, not visible, no mesh) as one `||` chain, all side-effect-free continues, so the
        // skipped set can't differ -- only WHICH one is told changes. Worth telling because a named
        // mesh with the visible bit clear is the zero-fill trap, not the ordinary case, and used to
        // drop silently. See DrawSkipReason.
        if (!(mr->flags & scene::kMeshRendererVisible)) {
            if (options.onSkipped)
                options.onSkipped(ent, mr->mesh, DrawSkipReason::NotVisible, options.user);
            continue;
        }

        // This entity's cache slot, resolved once (at the first WalkLookup site) and threaded by
        // pointer through the three sites that follow (the last, content.partsFor, runs after
        // options.decide) rather than re-derived. Safe to hold: the table is claimed by this call
        // alone (WalkCacheLease) and invisible to every DrawWorldOptions hook, so only the NEXT
        // iteration's own findMesh call could evict it. Null with the cache off, sending every site
        // below down its original direct-to-GameContent path.
        MeshLookupCacheSlot* meshSlot = nullptr;

        // content.meshFor is the first of WalkLookup's four named probes (CpuSpan's comment).
        // Wrapped in an IIFE so the probe is timed without changing where `handle` (a plain value)
        // lands. Unconditional for every entity, so there's no per-entity gate to respect -- resolved
        // once per mesh id, reused by every entity that shares it.
        //
        // THE WalkLookup CpuNest WRAPS THE PROBE, NOT THE CACHE CHECK, at all four sites: WalkLookup
        // is "GameContent hash probes ONLY", and a cache hit is not one -- it is a compare and a load
        // that used to be bracketed by two clock reads per site per entity. A hit's few nanoseconds
        // land in the enclosing phase instead, which is where they belong; the bucket's `calls` now
        // counts probes actually made (the misses, or every lookup with the cache off), and the
        // hit/miss line printed under the tree says how many were saved.
        const rhi::MeshHandle handle = [&] {
            if (useLookupCache) {
                meshSlot = &cacheLease.findMesh(mr->mesh);
                if (meshSlot->handleResolved) {
                    ++meshLookupHits;
                } else {
                    {
                        CpuNest lookupNest(CpuSpan::WalkLookup);
                        meshSlot->handle = content.meshFor(mr->mesh);
                    }
                    meshSlot->handleResolved = true;
                    ++meshLookupMisses;
                }
                return meshSlot->handle;
            }
            CpuNest lookupNest(CpuSpan::WalkLookup);
            return content.meshFor(mr->mesh);
        }();
        if (!handle) {
            if (options.onSkipped)
                options.onSkipped(ent, mr->mesh, DrawSkipReason::MeshNotLoaded, options.user);
            continue;
        }

        // worldMatrix is non-const on World, which is why this takes a non-const World&.
        const Mat4& wm = w.worldMatrix(ent);
        // Moving during Play (PlayMobility.hpp): its draws stay out of Voxi's GI bake. Asked on
        // every walk, the depth prepass included, so the first walk of a frame is the one that
        // updates the tracker and the colour walk reads the same answer back.
        const bool movableHere = options.mobility && options.mobility->movable(w, ent, wm);

        // The seam, in one line: a skinned entity's posed vertices live in a DIFFERENT MeshHandle
        // sharing this one's index buffer, so substituting the handle reaches every pass at once.
        // Zero means "not skinned", never "not drawn". Asked once, here, not again for the bounds
        // guard: a host reading EntityDecision::posedMesh must see the same answer that guard used.
        const rhi::MeshHandle posedMesh = skinning ? skinning->drawHandle(ent) : 0;
        const bool skinned = posedMesh != 0;

        // A static entity gets its bounds from the asset; a skinned one already has them written
        // this frame by SkinnedScene from its actual pose -- overwriting with the rest box would
        // reintroduce the popping this guard exists to stop. Written before skinning was wired; it
        // is live now.
        //
        // content.boundsFor is WalkLookup's second probe, nested around the probe itself only,
        // inside the SAME `!skinned` condition that gates whether it runs at all -- pulling it
        // ahead would probe (or cache) a skinned entity's bounds, which never happened before.
        // `boundsResolved` stays false per mesh id until the first `!skinned` entity reaches it, so
        // a mesh drawn only by skinned entities is never probed, cache on or off.
        if (!skinned) {
            bool haveBounds = false;
            Vec3 boundsMin{}, boundsMax{};
            if (useLookupCache) {
                // meshSlot is never null here: set by the meshFor call above this same
                // iteration for this same mr->mesh; nothing before here could have evicted it.
                if (!meshSlot->boundsResolved) {
                    const auto* found = [&] {
                        CpuNest lookupNest(CpuSpan::WalkLookup);
                        return content.boundsFor(mr->mesh);
                    }();
                    meshSlot->haveBounds = found != nullptr;
                    // A copy of what boundsFor found, not its pointer -- see
                    // MeshLookupCacheSlot's boundsMin/boundsMax comment.
                    if (found) { meshSlot->boundsMin = found->first; meshSlot->boundsMax = found->second; }
                    meshSlot->boundsResolved = true;
                    ++meshLookupMisses;
                } else {
                    ++meshLookupHits;
                }
                haveBounds = meshSlot->haveBounds;
                boundsMin = meshSlot->boundsMin;
                boundsMax = meshSlot->boundsMax;
            } else {
                CpuNest lookupNest(CpuSpan::WalkLookup);
                if (const auto* b = content.boundsFor(mr->mesh)) {
                    haveBounds = true;
                    boundsMin = b->first;
                    boundsMax = b->second;
                } else {
                    haveBounds = false;
                }
            }
            // The write-back is per entity, always, cache or no cache, hit or miss -- memoising the
            // LOOKUP must never memoise or skip this: every `!skinned` entity still gets its own
            // CMeshRenderer::aabbMin/aabbMax written, exactly as before this cache existed.
            if (haveBounds) {
                auto* mw = const_cast<scene::CMeshRenderer*>(mr);
                mw->aabbMin[0] = boundsMin.x; mw->aabbMin[1] = boundsMin.y; mw->aabbMin[2] = boundsMin.z;
                mw->aabbMax[0] = boundsMax.x; mw->aabbMax[1] = boundsMax.y; mw->aabbMax[2] = boundsMax.z;
            }
        }

        // ---- OWNER HIDE, DECIDED BEFORE THE CULL ----
        // The only ancestor walk now -- the editor's copy went with its loop; it feeds this one via
        // DrawWorldOptions::ownerHideRoot (SandboxRender.cpp sets it from firstPersonPawn_ for both
        // passes). An entity both frustum-culled and owner-hidden must still carry
        // hiddenFromOwner=true on its direct-route delivery below, or it would be primary-visible
        // again in ray-driven mode the moment it re-enters frame (the 0d3bcf1 regression chooseRoute
        // pins a test against). Ancestor walk, not a direct-parent compare: a COMP tree can nest
        // several hops. Guaranteed to terminate -- World::setParent already refuses a cycle.
        bool ownerHiddenHere = false;
        if ((mr->flags & scene::kMeshRendererHiddenFromOwner) &&
            options.ownerHideRoot != scene::kInvalidEntity) {
            for (scene::Entity anc = ent; w.valid(anc); anc = w.parent(anc)) {
                if (anc == options.ownerHideRoot) { ownerHiddenHere = true; break; }
            }
        }

        // Frustum cull on the world-space extent of the entity's own box. A DEGENERATE box is DRAWN
        // rather than culled: an entity whose bounds were never filled in must not vanish, and being
        // conservative costs a draw call where being wrong costs a character.
        //
        // The verdict is stored, not acted on here. Frustum-culled/owner-hidden used to `continue`
        // past every draw call, including Voxi's submitDraw() -- an off-screen caster's shadow
        // vanished with it, and an owner-hidden mesh's shadow never existed at all. The F4 fix
        // (SceneSubmission.hpp) routes it to Voxi directly instead; see the route decision below.
        //
        // The world box outlives the cull block, unlike before there was anyone to hand it to.
        // That scope WAS the bug on the editor's side: a second walk re-derived a sphere from LOCAL
        // aabbMin/aabbMax against a world-space eye while the first built it from these corners, so
        // the same LOD function picked different levels for the same instance -- 2.81% of pixels
        // differing, falling to 0.04% (noise) with --no-lod-select (measured on SandboxRender.cpp's
        // depth-prepass phase, which replaced that second walk). EntityDecision hands the host this
        // box for that reason.
        //
        // WalkFrustum opens here, closing WalkEntity for this iteration: everything above this
        // line is the "loop overhead" WalkEntity's comment names; everything through the frustum
        // test below builds the world-space box and tests it against the six planes from the top.
        lap.to(CpuSpan::WalkFrustum);
        bool haveWorldBox = false;
        bool frustumCulled = false;
        Vec3 wlo{1e30f, 1e30f, 1e30f}, whi{-1e30f, -1e30f, -1e30f};
        {
            const Vec3 lo{mr->aabbMin[0], mr->aabbMin[1], mr->aabbMin[2]};
            const Vec3 hi{mr->aabbMax[0], mr->aabbMax[1], mr->aabbMax[2]};
            if (hi.x > lo.x && hi.y > lo.y && hi.z > lo.z) {
                for (u32 c = 0; c < 8; ++c) {
                    const Vec3 p{(c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z};
                    const Vec3 t = xformPoint(wm, p);
                    wlo.x = std::fmin(wlo.x, t.x); whi.x = std::fmax(whi.x, t.x);
                    wlo.y = std::fmin(wlo.y, t.y); whi.y = std::fmax(whi.y, t.y);
                    wlo.z = std::fmin(wlo.z, t.z); whi.z = std::fmax(whi.z, t.z);
                }
                haveWorldBox = true;
                bool outside = false;
                for (u32 pi = 0; pi < 6 && !outside; ++pi) {
                    // The corner FURTHEST along the plane normal. If even that one is behind, every
                    // corner is, and only then is the box definitely out.
                    const f32 d = pl[pi][0] * (pl[pi][0] > 0 ? whi.x : wlo.x)
                                + pl[pi][1] * (pl[pi][1] > 0 ? whi.y : wlo.y)
                                + pl[pi][2] * (pl[pi][2] > 0 ? whi.z : wlo.z)
                                + pl[pi][3];
                    if (d < 0.0f) outside = true;
                }
                frustumCulled = outside;
            }
        }

        // The entity's own fallback material: 0 means "ask the mesh", not "no material" -- same
        // rule the editor applies. Needed unconditionally: planEntityDraws' single-draw case reads
        // it, and a split part's empty slot falls back to it too (`p.material ? p.material :
        // entityMaterial`).
        //
        // WalkDecide opens here: everything through planEntityDraws() below is the route/material
        // DECISION, distinct from the frustum TEST that closed above.
        lap.to(CpuSpan::WalkDecide);
#if AVER_MODULE_PBR && AVER_MODULE_SCENE
        // content.meshDefaultMaterial is one of WalkLookup's four named probes. A CpuNest, not
        // `.to()`, so it doesn't disturb WalkDecide, and wrapped in an IIFE around just the cache
        // check so the ternary's short-circuit still gates it on `mr->material == 0` exactly as
        // before either existed. Mirrors boundsFor's mesh-id gate: `defaultMaterialResolved` stays
        // false until the first zero-material entity sharing that mesh id asks -- a mesh every
        // entity names its own material for is never probed here, cache on or off.
        const i32 mat = mr->material ? mr->material
                                      : [&] {
                                            if (useLookupCache) {
                                                // meshSlot is never null here -- see its
                                                // declaration comment above.
                                                if (!meshSlot->defaultMaterialResolved) {
                                                    {
                                                        CpuNest lookupNest(CpuSpan::WalkLookup);
                                                        meshSlot->defaultMaterial =
                                                            content.meshDefaultMaterial(mr->mesh);
                                                    }
                                                    meshSlot->defaultMaterialResolved = true;
                                                    ++meshLookupMisses;
                                                } else {
                                                    ++meshLookupHits;
                                                }
                                                return meshSlot->defaultMaterial;
                                            }
                                            CpuNest lookupNest(CpuSpan::WalkLookup);
                                            return content.meshDefaultMaterial(mr->mesh);
                                        }();
#else
        const i32 mat = mr->material;
#endif

        // ---- THE ONE PLACE A HOST GETS A SAY ----
        // Everything above is what this walk knows on its own; everything below acts on it. Asked
        // exactly once, with bounds and both verdicts already computed, before a route is chosen
        // (see EntityDecision on why it's one in/out struct). A null `decide` and one that touches
        // nothing are the same walk -- every OUT field arrives already holding this walk's answer.
        EntityDecision dec;
        dec.pass = options.pass;
        dec.entity = ent;
        dec.visitIndex = oi;
        dec.meshId = mr->mesh;
        dec.material = mat;
        dec.world = &wm;
        dec.worldBoxMin = wlo;
        dec.worldBoxMax = whi;
        dec.haveWorldBox = haveWorldBox;
        dec.skinned = skinned;
        dec.frustumCulled = frustumCulled;
        dec.ownerHidden = ownerHiddenHere;
        dec.baseMesh = handle;
        dec.posedMesh = posedMesh;
        dec.chosenMesh = posedMesh ? posedMesh : handle;
        if (options.decide) options.decide(dec, options.user);
        if (dec.skip) continue;

        // The one place deciding who delivers this entity -- raster's drawMesh() or Voxi's direct
        // submit(). chooseRoute() (SceneSubmission.hpp) is called now, not inlined: it used to be
        // written here with its two editor-only inputs pinned false, on the argument that "there is
        // no decision left to share once the inputs are constants"; dec.occlusionCulled and
        // dec.tint are exactly those inputs ceasing to be constants. With a null `decide` it still
        // returns {raster = !frustumCulled && !ownerHiddenHere, hiddenFromOwner = ownerHiddenHere,
        // tint = false}.
        //
        // dec.tint feeds showCulled, not route.tint directly -- pairing "culled but still raster,
        // tinted there" is chooseRoute's call to make.
        const RouteDecision route =
            chooseRoute(frustumCulled, dec.occlusionCulled, ownerHiddenHere, dec.tint);

        // One mesh naming several materials draws as several meshes, one per slot (the split
        // GameContent::loadProjectMeshes built, mirroring SandboxApp::buildMeshParts), planned by
        // the shared planEntityDraws() -- the ONLY call site in either host now that the editor's
        // three went with its loop. An entity with no parts plans to the one draw this loop always
        // issued.
        //
        // GameContent::MeshPart satisfies planEntityDraws' `Part` template without the header
        // naming it: same two fields (mesh, material), rhi::MeshHandle being a plain u32 alias
        // (RHIResources.hpp:13) means PlannedDraw::mesh's u32 spelling is the identical type.
        //
        // After decide(): dec.chosenMesh is one of planEntityDraws' two inputs, compared against the
        // unsubstituted handle to know if the split still applies (a soft-body/LOD substitution
        // changes the answer -- see planEntityDraws' own comment).
        // content.partsFor is WalkLookup's fourth and last probe, unconditional for every entity, so
        // the cache check only changes whether the probe runs -- resolved once per mesh id, not per
        // entity, same as meshFor above. Caches the POINTER partsFor returns, not a copy of the
        // vector -- see MeshLookupCacheSlot's and DrawWorldOptions::useMeshLookupCache's own
        // comments for why that's safe and why copying was rejected (a heap allocation per mesh,
        // per frame).
        const std::vector<GameContent::MeshPart>* parts = [&] {
            if (useLookupCache) {
                // meshSlot is never null here (still true after decide() runs -- unlike the other
                // three WalkLookup call sites, which all run before it -- since decide() has no
                // reach into this walk's cache table, exposed through neither EntityDecision
                // nor DrawWorldOptions).
                if (!meshSlot->partsResolved) {
                    {
                        CpuNest lookupNest(CpuSpan::WalkLookup);
                        meshSlot->parts = content.partsFor(mr->mesh);
                    }
                    meshSlot->partsResolved = true;
                    ++meshLookupMisses;
                } else {
                    ++meshLookupHits;
                }
                return meshSlot->parts;
            }
            CpuNest lookupNest(CpuSpan::WalkLookup);
            return content.partsFor(mr->mesh);
        }();
        // ---- A SKINNED ENTITY'S SPLIT, RE-CUT OVER ITS POSE ----
        //
        // planEntityDraws only applies the per-material split when the chosen handle is the one it
        // was cut from, so a skinned entity (drawn through a different, posed handle) used to take
        // the single-draw branch: its hair-card material slot never drew with the hair material, no
        // alpha cut-out applied (KenneyChar in PTTest, two materials, drew flat for the same reason).
        //
        // Only fires when the walk's own skinning answer is still the chosen handle -- a
        // soft-body/LOD substitution keeps the single draw (neither has posed parts). The posed
        // parts are cut from posedMesh, so planning from it makes the split-applies test true for
        // exactly them. Any refusal (no split, no skin streams, a reloaded mesh, a backend without
        // createPosedPartMesh) returns null and leaves the whole-mesh draw as before. Creation cost
        // lands once per skin target, on first sight, in WalkLookup.
        const std::vector<GameContent::MeshPart>* planParts = parts;
        rhi::MeshHandle planFrom = handle;
        if (parts != nullptr && posedMesh != 0 && dec.chosenMesh == posedMesh) {
            CpuNest lookupNest(CpuSpan::WalkLookup);
            if (const auto* posed = content.posedPartsFor(device, mr->mesh, handle, posedMesh)) {
                planParts = posed;
                planFrom = posedMesh;
            }
        }
        // Fills the walk-wide pdraws[] (declared above the loop) from slot 0 and reports how many.
        const u32 pdrawCount = planEntityDraws(
            planFrom, dec.chosenMesh,
            planParts ? planParts->data() : nullptr,
            planParts ? static_cast<u32>(planParts->size()) : 0u,
            mat, pdraws, kMaxPlannedDraws);

        if (depthPass) {
            // WalkEmitDepth: mutually exclusive with the raster/direct routes below since
            // `depthPass` is fixed for the whole drawWorld() call (never touched per entity) --
            // fires every iteration of a depth-prepass call, never on a colour-pass one.
            lap.to(CpuSpan::WalkEmitDepth);
            // ---- DEPTH-ONLY DELIVERY ----
            // A non-raster entity writes no depth and is not counted -- this pass is one half of a
            // frame the colour pass finishes, and both the counters and the "who delivered" hook
            // belong to that half. See DrawWorldPass.
            if (!route.raster) continue;
            for (u32 pdi = 0; pdi < pdrawCount; ++pdi) {
                const PlannedDraw& pd = pdraws[pdi];
                if (!pd.mesh) continue;
                const auto dl = resolveDrawLook(pd.material);
                // Glass must never write depth -- unlike other prepass exclusions (skinned,
                // cluster-dispatched), which write depth some other way, this one is because the
                // blended replay runs with depth-WRITE off so a translucent surface never occludes
                // what's behind it. Pre-writing opaque depth for glass would leave it unconsumed
                // (the colour pass's own per-draw prepass gate excludes `blended` too) and make
                // objects behind the glass fail depth-test against it, vanishing under it instead of
                // showing through.
                if (dl.look.blended) continue;
                if (dl.matBytes) device.setDrawBinding(dl.matSet, constantsFor(ent, dl), dl.matBytes);
                // dl.look.col: same base colour the colour pass hands drawMesh (minus the debug
                // tint, which only touches .g). PSDepthPrepass's alpha test multiplies by its .a;
                // without it every alpha-masked material clipped every pixel and wrote no depth.
                device.drawMeshDepthPrepass(pd.mesh, &wm.m[0][0], dl.look.col);
            }
            continue;
        }

        // The raster route, minus entities someone else already shaded. dec.colourAlreadyDrawn is
        // read only here, on an entity already routed to raster: its lit pixels came from something
        // the host dispatched itself, so drawMesh() would double-draw it -- but drawMesh() is the
        // ONLY path to IRenderFeature::submitDraw, so skipping it would silently drop shadows/GI/TLAS
        // registration. It takes the direct route below instead, and still counts as drawn.
        const bool rasterDraws = route.raster && !dec.colourAlreadyDrawn;
        if (rasterDraws) {
            // WalkEmitRaster: mutually exclusive with WalkEmitDepth (already `continue`d above when
            // depthPass) and WalkEmitDirect below (an entity takes exactly one, per iteration).
            lap.to(CpuSpan::WalkEmitRaster);
#if AVER_MODULE_VOXI
            // drawMesh reaches Voxi through IRenderFeature::submitDraw, which has no movable
            // parameter; the flag rides on Voxi itself for these draws and is cleared after them.
            if (options.voxiRenderer) options.voxiRenderer->setSubmitMovable(movableHere);
#endif
            for (u32 pdi = 0; pdi < pdrawCount; ++pdi) {
                const PlannedDraw& pd = pdraws[pdi];
                if (!pd.mesh) continue;
                const auto dl = resolveDrawLook(pd.material);
                // occlusion.showCulled's magenta: knocks the green channel down (*0.15) so a false
                // cull is obvious. A branch, not a multiply that's silently an identity at 1.0 when
                // off -- one bool test per draw is what this debug view may cost. route.tint is
                // false on every ordinary frame, so `col` is a copy of dl.look.col, value for value.
                f32 col[4] = {dl.look.col[0], dl.look.col[1], dl.look.col[2], dl.look.col[3]};
                if (route.tint) col[1] *= 0.15f;
                if (dl.matBytes) device.setDrawBinding(dl.matSet, constantsFor(ent, dl), dl.matBytes);

                // Sticky on the device (RHI.hpp's setDrawBlended), so set on EVERY draw, not only
                // when true -- skipping false would leave the flag set for whatever draws next,
                // silently taking the blended path (no RT shadow, no GI bounce, no shadow-cascade
                // write) via scenePipeline(..., blended=true) instead of the opaque pipeline, just
                // from ordering.
                device.setDrawBlended(dl.look.blended);
                // Per draw, auto-consumed by the very next drawMesh() (not sticky -- RHI.hpp's
                // setNextDrawPrepassed comment), which is why this can't hoist to the entity: one
                // call before a multi-part loop would cover part 0 alone, silently asking the
                // LessEqual/no-write pipeline for parts no depth-prepass walk ever wrote depth for.
                // dec.prepassEligible is the host's half: false unless a host says it ran a prepass
                // walk.
                bool prepassedDraw = dec.prepassEligible && !dl.look.blended;
#if AVER_MODULE_PBR && AVER_MODULE_VOXI
                // ---- AN ALPHA-MASKED DRAW WRITES ITS OWN DEPTH FIRST WHEN NO PREPASS DID ----
                //
                // The scene colour shader forces early depth ([earlydepthstencil] on PSMainVoxi,
                // 39af0e32 -- load-bearing: without it hidden fragments poison the AO history and
                // shade for nothing). Under forced early depth the WRITE happens before the shader
                // runs, so averEvalMaterial's `clip(s.alpha - a.alphaCutoff)` (material_prelude.hlsl)
                // can't take the depth back: every cut-out texel wrote opaque depth, hiding whatever
                // was drawn behind it. 39af0e32 claimed PSMainVoxi had no alpha-cutout discard -- it
                // only searched voxi.hlsl; the clip lives in the prelude. Found by adversarial review.
                //
                // The fix reuses the prepass machinery for one draw: depth goes through
                // PSDepthPrepass, which does not force early depth and clips before writing, then
                // colour goes through the LessEqual/no-write twin, leaving early depth nothing to
                // write. Only alpha-masked draws the frame-wide prepass didn't cover pay for this;
                // with --depth-prepass on, they're already covered and it doesn't fire.
                //
                // Skinned/soft-body meshes are covered too: the posed handle is depth-drawn from
                // the same compute-written buffer its colour draw reads (IDevice::drawMeshDepthOnly).
                // Only an alpha-masked material can reach this branch, so opaque characters are
                // untouched.
                //
                // Known residuals: cluster-dispatched geometry never reaches this loop, and a
                // material graph driving opacity outside the stock alpha can disagree with
                // PSDepthPrepass at cut-out edges, same as under the frame-wide prepass.
                if (!prepassedDraw && !dl.look.blended && dl.matConstants &&
                    (static_cast<const pbr::MaterialConstants*>(dl.matConstants)->flags &
                     pbr::MaterialFlag_AlphaMask) != 0u) {
                    // `col` is the exact array drawMesh gets below; the depth shader's alpha test
                    // reads its .a. Marked prepassed only if depth was actually written -- false in
                    // wireframe, under a scene-suppressing feature, or with no depth-only pipeline,
                    // each of which must keep its ordinary pipeline.
                    prepassedDraw = device.drawMeshDepthOnly(pd.mesh, &wm.m[0][0], col);
                }
#endif
                if (prepassedDraw) device.setNextDrawPrepassed(true);
                device.drawMesh(pd.mesh, &wm.m[0][0], col, dl.look.metallic, dl.look.roughness);
            }
#if AVER_MODULE_VOXI
            if (options.voxiRenderer) options.voxiRenderer->setSubmitMovable(false);
#endif
        } else if (dec.emitDirectDraws &&
                   (options.voxiRenderer != nullptr || options.onDirectDraw != nullptr)) {
            // WalkEmitDirect: the culled/hidden direct route to Voxi, mutually exclusive with
            // WalkEmitRaster above. This is the bucket the whole stage turns on -- a culled entity
            // still reaches resolveDrawLook and VoxiRenderer::submit, which is why this stage's cost
            // curve is linear in entity count, not draw count: shadows/GI/TLAS never depend on
            // raster visibility, so a culled entity pays nearly the same price as a drawn one here,
            // on purpose.
            lap.to(CpuSpan::WalkEmitDirect);
            // The unified direct route: frustum-culled, occlusion-culled or owner-hidden (or
            // already shaded by the host) is handed straight to Voxi. This is the route for both
            // hosts now, from the editor's deleted emitEntityDraws else-branch (its
            // voxiRenderer_.submit() call). Submits with the same mesh/look/translucency the raster
            // route would use, plus hiddenFromOwner so a possessed pawn's body stays out of
            // ray-driven primary visibility (voxi.hlsl's AVER_RT_MASK_OWNER_HIDDEN lane) while still
            // casting a shadow and bouncing light, exactly like the raster walk did. Per planned
            // draw, not per entity, so a culled multi-material entity's parts keep their own
            // materials, the same split the raster route gets. Both sinks null reproduces this
            // walk's pre-existing behaviour exactly.
            //
            // The two sinks are independent, deliberately. The editor's path-traced scene view is
            // reached only from here -- PtSceneView::submitDraw is otherwise only called through
            // drawMesh(), the very call this route exists to skip, so before it was fed from here the
            // path tracer traced only what the camera could see. Gating it on VoxiRenderer would let
            // one feature's absence disable an unrelated one.
            //
            // What each draw carries is deliver()'s answer (SceneSubmission.hpp), not written out
            // again here -- the translucency flag and hiddenFromOwner used to be spelled inline,
            // leaving deliver() an uncalled duplicate of the rule it exists to state once. Now that
            // the editor runs this walk too, calling it here makes it the only statement in either
            // host.
            //
            // hiddenFromOwner follows the real route, not this branch: deliver() keys it on
            // route.raster, so an entity here only because the host already shaded it carries false,
            // exactly as the raster draw it replaces would have.
            for (u32 pdi = 0; pdi < pdrawCount; ++pdi) {
                const PlannedDraw& pd = pdraws[pdi];
                if (!pd.mesh) continue;
                const auto dl = resolveDrawLook(pd.material);
                const VoxiDelivery del = deliver(pd, dl.look, route);
                const void* const directConstants = constantsFor(ent, dl);
                f32 col[4] = {dl.look.col[0], dl.look.col[1], dl.look.col[2], dl.look.col[3]};
                if (route.tint) col[1] *= 0.15f;
#if AVER_MODULE_VOXI
                if (options.voxiRenderer) {
                    options.voxiRenderer->submit(del.mesh, &wm.m[0][0], col, dl.look.metallic,
                                                 dl.look.roughness, dl.matSet, directConstants,
                                                 dl.matBytes, del.translucent, del.hiddenFromOwner,
                                                 movableHere);
                }
#endif
                if (options.onDirectDraw) {
                    options.onDirectDraw(del.mesh, &wm.m[0][0], col, dl.look.metallic,
                                         dl.look.roughness, dl.matSet, directConstants, dl.matBytes,
                                         del.translucent, del.hiddenFromOwner, options.user);
                }
            }
        }

        // WalkOther: bookkeeping only (counters, onEntityDelivered), charged explicitly rather than
        // left to inherit whichever WalkEmit* ran last -- needed for the case where NEITHER branch
        // runs (route.raster false or dec.colourAlreadyDrawn true, and no direct-route sink attached:
        // no voxiRenderer, no onDirectDraw, even though dec.emitDirectDraws defaults true). That's
        // not rare: it's every colour-pass call with no Voxi feature attached. Without this `.to()`,
        // such an entity's cost would silently stay on WalkDecide -- exactly the unnamed gap
        // WalkOther exists to rule out (see CpuSpan's own comment).
        lap.to(CpuSpan::WalkOther);
        if (route.raster) {
            // Once per delivered entity, after its draws, with the handle actually used -- the
            // editor latches its selection outline here (colourDelivered); an outline from the base
            // handle while a posed/LOD copy rendered would trace the wrong silhouette.
            if (options.onEntityDelivered)
                options.onEntityDelivered(ent, mr->mesh, dec.chosenMesh, wm, /*raster=*/true,
                                          options.user);
            ++drawn;
        } else {
            // Not fired for an entity dec.emitDirectDraws held back: nothing was delivered on
            // either route, and a "delivered" hook firing for a draw that didn't happen is a trap.
            if (options.onEntityDelivered && dec.emitDirectDraws)
                options.onEntityDelivered(ent, mr->mesh, dec.chosenMesh, wm, /*raster=*/false,
                                          options.user);
            // The counting convention is this walk's, both hosts word their sentence from it: an
            // entity both frustum-culled and owner-hidden counts as culled, never owner-hidden -- even
            // though its direct-route delivery above always carries hiddenFromOwner=true regardless (a
            // convention, not a correctness question). dec.occlusionCulled joins the culled bucket
            // too, else the feature would report as free -- why the editor relabelled its line from
            // "frustum-culled" to plain "culled".
            if (frustumCulled || dec.occlusionCulled) ++culled; else ++ownerHidden;
        }
    }

    // Not in the depth pass: these counters describe a FRAME, and a host running this walk twice
    // over the same entities must not see the count doubled or this line fire twice
    // (DrawWorldPass::DepthPrepass). The mesh-cache counters follow the identical rule -- a
    // depth-prepass call's own hits/misses are
    // deliberately dropped, not added to the colour pass's, to avoid reporting a rate no call
    // actually produced.
    if (!depthPass) {
        stats.drawn = drawn;
        stats.culled = culled;
        stats.ownerHidden = ownerHidden;
        stats.meshLookupCacheHits = meshLookupHits;
        stats.meshLookupCacheMisses = meshLookupMisses;
        if (drawn != stats.lastDrawn || culled != stats.lastCulled ||
            ownerHidden != stats.lastOwnerHidden) {
            // Suppressed, not reworded, for a host with its own sentence: the editor names
            // "spawned CMeshRenderer entities" and counts occlusion into its "culled" (written from
            // colourStats once the walk returns) -- vocabulary a packaged game has no business
            // borrowing. The counters above are what such a host reads instead. See SceneDrawStats.
            if (!options.suppressLog) {
                AVER_INFO("[Game] scene-render: {} drawn, {} frustum-culled, {} owner-hidden",
                          drawn, culled, ownerHidden);
            }
            stats.lastDrawn = drawn;
            stats.lastCulled = culled;
            stats.lastOwnerHidden = ownerHidden;
        }
    }
}

} // namespace aver::game

#endif // AVER_MODULE_SCENE
