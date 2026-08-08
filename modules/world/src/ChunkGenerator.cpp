#include "aver/world/ChunkGenerator.hpp"

#if AVER_MODULE_SCENE

#  include "aver/core/Hash.hpp"
#  include "aver/world/ChunkCodec.hpp"

#  include <cmath>

namespace aver::world {
namespace {

// splitmix32, written out again rather than reached for.
//
// pcg's own hash32 is file-static inside modules/render.pcg, and this needs the SAME mixer to name
// its entities stably. Copying twenty bytes of arithmetic beats exporting a symbol whose only caller
// is here -- and tests/pcg's PcgMirrorTest is what pins the constants, so a divergence between the
// two copies fails a test rather than silently renaming a world's contents.
u32 mix32(u32 x) {
    u32 z = x + 0x9E3779B9u;
    z = (z ^ (z >> 16)) * 0x21F0AAADu;
    z = (z ^ (z >> 15)) * 0x735A2D97u;
    return z ^ (z >> 15);
}

u32 mixChunk(u64 seed, const ChunkCoord& c, u32 salt) {
    u32 h = mix32(static_cast<u32>(seed));
    h = mix32(h ^ static_cast<u32>(seed >> 32));
    h = mix32(h ^ static_cast<u32>(c.x));
    h = mix32(h ^ static_cast<u32>(c.y));
    h = mix32(h ^ static_cast<u32>(c.z));
    return mix32(h ^ salt);
}

// A SECOND, INDEPENDENT DRAW off the same per-candidate hash, rather than stealing more bits out
// of it. `h` already commits bits [0,9] to yaw and [10,17] to scale (below); reusing any of those
// for species choice would correlate species with rotation/size in a way nothing asked for, and
// running out of bits as the palette grows is a real risk with only 14 left. mix32 is a full
// avalanche, so XOR-ing in a fixed salt and mixing again is a cheap, still-pure second value that
// depends on nothing but `h` -- and therefore, transitively, on nothing but (worldSeed, coord, salt).
constexpr u32 kPaletteSpeciesSalt = 0xA5A5C3C3u;
u32 mixSpecies(u32 h) { return mix32(h ^ kPaletteSpeciesSalt); }

// Picks a species index for a candidate whose density `d` already passed the existence threshold.
// Returns -1 when no species in `pal` is eligible (empty palette, every weight <= 0, or `d` falls
// outside every band) -- the candidate then places nothing at all, despite having passed threshold.
//
// PURE IN hSpecies AND d ONLY. No other state, so calling this twice with the same inputs -- from
// the same chunk, from a different chunk, in any order -- always agrees.
i32 pickSpecies(const std::vector<ScatterSpecies>& pal, f32 d, u32 hSpecies) {
    f64 total = 0.0;
    for (const ScatterSpecies& s : pal) {
        if (s.weight <= 0.0f) continue;
        if (d < s.densityMin || d > s.densityMax) continue;
        total += static_cast<f64>(s.weight);
    }
    if (total <= 0.0) return -1;

    // hSpecies is uniform over [0, 2^32); scale it into [0, total) and walk the cumulative weight.
    const f64 target = (static_cast<f64>(hSpecies) / 4294967296.0) * total;
    f64 cum = 0.0;
    i32 lastEligible = -1;
    for (usize idx = 0; idx < pal.size(); ++idx) {
        const ScatterSpecies& s = pal[idx];
        if (s.weight <= 0.0f) continue;
        if (d < s.densityMin || d > s.densityMax) continue;
        lastEligible = static_cast<i32>(idx);
        cum += static_cast<f64>(s.weight);
        if (target < cum) return static_cast<i32>(idx);
    }
    // Only reachable through float rounding at the very top of the range -- fall back to the last
    // eligible entry rather than dropping a candidate that should have placed something.
    return lastEligible;
}

} // namespace

void GeneratedChunkSource::setSettings(const GeneratorSettings& s) {
    settings_ = s;
    if (!chunkSizeValid(settings_.chunkSizeCm)) settings_.chunkSizeCm = kDefaultChunkSizeCm;
    if (settings_.samplesPerAxis == 0) settings_.samplesPerAxis = 1;
    if (settings_.octaves == 0) settings_.octaves = 1;
    memoValid_ = false;

    // RESOLVED ONCE, HERE -- never inside generate(). An empty palette becomes exactly the one
    // species the old single-mesh generator placed, built from meshPath/material with the palette
    // struct's own default scale range (0.75..1.25, matching the old hardcoded range) and an
    // unbounded density band, so a caller that never touches the palette sees byte-identical output.
    if (settings_.palette.empty()) {
        ScatterSpecies def;
        def.meshPath = settings_.meshPath;
        def.material = settings_.material;
        resolvedPalette_ = {def};
    } else {
        resolvedPalette_ = settings_.palette;
    }

    spec_ = pcg::InfiniteSpec{};
    spec_.seed = static_cast<i32>(settings_.worldSeed ^ (settings_.worldSeed >> 32));
    spec_.layerCount = 1;
    spec_.layers[0].frequency = 1.0f;
    spec_.layers[0].amplitude = 1.0f;
    spec_.layers[0].octaves = static_cast<i32>(settings_.octaves);
    spec_.layers[0].lacunarity = 2.0f;
    spec_.layers[0].gain = 0.5f;
    spec_.layers[0].seedOffset = 0;
    spec_.cellSizeCm = settings_.featureSizeCm > 0.0f ? settings_.featureSizeCm : 1600.0f;
    // PINNED, and the header says why: these two are the only route to std::pow inside
    // sampleInfinite, and pow is the one operation there that IEEE 754 does not fix across libm
    // implementations. With floor 0 and bias 1 the function reduces to multiplies, adds and one
    // divide -- all exactly specified -- so "the same seed generates the same world" is a property
    // of the arithmetic rather than of the C runtime that happens to be linked.
    spec_.coverageFloor = 0.0f;
    spec_.coverageBias = 1.0f;
}

ChunkPayload GeneratedChunkSource::generate(const ChunkCoord& c) const {
    ChunkPayload p;
    p.coord = c;
    p.chunkSizeCm = settings_.chunkSizeCm;
    if (c.z != settings_.surfaceChunkZ) return p;   // a surface world: everything else is air

    const i64 ox = chunkOriginCmAxis(c.x, settings_.chunkSizeCm);
    const i64 oy = chunkOriginCmAxis(c.y, settings_.chunkSizeCm);
    const i64 oz = chunkOriginCmAxis(c.z, settings_.chunkSizeCm);
    const u32 n = settings_.samplesPerAxis;
    const f32 step = static_cast<f32>(settings_.chunkSizeCm) / static_cast<f32>(n);

    // IN-CHUNK-ONLY interpenetration rejection. Big species register themselves here as they are
    // accepted and reject a later candidate whose circle would overlap one already placed.
    //
    // WHY THIS STAYS DETERMINISTIC: candidates are walked in a FIXED raster order -- salt = j*n+i,
    // ascending, always -- which depends only on n. That order is the same every time generate(c)
    // is called for this c, so "did an earlier candidate in THIS chunk claim this spot" is itself a
    // pure function of (worldSeed, coord, salt), never of which OTHER chunks were generated first or
    // in what order streaming asked for them. `placedSolid` is a local, lives only for this call, and
    // is never touched outside it -- there is no member or static counter here for a different chunk
    // order to disagree about.
    //
    // THE CHUNK BOUNDARY CASE IS NOT SOLVED. A big species candidate near an edge only ever sees
    // OTHER candidates inside the SAME chunk; a neighbour chunk's candidates are invisible here,
    // whether or not that neighbour has been generated yet (chunks are generated independently, and
    // streaming does not generate them in any particular order relative to each other -- see
    // ChunkStreamer.cpp). Two big species can therefore still interpenetrate across a chunk seam.
    // Solving it properly needs either sampling a margin into each neighbour (which multiplies
    // generation cost per chunk and makes a chunk's result depend on reading outside its own bounds)
    // or a placement grid that is not chunk-scoped at all -- both bigger changes than this task's
    // scope. Keeping species that need real clearance away from typical chunk edges, or accepting
    // the occasional seam overlap, are the only mitigations today.
    struct PlacedSolid { f32 x, y, r; };
    std::vector<PlacedSolid> placedSolid;

    for (u32 j = 0; j < n; ++j) {
        for (u32 i = 0; i < n; ++i) {
            // Candidate at the centre of its cell, so no sample ever lands exactly on a chunk face --
            // where the floor in sampleInfinite would make the result depend on which side asked.
            const f32 lx = (static_cast<f32>(i) + 0.5f) * step;
            const f32 ly = (static_cast<f32>(j) + 0.5f) * step;
            // World position built from an INTEGER origin plus a small local offset, in f64 and
            // narrowed once -- the same trick the coordinate hierarchy exists for. Accumulating in
            // f32 would make a chunk's contents depend on how far from the origin it is.
            const f32 wx = static_cast<f32>(static_cast<f64>(ox) + lx);
            const f32 wy = static_cast<f32>(static_cast<f64>(oy) + ly);
            const f32 wz = static_cast<f32>(oz);

            const f32 d = pcg::sampleInfinite(spec_, wx, wy, wz);
            if (d <= settings_.threshold) continue;

            const u32 salt = j * n + i;
            const u32 h = mixChunk(settings_.worldSeed, c, salt);

            // WHICH SPECIES, from a SECOND hash draw off `h` -- never from a counter, never from how
            // many candidates in this chunk (or any other) were accepted before this one. See
            // mixSpecies()/pickSpecies() above.
            const i32 speciesIdx = pickSpecies(resolvedPalette_, d, mixSpecies(h));
            if (speciesIdx < 0) continue;   // no species wanted this candidate's density band
            const ScatterSpecies& sp = resolvedPalette_[static_cast<usize>(speciesIdx)];

            // Same bit range the original generator used for scale (bits [10,17] of h), just
            // remapped into the species' own range instead of a hardcoded 0.75..1.25. For the
            // default species (scaleMin/Max unchanged at 0.75/1.25) this is the SAME expression the
            // generator always used, so default output is bit-for-bit unchanged.
            const f32 raw01 = static_cast<f32>((h >> 10) & 255u);
            const f32 scale = sp.scaleMin + raw01 * ((sp.scaleMax - sp.scaleMin) / 255.0f);

            if (sp.collisionRadiusCm > 0.0f) {
                const f32 r = sp.collisionRadiusCm * scale;
                bool rejected = false;
                for (const PlacedSolid& ps : placedSolid) {
                    const f32 dx = ps.x - lx, dy = ps.y - ly;
                    const f32 minDist = ps.r + r;
                    if (dx * dx + dy * dy < minDist * minDist) { rejected = true; break; }
                }
                if (rejected) continue;   // a bigger neighbour already claimed this spot
                placedSolid.push_back({lx, ly, r});
            }

            PayloadEntity e;
            // NAMED FROM THE SEED AND THE COORD, never from a counter: a counter would renumber every
            // entity in a chunk the moment one candidate above it stopped qualifying, and the names
            // are what a save file's overrides are matched by.
            e.name = "gen_" + std::to_string(c.x) + "_" + std::to_string(c.y) + "_" + std::to_string(salt);
            e.objectId = (static_cast<u64>(h) << 32) | salt;
            e.parent = -1;
            // Chunk-local, which is what a payload stores: in [0, chunkSizeCm) by construction.
            e.local.position = Vec3{lx, ly, 0.0f};
            // A deterministic yaw from the same hash, so the field does not look stamped.
            e.local.rotation = sp.randomizeYaw
                ? Quat::fromAxisAngle({0, 0, 1}, static_cast<f32>(h & 1023u) * 0.006135923f)
                : Quat::identity();
            e.local.scale = Vec3{scale, scale, scale};
            e.hasMesh = true;
            e.mesh = fnv1a64(sp.meshPath.c_str());
            e.material = sp.material;
            e.meshFlags = 1;   // kMeshRendererVisible, without dragging the scene header in here
            p.entities.push_back(std::move(e));
        }
    }
    return p;
}

bool GeneratedChunkSource::has(const ChunkCoord& c) {
    if (c.z != settings_.surfaceChunkZ) return false;   // the cheap answer, for most of the world
    if (!memoValid_ || !(memoCoord_ == c)) {
        memo_ = generate(c);
        memoCoord_ = c;
        memoValid_ = true;
    }
    return !memo_.entities.empty();
}

bool GeneratedChunkSource::load(const ChunkCoord& c, ChunkPayload& out, std::string* why) {
    (void)why;
    if (memoValid_ && memoCoord_ == c) { out = memo_; return true; }
    out = generate(c);
    memo_ = out;
    memoCoord_ = c;
    memoValid_ = true;
    return true;
}

// ---- the layered source ------------------------------------------------------------------------

bool LayeredChunkSource::isOverridden(const ChunkCoord& c) {
    return overrides_ && overrides_->has(c);
}

bool LayeredChunkSource::has(const ChunkCoord& c) {
    if (isOverridden(c)) return true;
    return baseline_ && baseline_->has(c);
}

bool LayeredChunkSource::load(const ChunkCoord& c, ChunkPayload& out, std::string* why) {
    // The override WINS, and is not merged with the baseline. A merge would need a rule for every
    // field of every entity and a way to express "deleted"; storing the whole chunk once it differs
    // is a fraction of the machinery and cannot disagree with itself.
    if (isOverridden(c)) return overrides_->load(c, out, why);
    if (!baseline_) { if (why) *why = "no baseline generator"; return false; }
    return baseline_->load(c, out, why);
}

PersistOutcome persistChunk(RegionWriter& region, GeneratedChunkSource& baseline,
                            const ChunkCoord& c, const ChunkPayload& current, std::string* why) {
    const ChunkLocal l = localOf(c);

    ChunkPayload generated;
    baseline.load(c, generated, nullptr);

    // OVER ENCODED BYTES. They are what a region stores, so they are the only definition of "the
    // same" that cannot drift from what is actually persisted.
    if (encodeChunk(current) == encodeChunk(generated)) {
        // Identical to what generation produces, so it does not need to exist on disk -- and if a
        // stale override is there from an earlier edit that has since been undone, it must GO, or
        // the file keeps a chunk it no longer needs and the save grows monotonically.
        if (region.removeChunk(l, nullptr)) return PersistOutcome::OverrideRemoved;
        return PersistOutcome::Unchanged;
    }
    if (!region.writeChunk(l, current, why)) return PersistOutcome::Failed;
    return PersistOutcome::Stored;
}

} // namespace aver::world

#endif // AVER_MODULE_SCENE
