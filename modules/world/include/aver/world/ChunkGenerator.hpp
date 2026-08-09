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
#  include <functional>
#  include <limits>
#  include <string>
#  include <vector>

namespace aver::world {

// One weighted entry in a scatter palette: what to place, how big, how it randomises, and which
// band of the density field it prefers.
//
// THE DENSITY BAND IS WHAT MAKES THIS AN ECOLOGY RATHER THAN CONFETTI. A candidate already passed
// GeneratorSettings::threshold (it exists at all); the band then decides WHICH species is even
// eligible there -- rock species low in the field, tree species high in it -- before the weighted
// draw picks among whatever is left. A species whose band spans the whole range (the default) is
// eligible everywhere, which is what reproduces today's one-species behaviour exactly.
struct ScatterSpecies {
    std::string meshPath = "Meshes/cube.ocmesh";
    std::string material = "M_Foliage";

    // Relative selection weight among the species eligible for a given candidate. <= 0 means this
    // species can never be picked (a way to disable an entry without removing it).
    f32 weight = 1.0f;

    f32 scaleMin = 0.75f;
    f32 scaleMax = 1.25f;
    bool randomizeYaw = true;

    // Candidates are eligible for this species only when the sampled density d satisfies
    // densityMin <= d <= densityMax. Defaults span every value sampleInfinite can return, so a
    // single default species never excludes a candidate the threshold already accepted.
    f32 densityMin = -std::numeric_limits<f32>::max();
    f32 densityMax = std::numeric_limits<f32>::max();

    // Half-extent in world centimetres used for the in-chunk interpenetration check, BEFORE the
    // per-candidate scale is applied (the check scales it up alongside the candidate). 0 disables
    // the check for this species -- grass and other small/overlap-tolerant fill has no business
    // paying for it or blocking anything else.
    f32 collisionRadiusCm = 0.0f;
};

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

    // Back-compat single-species convenience, used ONLY when `palette` below is empty -- see
    // GeneratedChunkSource::setSettings. A caller (and every existing test) that never touches the
    // palette gets exactly one species built from these two fields, with the palette's own default
    // scale range and yaw, which reproduces the generator's original behaviour bit-for-bit.
    std::string meshPath = "Meshes/cube.ocmesh";
    std::string material = "M_Foliage";

    // The weighted scatter palette. Empty means "one species from meshPath/material above".
    std::vector<ScatterSpecies> palette;

    // OPTIONAL: where a candidate that already exists actually sits vertically. Called at most once
    // per ACCEPTED candidate (one that already cleared `threshold` and got a species from the
    // palette), with that candidate's world (x, y) in centimetres; on true, `outWorldZCm` is the
    // world Z the entity is placed at. False (or this left unset, its default) means "no surface
    // known here", and the candidate falls back to today's flat behaviour for ITSELF ALONE -- every
    // other candidate in the same chunk still asks independently.
    //
    // THE SEAM IS PLACEMENT ONLY. It cannot change which candidates exist, which species one gets, its
    // scale, its yaw, or the interpenetration bookkeeping (ChunkGenerator.cpp's `placedSolid`) -- none
    // of those read Z. Installing or changing a height source only moves accepted entities up or down.
    //
    // UNSET IS THE DEFAULT, AND MUST STAY BYTE-IDENTICAL TO TODAY. An empty std::function is falsy, so
    // every candidate's local Z stays exactly 0.0f, the same literal ChunkGenerator.cpp always wrote --
    // this field costs a caller that never touches it nothing, not even a different code path.
    //
    // MUST BE A PURE FUNCTION OF (worldXCm, worldYCm) for as long as it is installed -- the same
    // contract every other field here already carries (worldSeed, threshold, octaves, ...): generate()
    // is documented as a pure function of (settings, coord), and a height source that reads live,
    // still-changing terrain breaks that guarantee the moment two calls disagree. A loaded, immutable
    // `.ocland` section qualifies; a terrain edit mid-stroke does not, and must be finalised first.
    //
    // SET BEFORE ChunkWorld::open(), NOT AFTER. Unlike RestoreOptions::bindMaterial/::createBody --
    // which the streamer owns and a caller assigns once open() has returned -- GeneratorSettings is
    // value-copied into GeneratedChunkSource by setSettings(), which ChunkWorld::open() calls once, at
    // open time. Assigning this field on a ChunkWorldSettings already passed to open() has no effect:
    // it never reaches the copy generate() reads.
    //
    // NEVER LINKS Aver.Landscape. This module takes no dependency on it and never will -- a host that
    // wants terrain-backed placement links Aver.Landscape itself and closes a lambda over it, exactly
    // as RestoreOptions::bindMaterial closes over a host's own material system rather than this module
    // depending on Aver.Render.PBR.
    std::function<bool(f32 worldXCm, f32 worldYCm, f32& outWorldZCm)> heightSource;
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
    // Resolved once in setSettings(), never touched inside generate(): either settings_.palette
    // verbatim, or the single meshPath/material species when the palette was left empty. Fixed for
    // the lifetime of a settings assignment, so generate() stays a pure function of (coord, salt).
    std::vector<ScatterSpecies> resolvedPalette_;
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
