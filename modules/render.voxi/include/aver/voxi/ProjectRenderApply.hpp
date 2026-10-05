// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
#pragma once
#include "aver/voxi/Voxi.hpp"
#include "aver/voxi/QualityLadder.hpp"
#include "aver/voxi/RenderSettingsResolver.hpp"
#include "aver/voxi/Scalability.hpp"
#include "aver/formats/OcProject.hpp"

// THE ONE PLACE A .ocproject's RENDER.* KEYS BECOME A voxi::Settings, SHARED BY BOTH HOSTS. Before
// this header existed, the editor's applyProjectVoxiSettings (sandbox/src/SandboxApp.cpp) and the
// game's applyProjectRenderSettings (Runtime/src/GameApp.cpp) each carried their own
// copy of the same tier/knob apply logic, and the game's copy was the one that had drifted: it never
// seeded the volume before init (N2), never derived an absent knob back to its tier on reopen (N6),
// and applied everything in ONE setSettings call instead of two (so a manifest naming both a tier and
// a knob at that tier's OWN rung raced the derivation and could lose the knob to a coincidence -- R2's
// own regression test is built on exactly that race). This header is now the only place either fact is
// encoded; SandboxApp.cpp's applyProjectVoxiSettings becomes a thin two-phase caller of it (Lane 4),
// the same as GameApp's two call sites below.
//
// AND THE SAME FOR THE COMMAND LINE (N7). The flag half used to be twenty `*Override_` members read
// one at a time inside SandboxProject.cpp's applyProjectRenderSettings; it is now RenderCliOverrides
// plus applyCliTiers/applyCliKnobs/applyCliOverrides below, carrying its own two-phase rule for the
// same reason the manifest half carries one. THE ARGV PARSING DID NOT MOVE and must not: the editor's
// flag table still owns which spelling fills which field.
//
// PURE AND HEADER-ONLY, RenderSettingsResolver.hpp's exact reason: applyManifestTiers and
// applyManifestKnobs take and mutate a Settings& directly and touch nothing else -- no
// voxi::Renderer::get(), no AVER_WARN, no device. That is what makes the two-phase apply, the N6 knob
// reset and the capture rule each a headless unit test (tests/formats/src/ProjectRenderApplyTest.cpp)
// rather than something only exercisable through a live singleton and a real project on disk. The CLI
// apply keeps that property by taking its LOG SINK from the caller (CliOverrideNote): the host owns
// the sentence and its "[Sandbox]"/"[Game]" tag, this header owns the decision that there is one to
// print -- which is also what lets a test assert the precedence WITHOUT reading a log file.
namespace aver::voxi {

// ---- APPLY: manifest -> Settings ------------------------------------------------------------------

// Applies the manifest's four TIER keys (GI, RT, PT, LayeredBsdf) to `s`, and nothing else. Split out
// from applyManifestKnobs for the same reason SandboxApp.cpp's own two-call apply is split: a knob
// asked for at the value it already holds is indistinguishable, to setSettings' own change-gated
// derivation, from a knob never asked for at all -- so the tiers must already be COMMITTED (see
// applyManifestTwoPhase below) before any knob is read against them, or a manifest naming both a tier
// and a same-rung knob races the derivation and can lose the knob (this is exactly what R2's
// regression test in ProjectRenderApplyTest.cpp exists to pin down).
//
// Logic copied from SandboxApp.cpp's applyProjectVoxiSettings, first setSettings call: same four
// fields, same "-1 means unstated" sentinel, same order.
inline void applyManifestTiers(const fmt::ProjectDesc& project, Settings& s) {
    if (project.giQuality   >= 0) s.globalIllumination = static_cast<Quality>(project.giQuality);
    if (project.rayTracing  >= 0) s.rayTracing         = static_cast<Quality>(project.rayTracing);
    if (project.pathTracing >= 0) s.pathTracing        = static_cast<Quality>(project.pathTracing);
    if (project.layeredBsdf >= 0) s.layeredBsdf        = static_cast<Quality>(project.layeredBsdf);
}

// Applies every OTHER manifest key that lands on a Settings field -- everything
// SandboxApp.cpp's applyProjectVoxiSettings applies in its SECOND setSettings call, same order, same
// sentinels, with ONE deliberate change (N6):
//
// THE N6 FIX. Ten of these knobs are TIER-DERIVED: Renderer::setSettings only recomputes one of them
// from its tier when the tier CHANGES in that same call (Voxi.cpp's own derivation block) -- so a
// manifest that states a tier but says nothing about one of its derived knobs used to just inherit
// whatever was ALREADY LIVE, tier change or not. Project A pins GICONES 7 at GI Epic; project B states
// GI Epic too and never mentions GICONES; opening B right after A used to run Epic's cones at 7, not
// the ladder's 13, because the tier never "changed" out from under B's own apply. Here, the ABSENCE of
// one of these ten keys does not mean "leave it" -- it means "follow the tier `s` was just committed
// to", which is what running this function AFTER applyManifestTiers has already been committed through
// setSettings (see applyManifestTwoPhase) is what actually gives it: `s.globalIllumination`,
// `s.rayTracing` and `s.pathTracing` here are the DEVICE-CLAMPED, POST-DERIVATION tiers, not the raw
// manifest ask. The ten: voxelResolution, giCones, giUpdateInterval, giRestirVisibility (GI);
// rtShadowRays, rtPixelsPerRayTile, rtShadowDenoise, rtRenderMode, refractionMode (RT); ptBounces (PT).
//
// SANITY THIS FIX COSTS NOTHING TODAY: Settings{}'s own defaults already equal ladder(Medium) per
// group (QualityLadder.hpp's static_asserts enforce this), and a tier CHANGE already re-derives these
// same values through setSettings regardless of this function -- so a fresh launch, or any project that
// actually changes a tier, sees byte-identical behaviour. This only changes the case that used to leak:
// tier UNCHANGED, knob UNSTATED.
//
// giSkyOcclusionRays/giSkyOcclusionTile have NO manifest key at all (OcProject.hpp never grew one for
// either) -- they are ALWAYS set to the RT tier's ladder value here, every apply, manifest or not.
//
// Every startup knob CLI flag (SandboxApp.cpp's own take() block) still outranks whatever this writes,
// because that block runs AFTER this one and only takes a flag whose value actually differs from what
// setSettings already holds -- this function changes what "already holds" means for an unstated
// derived knob, it does not touch flag precedence at all.
inline void applyManifestKnobs(const fmt::ProjectDesc& project, Settings& s) {
    if (project.voxelResolution > 0) s.voxelResolution = static_cast<u32>(project.voxelResolution);
    else                              s.voxelResolution = ladder::voxelResolution(s.globalIllumination);

    if (project.giIntensity   >= 0.0f) s.giIntensity   = project.giIntensity;
    if (project.giMaxDistance >= 0.0f) s.giMaxDistance = project.giMaxDistance;

    if (project.rtShadowRays >= 0) s.rtShadowRays = static_cast<u32>(project.rtShadowRays);
    else                            s.rtShadowRays = ladder::rtShadowRays(s.rayTracing);

    if (project.rtPixelsPerRayTile >= 0) s.rtPixelsPerRayTile = static_cast<u32>(project.rtPixelsPerRayTile);
    else                                  s.rtPixelsPerRayTile = ladder::rtPixelsPerRayTile(s.rayTracing);

    if (project.rtShadowDenoise >= 0) s.rtShadowDenoise = static_cast<u32>(project.rtShadowDenoise);
    else                               s.rtShadowDenoise = ladder::rtShadowDenoise(s.rayTracing);

    if (project.rtRenderMode >= 0) s.rtRenderMode = static_cast<u32>(project.rtRenderMode);
    else                             s.rtRenderMode = ladder::rtRenderMode(s.rayTracing);

    if (project.ptBounces >= 0) s.ptBounces = static_cast<u32>(project.ptBounces);
    else                          s.ptBounces = ladder::ptBounces(s.pathTracing);

    if (project.giCones >= 0) s.giCones = static_cast<u32>(project.giCones);
    else                        s.giCones = ladder::giCones(s.globalIllumination);

    if (project.giMode   >= 0) s.giMode   = static_cast<u32>(project.giMode);
    if (project.denoiser  >= 0) setDenoiserMode(s, static_cast<u32>(project.denoiser));   // 0, 1, 2 = NRD2
    if (project.neuralDenoise >= 0) s.neuralDenoise = project.neuralDenoise != 0;
    if (project.ptMode    >= 0) s.ptMode   = static_cast<u32>(project.ptMode);

    // RENDER.RESTIRHISTORY rides the same plain-knob shape as giMode/denoiser just above, NOT the
    // tier-derived one below: giRestirMaxHistory has no ladder rung to fall back to (Voxi.hpp's own
    // comment on the field), so an absent key simply leaves the engine default (0) alone -- no N6
    // else-branch, no re-derivation on a tier change.
    if (project.restirHistory >= 0) s.giRestirMaxHistory = static_cast<u32>(project.restirHistory);

    // RENDER.RDSTAGES: the SAME plain-knob shape as RESTIRHISTORY directly above, for the identical
    // reason -- Settings::rayDrivenStages has no ladder rung either, so an absent key leaves the
    // engine default (2, staged + half-rate GI) alone regardless of any rtRenderMode tier change just
    // applied.
    if (project.rdStages >= 0) s.rayDrivenStages = static_cast<u32>(project.rdStages);

    // RENDER.FOGOCCLUSION: the same plain-knob shape again -- no ladder rung, so an absent key keeps
    // the engine default (on).
    if (project.fogOcclusion >= 0) s.fogOcclusion = project.fogOcclusion != 0;

    // THE TENTH TIER-DERIVED KNOB (N6): absent RENDER.RESTIRVISIBILITY follows the GI tier that was
    // just committed above, exactly like voxelResolution/giCones/giUpdateInterval do -- not whatever
    // giRestirVisibility happened to already be live at.
    if (project.restirVisibility >= 0) s.giRestirVisibility = static_cast<u32>(project.restirVisibility);
    else                                 s.giRestirVisibility = ladder::giRestirVisibility(s.globalIllumination);

    if (project.refractionMode >= 0) s.refractionMode = static_cast<u32>(project.refractionMode);
    else                               s.refractionMode = ladder::refraction(s.rayTracing);

    if (project.refractionStrength >= 0.0f) s.refractionStrength = project.refractionStrength;
    if (project.refractionEdgeFade >= 0.0f) s.refractionEdgeFade = project.refractionEdgeFade;

    // THE THREE KEYS THAT ARE NOT TIER-DERIVED but still ride this same second call in
    // SandboxApp.cpp's own applyProjectVoxiSettings (msaa/meshShaders/giUpdateInterval) -- kept here
    // rather than split into applyManifestTiers for the same reason that file states: this is the call
    // that carries every knob a manifest can state, and separating them by derivation would be a
    // distinction only that file's own history explains. giUpdateInterval IS tier-derived, unlike its
    // two neighbours here, and gets the same N6 else-branch as the ten above.
    if (project.msaa        >  0) s.msaa        = static_cast<Msaa>(project.msaa);
    if (project.meshShaders >= 0) s.meshShaders = project.meshShaders != 0;

    if (project.giUpdateInterval >= 0) s.giUpdateInterval = static_cast<u32>(project.giUpdateInterval);
    else                                 s.giUpdateInterval = ladder::giUpdateInterval(s.globalIllumination);

    // NO MANIFEST KEY EXISTS FOR EITHER OF THESE -- unconditional, every apply.
    s.giSkyOcclusionRays = ladder::giSkyOcclusionRays(s.rayTracing);
    s.giSkyOcclusionTile = ladder::giSkyOcclusionTile(s.rayTracing);
}

// THE TWO-PHASE APPLY, shared by both hosts (R2). `s` is the Settings a caller is building up --
// seeded from whatever is already live before this call -- and `commit` is invoked with NO ARGUMENTS
// after each phase; it is expected to push `s` through Renderer::setSettings and read the
// (device-clamped, tier-derived) result back into the SAME `s` the caller passed in, by reference --
// e.g. `voxi::Settings x = vx.settings(); applyManifestTwoPhase(project, x, [&](){ vx.setSettings(x);
// x = vx.settings(); });`. That is the shape both call sites below actually need: `s` and the value the
// lambda reads/writes are one and the same object, so applyManifestTiers' write is visible to `commit`,
// and `commit`'s read-back is visible to applyManifestKnobs.
//
// STRICT ORDER, NEVER MERGED (R2): apply the four tiers, commit them (so setSettings' own
// change-gated derivation runs against the NEW tiers, and `s` comes back holding whatever it derived),
// THEN apply every knob -- now against the post-derivation effective tiers rather than the pre-commit
// ones -- and commit again.
//
// COLLAPSING THIS INTO ONE setSettings CALL IS THE EXACT BUG R2's REGRESSION TEST CATCHES. Live
// settings: GI Medium, giCones 6 (Medium's own ladder rung). A manifest states GI 4 (Epic) and GICONES
// 6 -- explicitly asking for the SAME cone count Medium already had. Merge both edits into `s` before
// ever calling setSettings and the single commit sees globalIllumination go Medium -> Epic (a real
// change) with giCones ARRIVING AT 6 -- which is indistinguishable, to setSettings' own change-gated
// derivation, from "the caller left giCones alone", because 6 also happens to be what was already
// live. The tier changed and the knob "didn't", so the derivation fires and overwrites the caller's
// explicit 6 with ladder(Epic)'s 13 -- silently discarding a value the manifest asked for by name,
// purely because it collided with the OLD tier's own default.
//
// TWO-PHASE AVOIDS IT: phase 1 commits ONLY the tier change with giCones still untouched at 6, so the
// SAME derivation fires -- correctly, this time, since giCones really is unedited at this point -- and
// `s.giCones` comes back 13. Phase 2 then applies the manifest's explicit GICONES 6 on top of that,
// and commits again with the tier already Epic (unchanged in this second commit), so nothing re-derives
// over it. Final result: 6, exactly what the manifest asked for. See ProjectRenderApplyTest.cpp's own
// R2 case, which asserts two-phase gives 6 and records in a comment that a merged single call would
// have given 13.
template <class CommitReadBack>
inline void applyManifestTwoPhase(const fmt::ProjectDesc& project, Settings& s, CommitReadBack&& commit) {
    applyManifestTiers(project, s);
    commit();
    applyManifestKnobs(project, s);
    commit();
}

// ---- APPLY: command line -> Settings ---------------------------------------------------------------

// EVERY RENDER FLAG THAT OUTRANKS A MANIFEST, IN ONE STRUCT. Before this, SandboxProject.cpp's
// applyProjectRenderSettings read twenty separate `*Override_` members of SandboxApp one at a time,
// inline, inside two hand-rolled blocks -- and that function's own comments record FOUR separate
// occasions on which a flag was added and one of those blocks was not updated, each one letting a
// recorded preference silently outrank a human at the keyboard and each one corrupting a measurement
// before anyone noticed (--pt-scene, --no-gi/--no-rt, --gi-update-interval, --msaa). A knob that is a
// field of a struct can be missed the same way, but only once: the struct is the checklist.
//
// THE ARGV PARSING IS NOT HERE AND IS NOT MOVING. The editor's own flag table (SandboxMain.cpp)
// still owns "--rt-rays N" -> rtRaysOverride_; SandboxProject.cpp fills this struct from those
// members at the one call site. What moved is the half worth sharing -- the precedence rule, the
// sentinels, and the two-phase order below.
//
// THE SENTINELS ARE NOT UNIFORM AND MUST NOT BE MADE SO. Three different "absent" values are in use,
// each load-bearing for the flag that carries it, and each default below IS that flag's absent value
// so a default-constructed RenderCliOverrides applies nothing at all:
//   -1    for the tiers and for every knob whose 0 is a legal thing to ask for -- `--gi 0`, `--pt 0`,
//         `--gi-mode 0`, `--rt-render-mode 0`. These had a 0-means-absent sentinel once and could not
//         express Off at all, which is why `--pt 0` had to be fixed before a raster-versus-ray-driven
//         measurement could mean anything.
//    0    for msaa, rtShadowRays, rtPixelsPerRayTile and giUpdateInterval, where 0 is meaningless as a
//         request ("zero samples", "revoxelise every zero frames") and so reads as "flag not given".
//         applyCliKnobs guards exactly these four with `> 0`; take()'s own negative test does NOT
//         cover them, and dropping a guard would write a 0 nobody asked for.
//   -1.0f for giIntensity, the one float.
struct RenderCliOverrides {
    // Phase A (see applyCliTiers): the five flags that move a TIER, and nothing else.
    int  globalIllumination = -1;    // --gi TIER
    int  rayTracing         = -1;    // --rt TIER
    int  pathTracing        = -1;    // --pt TIER
    // BOOLS, not -1-sentinel ints, and that difference is why closing the flag-outranks-manifest rule
    // for integers left these two behind for a third go round. `false` is "flag not given".
    bool giForceOff         = false; // --no-gi
    bool rtForceOff         = false; // --no-rt

