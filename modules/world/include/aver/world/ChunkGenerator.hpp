// Generating a chunk's contents from nothing but a seed and a coordinate.
//
// THE POINT IS NOT THE CONTENT, IT IS THE PROPERTY. A chunk that is a pure function of
// (worldSeed, chunkCoord) never has to be stored: it can be thrown away on eviction and rebuilt
// identically later. So a save file holds only what the player CHANGED, and an unexplored world
// costs nothing on disk however large it is. That is what makes "generate more as we go" affordable,
// and it is worthless the moment generation stops being reproducible.
//
// CPU, NOT GPU, AND THAT IS NOT A PERFORMANCE CHOICE. The GPU and CPU halves of the PCG mirror agree
// to ONE ULP, not bit-exactly, because DXC fuses multiply-add where MSVC under /fp:precise does not
// (PcgShaders.hpp:8-12). One ULP is invisible in a cloud and fatal here: a density landing either
// side of a threshold places an entity in one process and not the other, and the two disagree about
// what the world contains. The GPU volume stays for VISUAL fields; placement is decided here.
//
// Three further things keep the arithmetic reproducible rather than merely deterministic-looking:
//   * the lattice hash is splitmix32 over INTEGER cell coordinates, so the noise itself has no
//     floating-point state at all -- tests/pcg's PcgMirrorTest is what guards that;
//   * coverageBias is pinned to 1 and coverageFloor to 0, which makes sampleInfinite's final
//     `pow(remapped, bias)` the identity. std::pow is the one operation in that function whose
//     result is not pinned by IEEE 754 across libm implementations, and this removes it rather than
//     hoping two platforms agree;
//   * every position fed to the sampler is derived from integer chunk coordinates by multiply and
//     add only.
//
// FIRST CALLER AND FIRST TEST OF pcg::sampleInfinite. It had neither: tests/pcg exercises only the
// bounded sampler and the PCGVOLUME round trip, and the infinite spec was built at level load and
// never evaluated. docs/CHUNKS.md section 7 flags exactly that.
#pragma once

#include "aver/core/Types.hpp"
#include "aver/pcg/PcgVolume.hpp"
#include "aver/world/ChunkCoord.hpp"
#include "aver/world/ChunkSource.hpp"

#if AVER_MODULE_SCENE
#  include <string>

namespace aver::world {

struct GeneratorSettings {
    u64 worldSeed = 0;
    i32 chunkSizeCm = kDefaultChunkSizeCm;

    // Bumped whenever the RULES change. Recorded in the region index, so a save made by an older
    // generator is detectable instead of being silently mixed with content from a newer one --
    // docs/CHUNKS.md section 6, job 4.
    u32 generatorVersion = 1;

    // Candidate positions per horizontal axis inside one chunk.
    u32 samplesPerAxis = 4;
    // Density above which a candidate becomes an entity.
    f32 threshold = 0.62f;
    // Feature size of the noise's FIRST octave, in world centimetres.
    //
    // AT OR BELOW THE CHUNK SIZE, and the first draft got this wrong in a way worth recording. At
    // 6400 against a 1600 cm chunk, every candidate in a chunk falls inside ONE noise cell -- the
    // sampler floors world position by cell size -- so all 25 hashed to the same value and every
    // chunk came out solid or empty. 75 entities in exactly 3 of 6 chunks, 25 each, which is what
    // the test now refuses to accept. One chunk per cell makes the later octaves the only source of
    // within-chunk variation, and they carry a third of the amplitude between them.
    f32 featureSizeCm = 1600.0f;
    u32 octaves = 3;

    // Only chunks on this Z layer generate anything. A surface world, so the vertical axis is empty
    // by default and `has()` can answer instantly for the overwhelming majority of coordinates.
    i32 surfaceChunkZ = 0;

    std::string meshPath = "Meshes/cube.ocmesh";
    std::string material = "M_Foliage";
};

// A source that invents its chunks. Never fails and never touches a file.
class GeneratedChunkSource final : public IChunkSource {
public:
    explicit GeneratedChunkSource(const GeneratorSettings& s = {}) { setSettings(s); }
    void setSettings(const GeneratorSettings& s);
    const GeneratorSettings& settings() const { return settings_; }

    bool has(const ChunkCoord& c) override;
    bool load(const ChunkCoord& c, ChunkPayload& out, std::string* why) override;

    // The pure function at the centre of it: same settings and coord, same payload, always.
    ChunkPayload generate(const ChunkCoord& c) const;

private:
    GeneratorSettings settings_;
    pcg::InfiniteSpec spec_{};
    // has() is almost always followed by load() for the same chunk, and generating twice would
    // double the cost of every streamed chunk for nothing.
    mutable ChunkCoord memoCoord_{0, 0, 0};
    mutable bool memoValid_ = false;
    mutable ChunkPayload memo_;
};

// Generated content underneath, persisted overrides on top.
//
// A chunk the player has changed is stored and wins; everything else is invented on demand. That is
// the whole delta scheme, and it is why an unexplored world occupies no disk at all.
class LayeredChunkSource final : public IChunkSource {
public:
    // Neither pointer is owned. `overrides` may be null, which means "nothing has been saved yet".
    LayeredChunkSource(IChunkSource* baseline, IChunkSource* overrides)
        : baseline_(baseline), overrides_(overrides) {}

    bool has(const ChunkCoord& c) override;
    bool load(const ChunkCoord& c, ChunkPayload& out, std::string* why) override;

    // Whether `c` is currently served by a stored override rather than by generation.
    bool isOverridden(const ChunkCoord& c);

private:
    IChunkSource* baseline_;
    IChunkSource* overrides_;
};

enum class PersistOutcome {
    Unchanged,        // identical to what generation produces -- nothing was written
    Stored,           // differs, and was written as an override
    OverrideRemoved,  // matched generation again, so the stale override was deleted
    Failed,
};

// Decides whether a chunk needs to exist on disk at all, and acts on it.
//
// THE COMPARISON IS OVER ENCODED BYTES, not field by field. The bytes are what a region stores, so
// they are the only definition of "the same" that cannot drift from what is actually persisted -- a
// field-wise comparison that forgets a member silently starts storing chunks it does not need to.
PersistOutcome persistChunk(RegionWriter& region, GeneratedChunkSource& baseline,
                            const ChunkCoord& c, const ChunkPayload& current, std::string* why = nullptr);

} // namespace aver::world

#endif // AVER_MODULE_SCENE
