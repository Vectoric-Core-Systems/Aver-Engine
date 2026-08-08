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

} // namespace

void GeneratedChunkSource::setSettings(const GeneratorSettings& s) {
    settings_ = s;
    if (!chunkSizeValid(settings_.chunkSizeCm)) settings_.chunkSizeCm = kDefaultChunkSizeCm;
    if (settings_.samplesPerAxis == 0) settings_.samplesPerAxis = 1;
    if (settings_.octaves == 0) settings_.octaves = 1;
    memoValid_ = false;

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
            e.local.rotation = Quat::fromAxisAngle({0, 0, 1}, static_cast<f32>(h & 1023u) * 0.006135923f);
            const f32 scale = 0.75f + static_cast<f32>((h >> 10) & 255u) * (0.5f / 255.0f);
            e.local.scale = Vec3{scale, scale, scale};
            e.hasMesh = true;
            e.mesh = fnv1a64(settings_.meshPath.c_str());
            e.material = settings_.material;
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