    // Phase B (see applyCliKnobs): everything else, in the order the editor has always applied it.
    int rtRenderMode        = -1;    // --rt-render-mode 0|1
    int refractionMode      = -1;    // --refraction MODE
    int rtShadowDenoise     = -1;    // --rt-shadow-denoise RADIUS
    int ptBounces           = -1;    // --pt-bounces N
    int layeredBsdf         = -1;    // --layered-bsdf TIER
    int msaa                =  0;    // --msaa N      (0 = flag not given)
    int rtShadowRays        =  0;    // --rt-rays N   (0 = flag not given)
    int giSkyOcclusionRays  = -1;    // --gi-sky-occlusion-rays N
    int giSkyOcclusionTile  = -1;    // --gi-sky-occlusion-tile N
    f32 giIntensity         = -1.0f; // --gi-intensity F
    int rtPixelsPerRayTile  =  0;    // --rt-pixels-per-ray N   (0 = flag not given)
    int giUpdateInterval    =  0;    // --gi-update-interval N  (0 = flag not given)
    int giMode              = -1;    // --gi-mode 0|1
    int giRestirVisibility  = -1;    // --restir-visibility N
    int denoiser            = -1;    // --denoiser 0|1|2 (2 = NRD2)
    int neuralDenoise       = -1;    // --neural-denoise 0|1
    int rayDrivenStages     = -1;    // --rd-stages 0|1|2
};

// WHICH SENTENCE A WINNING OVERRIDE WANTS PRINTED. The host owns the sentence, this header owns the
// decision -- the same division GameRender.hpp's DrawWorldSurfaceWarnFn draws, and for the same
// reason here: the editor prints these under "[Sandbox]" and the shipped game would print "[Game]",
// so a tag baked in at this level is wrong for one of the two. Three kinds because the editor prints
// three genuinely different sentences today and collapsing the apply is not licensed to reword any of
// them.
enum class CliOverrideKind : u32 {
    Take,        // an integer flag replaced a value: `flag`, `asked`, `found`
    ForceOff,    // --no-gi/--no-rt drove a tier to Off: `flag`, `found` (the tier it displaced)
    GiIntensity, // --gi-intensity replaced a float: `flag`, `askedF`, `foundF`
};

// One note per override that actually won. Each kind reads only the fields its comment above names;
// the rest stay at their defaults, which is why they have defaults at all.
struct CliOverrideNote {
    CliOverrideKind kind = CliOverrideKind::Take;
    const char* flag = "";
    int  asked  = 0;
    u32  found  = 0;
    f32  askedF = 0.0f;
    f32  foundF = 0.0f;
};

namespace detail {
inline CliOverrideNote intNote(CliOverrideKind kind, const char* flag, int asked, u32 found) {
    CliOverrideNote n;
    n.kind = kind;
    n.flag = flag;
    n.asked = asked;
    n.found = found;
    return n;
}

inline CliOverrideNote floatNote(const char* flag, f32 asked, f32 found) {
    CliOverrideNote n;
    n.kind = CliOverrideKind::GiIntensity;
    n.flag = flag;
    n.askedF = asked;
    n.foundF = found;
    return n;
}
} // namespace detail

// PHASE A (N7): THE TIER FLAGS AND THE FORCE-OFFS, AND NOTHING ELSE. Returns true when it actually
// changed `s` -- the caller's signal to commit, and the reason this returns a bool rather than
// committing itself (see applyCliOverrides).
//
// THE BUG THIS SPLIT FIXES. A tier flag (--gi/--rt/--pt) and a knob flag for THAT SAME tier (e.g.
// --rt-rays) used to land in one merged setSettings call. take()'s own "ignore an override already
// equal to the live value" rule reads the knob against the OLD tier's live value -- so an explicit
// --rt-rays 4 that happened to equal the OLD tier's own rtShadowRays looked like a no-op to take(),
// was never marked overridden, and rode into that single setSettings() call still at its pre-flag
// value. setSettings' own tier-derivation then saw the tier change AND an "untouched" knob in the
// SAME call and rederived the knob to the NEW tier's ladder rung, silently discarding the 4 the flag
// asked for: PTTest (RENDER.RAYTRACING 4, RENDER.RTSHADOWRAYS 4) opened with `--rt 1 --rt-rays 4`
// lost the explicit 4 to Low's derived 1, with nothing logged, purely because the coincidence made
// the flag look unchanged. It is the same race applyManifestTwoPhase's own comment describes for a
// manifest, arriving through a different door.
//
// Committing the tiers ALONE means Phase B reads settings that already carry the NEW tier's derived
// knobs, so its take() calls compare each flag against those -- the coincidence can no longer hide an
// override.
template <class Log>
inline bool applyCliTiers(const RenderCliOverrides& cli, Settings& s, Log&& log) {
    bool overridden = false;
    const auto take = [&](int ov, u32& dst, const char* name) {
        if (ov < 0 || static_cast<u32>(ov) == dst) return;
        log(detail::intNote(CliOverrideKind::Take, name, ov, dst));
        dst = static_cast<u32>(ov);
        overridden = true;
    };
    take(cli.globalIllumination, reinterpret_cast<u32&>(s.globalIllumination), "--gi");
    take(cli.rayTracing,         reinterpret_cast<u32&>(s.rayTracing),         "--rt");
    take(cli.pathTracing,        reinterpret_cast<u32&>(s.pathTracing),        "--pt");

    // forceOff CHANGES A TIER exactly as take(cli.globalIllumination, ...) does, which is why it sits
    // in this phase rather than with the knobs: it has to be committed before any knob flag is
    // compared against what follows from it. Measured cost of getting this wrong: on
    // RENDER.RAYTRACING 4, `--no-rt` was silently discarded and an A/B built on it said ray tracing
    // cost -0.3ms (appeared FASTER to turn on) when the real answer was 6.7ms.
    const auto forceOff = [&](bool want, Quality& dst, const char* name) {
        if (!want || dst == Quality::Off) return;
        log(detail::intNote(CliOverrideKind::ForceOff, name, 0, static_cast<u32>(dst)));
        dst = Quality::Off;
        overridden = true;
    };
    forceOff(cli.giForceOff, s.globalIllumination, "--no-gi");
    forceOff(cli.rtForceOff, s.rayTracing,         "--no-rt");
    return overridden;
}

// PHASE B (N7): EVERY KNOB FLAG, AGAINST THE POST-TIER SETTINGS. Returns true when it changed `s`,
// same contract as Phase A.
//
// THIS PHASE TRULY HAS NO TIER CHANGE IN IT -- nothing below touches s.globalIllumination,
// s.rayTracing or s.pathTracing, so setSettings' own change-gated derivation sees no tier change on
// the commit that follows and leaves every knob written here exactly as written. That is a property
// of the code below, not a wish: an earlier version of this claim was made about the single merged
// call that also carried the tier takes, and was simply wrong.
template <class Log>
inline bool applyCliKnobs(const RenderCliOverrides& cli, Settings& s, Log&& log) {
    bool overridden = false;
    const auto take = [&](int ov, u32& dst, const char* name) {
        if (ov < 0 || static_cast<u32>(ov) == dst) return;
        log(detail::intNote(CliOverrideKind::Take, name, ov, dst));
        dst = static_cast<u32>(ov);
        overridden = true;
    };
    take(cli.rtRenderMode,    s.rtRenderMode,    "--rt-render-mode");
    take(cli.rayDrivenStages, s.rayDrivenStages, "--rd-stages");
    take(cli.refractionMode,  s.refractionMode,  "--refraction");
    take(cli.rtShadowDenoise, s.rtShadowDenoise, "--rt-shadow-denoise");
    take(cli.ptBounces,       s.ptBounces,       "--pt-bounces");
    take(cli.layeredBsdf,     reinterpret_cast<u32&>(s.layeredBsdf), "--layered-bsdf");
    // THE FOUR 0-MEANS-ABSENT FLAGS keep their own `> 0` guard -- see RenderCliOverrides' sentinel
    // note. take() treats only a NEGATIVE as absent, so without these a default 0 would be applied as
    // an ask for zero samples, zero rays, a zero-pixel tile or a zero-frame GI interval.
    if (cli.msaa > 0)               take(cli.msaa,         reinterpret_cast<u32&>(s.msaa), "--msaa");
    if (cli.rtShadowRays > 0)       take(cli.rtShadowRays, s.rtShadowRays,                 "--rt-rays");
    take(cli.giSkyOcclusionRays, s.giSkyOcclusionRays, "--gi-sky-occlusion-rays");
    take(cli.giSkyOcclusionTile, s.giSkyOcclusionTile, "--gi-sky-occlusion-tile");

    // --gi-intensity IS NOT A take(): it is the one float here, and it writes and marks `overridden`
    // even when the value it writes already matches, while only LOGGING on a real difference. That
    // asymmetry is deliberate and is kept verbatim. Marking `overridden` is the load-bearing half:
    // written without it -- which is exactly what happened, in the commit that added the flag,
    // directly underneath four paragraphs warning about this class of bug -- the whole override
    // becomes a no-op whenever no OTHER flag happens to be present, and it fails in the shape this
    // header exists to prevent: the log says the flag outranked the manifest, and nothing changes.
    // Caught only by measurement: --gi-intensity 0, 2 and 4 produced three byte-identical images.
    if (cli.giIntensity >= 0.0f) {
        if (s.giIntensity != cli.giIntensity)
            log(detail::floatNote("--gi-intensity", cli.giIntensity, s.giIntensity));
        s.giIntensity = cli.giIntensity;
        overridden = true;
    }

    if (cli.rtPixelsPerRayTile > 0) take(cli.rtPixelsPerRayTile, s.rtPixelsPerRayTile, "--rt-pixels-per-ray");
    if (cli.giUpdateInterval > 0)   take(cli.giUpdateInterval,   s.giUpdateInterval,   "--gi-update-interval");
    take(cli.giMode,             s.giMode,             "--gi-mode");
    take(cli.giRestirVisibility, s.giRestirVisibility, "--restir-visibility");

    // The denoiser is a bool plus a kind (Voxi.hpp), so the flag is staged through denoiserMode()'s
    // 0/1/2 and assigned back. The whole point of this pass is that RENDER.DENOISER must not outrank a
    // human who just typed --denoiser.
    if (cli.denoiser >= 0) {
        u32 den = denoiserMode(s);
        take(cli.denoiser > 2 ? 2 : cli.denoiser, den, "--denoiser");
        setDenoiserMode(s, den);
    }
    if (cli.neuralDenoise >= 0) {
        u32 nd = s.neuralDenoise ? 1u : 0u;
        take(cli.neuralDenoise != 0 ? 1 : 0, nd, "--neural-denoise");
        s.neuralDenoise = nd != 0;
    }
    return overridden;
}

// THE TWO-PHASE CLI APPLY (N7), the flag-side twin of applyManifestTwoPhase above and subject to the
// identical rule: PHASE A ALONE, COMMITTED, THEN PHASE B AGAINST THE RESULT. `s` and the Settings the
// `commit` lambda pushes and reads back must be the SAME object, exactly as applyManifestTwoPhase
// needs -- e.g. `voxi::Settings k = vx.settings(); applyCliOverrides(cli, k, log, [&](){
// vx.setSettings(k); k = vx.settings(); });`.
//
// MERGING THE TWO PHASES INTO ONE COMMIT IS THE BUG, not a tidy-up: see applyCliTiers' own comment
// for the --rt 1 --rt-rays 4 case it silently loses, and ProjectRenderApplyTest.cpp's
// testN7CliPhaseOrderThroughTheSingleton, which fails if they are merged.
//
// EACH COMMIT IS GATED ON ITS PHASE HAVING ACTUALLY CHANGED SOMETHING, which is what the phases'
// bool returns are for. setSettings is not a free no-op -- it re-clamps against the device and can
// re-log a refusal -- so a run with no flags at all must reach it zero times, the way this apply has
// always behaved. A phase that returns false has provably left `s` untouched: every write to `s`
// above is paired with `overridden = true`, so skipping the commit leaves `s` still equal to what the
// caller read out of the renderer, which is precisely what the next phase needs to read.
template <class Log, class CommitReadBack>
inline void applyCliOverrides(const RenderCliOverrides& cli, Settings& s, Log&& log,
                               CommitReadBack&& commit) {
    if (applyCliTiers(cli, s, log)) commit();
    if (applyCliKnobs(cli, s, log)) commit();
}

// ---- CAPTURE: Settings -> manifest -----------------------------------------------------------------

// Applies the capture rule for ONE of the ten keyed derived knobs to one ProjectDesc field.
// `group` is which Scalability group this knob belongs to, for the overallFollowMask bit test and for
// reading each side's TIER through groupTier (Scalability.hpp) -- the same tier accessor
// groupFollowsLadder and applyOverall already use, so "the group's tier" means the same thing
// everywhere this codebase asks the question.
namespace detail {
inline void captureKnob(int& manifestField, u32 requestedField, u32 liveField,
                         ScalabilityGroup group, const Settings& requested, const Settings& live,
                         u32 overallFollowMask) {
    // 1. An Overall preset just wrote this group -- the manifest goes back to "follow the tier".
    if ((overallFollowMask & (1u << static_cast<u32>(group))) != 0) { manifestField = -1; return; }
    // 2. The group's TIER changed and this specific knob did NOT -- the same test setSettings' own
    //    derivation uses to decide whether to recompute a knob from its tier. If the user only moved
    //    the tier slider and left this knob alone, let it keep following whatever tier comes next.
    if (groupTier(requested, group) != groupTier(live, group) && requestedField == liveField) {
        manifestField = -1;
        return;
    }
    // 3. An explicit edit -- captured as a pin, verbatim.
    if (requestedField != liveField) { manifestField = static_cast<int>(requestedField); return; }
    // 4. Neither of the above: leave the manifest value exactly as it already was (-1 stays -1; an
    //    existing pin stays pinned). Nothing to do.
}

// Applies the capture rule for one of the five DEVICE-CLAMPED fields (the three tier enums,
// meshShaders, msaa) to one ProjectDesc field. `deviceCanRun` answers "could THIS device even produce
// a `requested` different from `live` here" -- when it could not (N5), an unrelated edit on a machine
// without the hardware must not be the thing that overwrites a teammate's pin for hardware it does
// have.
inline void captureClamped(int& manifestField, u32 requestedField, u32 liveField, bool deviceCanRun) {
    // 1. An actual difference -- write it, regardless of whether this device could have produced it
    //    itself (a value already loaded from a manifest a capable machine wrote counts here).
    if (requestedField != liveField) { manifestField = static_cast<int>(requestedField); return; }
    // 2. requested == live, and this device could never have made them differ -- leave the manifest
    //    alone (N5): the equality tells us nothing about what the USER wants, only that Renderer::
    //    setSettings already clamped this field for us, silently, on every apply.
    if (!deviceCanRun) return;
    // 3. requested == live, and this device COULD run it -- today's existing behaviour: write it
    //    through anyway (a no-op byte-for-byte, since it already equals liveField).
    manifestField = static_cast<int>(requestedField);
}
} // namespace detail

// Writes every Voxi-owned RENDER.* field of `project` from `requested` (the Settings about to be
// applied -- e.g. buildRenderingSettings' local `s`, before vx.setSettings(s) runs) against `live`
// (voxi::Renderer::get().settings(), the settings from BEFORE this edit -- this call must run before
// the commit, so "live" means what it says). `overallFollowMask` is whatever applyOverall (or an
// Overall button that just called it) returned this same edit, 0 when no Overall preset ran.
//
// THE TEN KEYED DERIVED KNOBS use captureKnob's four-step rule above: GI's voxelResolution/giCones/
// giUpdateInterval/giRestirVisibility, RT's rtShadowRays/rtPixelsPerRayTile/rtShadowDenoise/
// rtRenderMode/refractionMode, PT's ptBounces. giSkyOcclusionRays/giSkyOcclusionTile have NO manifest
// key and are never captured -- there is nothing in `project` to write them into.
//
// THE FIVE DEVICE-CLAMPED FIELDS (the three tier enums, meshShaders, msaa) use captureClamped's rule.
//
// EVERYTHING ELSE keeps today's unconditional write-requested behaviour: giIntensity, giMaxDistance,
// layeredBsdf, giMode, denoiser, restirHistory, refractionStrength, refractionEdgeFade. giMode and
// denoiser now round-trip the stored REQUEST rather than a clamped value -- Lane 1 removed setSettings'
// own giMode clamp (Voxi.cpp), and denoiser was never clamped to begin with, so `requested.giMode`/
// `requested.denoiser` already ARE the honest ask in both cases. restirHistory joins them for the
// identical reason: it is never clamped against a tier or a device, only range-clamped in Voxi.cpp's
// setSettings, so requested == the honest ask always and there is no tier-change case to distinguish
// from an explicit edit the way captureKnob's four-step rule exists for.
inline void captureVoxiSettings(fmt::ProjectDesc& project, const Settings& requested,
                                 const Settings& live, const DeviceInfo& d, u32 overallFollowMask) {
    using detail::captureKnob;
    using detail::captureClamped;

    captureKnob(project.voxelResolution, requested.voxelResolution, live.voxelResolution,
                ScalabilityGroup::GlobalIllumination, requested, live, overallFollowMask);
    captureKnob(project.giCones, requested.giCones, live.giCones,
                ScalabilityGroup::GlobalIllumination, requested, live, overallFollowMask);
    captureKnob(project.giUpdateInterval, requested.giUpdateInterval, live.giUpdateInterval,
                ScalabilityGroup::GlobalIllumination, requested, live, overallFollowMask);
    captureKnob(project.restirVisibility, requested.giRestirVisibility, live.giRestirVisibility,
                ScalabilityGroup::GlobalIllumination, requested, live, overallFollowMask);

    captureKnob(project.rtShadowRays, requested.rtShadowRays, live.rtShadowRays,
                ScalabilityGroup::RayTracing, requested, live, overallFollowMask);
    captureKnob(project.rtPixelsPerRayTile, requested.rtPixelsPerRayTile, live.rtPixelsPerRayTile,
                ScalabilityGroup::RayTracing, requested, live, overallFollowMask);
    captureKnob(project.rtShadowDenoise, requested.rtShadowDenoise, live.rtShadowDenoise,
                ScalabilityGroup::RayTracing, requested, live, overallFollowMask);
    captureKnob(project.rtRenderMode, requested.rtRenderMode, live.rtRenderMode,
                ScalabilityGroup::RayTracing, requested, live, overallFollowMask);
    captureKnob(project.refractionMode, requested.refractionMode, live.refractionMode,
                ScalabilityGroup::RayTracing, requested, live, overallFollowMask);

    captureKnob(project.ptBounces, requested.ptBounces, live.ptBounces,
                ScalabilityGroup::PathTracing, requested, live, overallFollowMask);

    captureClamped(project.giQuality, static_cast<u32>(requested.globalIllumination),
                   static_cast<u32>(live.globalIllumination),
                   featureStatus(Feature::GlobalIllumination, d) == Status::Ready);
    captureClamped(project.rayTracing, static_cast<u32>(requested.rayTracing),
                   static_cast<u32>(live.rayTracing),
                   featureStatus(Feature::RayTracing, d) == Status::Ready);
    captureClamped(project.pathTracing, static_cast<u32>(requested.pathTracing),
                   static_cast<u32>(live.pathTracing),
                   featureStatus(Feature::PathTracing, d) == Status::Ready);
    captureClamped(project.meshShaders, requested.meshShaders ? 1u : 0u, live.meshShaders ? 1u : 0u,
                   featureStatus(Feature::MeshShaders, d) == Status::Ready);
    // "a stated count not in msaaMask" -- DeviceInfo::msaaMask is tested directly against the sample
    // COUNT (bit N set means N samples are supported: `device_.msaaMask & samples`, Voxi.cpp's own
    // MSAA clamp), never against 1u << count.
    captureClamped(project.msaa, static_cast<u32>(requested.msaa), static_cast<u32>(live.msaa),
                   (d.msaaMask & static_cast<u32>(requested.msaa)) != 0);

    project.giIntensity        = requested.giIntensity;
    project.giMaxDistance      = requested.giMaxDistance;
    project.layeredBsdf        = static_cast<int>(requested.layeredBsdf);
    project.giMode             = static_cast<int>(requested.giMode);
    project.denoiser           = static_cast<int>(denoiserMode(requested));
    project.neuralDenoise      = requested.neuralDenoise ? 1 : 0;
    project.ptMode             = static_cast<int>(requested.ptMode);
    project.restirHistory     = static_cast<int>(requested.giRestirMaxHistory);
    // RENDER.RDSTAGES: unconditional capture, same "EVERYTHING ELSE" rule as RESTIRHISTORY directly
    // above (giMode/denoiser's shape), never captureKnob's four-branch tier-aware rule -- there is no
    // ladder rung for a tier change to race against.
    project.rdStages           = static_cast<int>(requested.rayDrivenStages);
    // RENDER.FOGOCCLUSION: unconditional capture, denoiser's shape (a bool, no tier to race).
    project.fogOcclusion       = requested.fogOcclusion ? 1 : 0;
    project.refractionStrength = requested.refractionStrength;
    project.refractionEdgeFade = requested.refractionEdgeFade;
}

} // namespace aver::voxi
