// Implementation of aver::fluids::applyUnderwaterFog. Kept in its own .cpp rather than made
// header-only -- the function has a natural single home (it compiles once into Aver.Fluids, not
// once per translation unit that happens to include the header), matching how the rest of this
// codebase treats a .cpp as the default and header-only as the exception for things that must be
// inlined (small accessors, templates) rather than the norm.
#include "aver/fluids/Underwater.hpp"

namespace aver::fluids {
namespace {

// Linear interpolation, spelled out locally rather than pulled from <algorithm>/Math.hpp: this file
// intentionally has no dependency beyond Types.hpp and RHI.hpp (see Underwater.hpp's header
// comment), and a three-line lerp is not worth widening that on its own.
f32 lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

}   // namespace

rhi::SkyAtmosphere applyUnderwaterFog(const rhi::SkyAtmosphere& authored,
                                       f32 cameraZCm,
                                       f32 waterLevelCm,
                                       const UnderwaterFogTuning& tuning) {
    // Above the fade band: unmodified. Returning `authored` by value here (rather than falling
    // through to a t=0 blend) is the identity guarantee UnderwaterFogTest's first case checks --
    // every field, not just the six fog ones, must come back byte-for-byte unchanged, which a
    // field-by-field lerp at t=0 would only accidentally guarantee for fields it touches at all.
    if (cameraZCm >= waterLevelCm + tuning.fadeBandCm)
        return authored;

    // Below the fade band: full override. Same reasoning in reverse -- returning a freshly built
    // copy rather than a t=1 blend keeps this branch an exact, obviously-correct statement of "the
    // tuning values, in full" rather than a lerp that merely converges to them at the boundary.
    if (cameraZCm <= waterLevelCm - tuning.fadeBandCm) {
        rhi::SkyAtmosphere result = authored;
        result.fogColor[0] = tuning.color[0];
        result.fogColor[1] = tuning.color[1];
        result.fogColor[2] = tuning.color[2];
        result.fogDensity    = tuning.densityPerCm;
        result.fogHeight     = waterLevelCm;   // see Underwater.hpp: anchored here, not tuned
        result.fogFalloff    = tuning.falloff;
        result.fogStart      = tuning.startCm;
        result.fogMaxOpacity = tuning.maxOpacity;
        return result;
    }

    // Inside the band: blend by how far cameraZCm has crossed from the top of the band (t=0, pure
    // authored) to the bottom (t=1, pure tuning). This is the ONLY reason fadeBandCm exists at all
    // -- without it, crossing the surface plane would swap every fog field in a single frame, which
    // reads as a visible pop rather than as entering water. At cameraZCm == waterLevelCm this
    // evaluates to exactly 0.5, which is what UnderwaterFogTest's third case checks: the blend is
    // centered on the surface, not offset toward either side.
    const f32 t = (waterLevelCm + tuning.fadeBandCm - cameraZCm) / (2.0f * tuning.fadeBandCm);

    rhi::SkyAtmosphere result = authored;
    result.fogColor[0] = lerp(authored.fogColor[0], tuning.color[0], t);
    result.fogColor[1] = lerp(authored.fogColor[1], tuning.color[1], t);
    result.fogColor[2] = lerp(authored.fogColor[2], tuning.color[2], t);
    result.fogDensity    = lerp(authored.fogDensity,    tuning.densityPerCm, t);
    result.fogHeight     = lerp(authored.fogHeight,     waterLevelCm,        t);
    result.fogFalloff    = lerp(authored.fogFalloff,    tuning.falloff,      t);
    result.fogStart      = lerp(authored.fogStart,      tuning.startCm,      t);
    result.fogMaxOpacity = lerp(authored.fogMaxOpacity, tuning.maxOpacity,   t);
    return result;
}

}   // namespace aver::fluids
