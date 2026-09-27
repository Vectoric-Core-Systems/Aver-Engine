#pragma once
// A fluid volume the water module offers, simulated by the engine's Jolt-backed soft-body solver --
// without this module ever learning Jolt exists (README.md's buoyancy row: "this module must never
// learn what Jolt is"; the two halves "meet in the composition root and nowhere else"). This is the
// water-module half; modules/physics/include/aver/physics/physics_abi.h's aver_phys_softbody_* block
// is the physics half. Nothing below includes that header, names a Jolt type, or calls an
// aver_phys_ symbol.
//
// Two responsibilities, neither of them physics:
//   (a) generateFluidSeedShell builds the seed shape -- a closed box mesh fed to
//       aver_phys_softbody_create's verticesXyz/indices. Pure arithmetic, testable with no
//       GPU/physics world, same spirit as GerstnerWave.hpp.
//   (b) FluidVolume::updateFromSimulation takes the result back: world-space vertex positions
//       matching aver_phys_softbody_vertices' output, held for a renderer, normals recomputed each
//       update. Stateful, unlike (a), since a renderer needs something to read on frames the host
//       skips.
//
// The composition root sequences (a) into aver_phys_softbody_create, steps the world, and feeds
// aver_phys_softbody_vertices' readback to (b); none of that lives here -- this file never creates or
// steps a soft body, and does not know a body handle exists.
#include "aver/core/Types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aver::fluids {

// Inverse stiffness of the seed shell's edge constraints, NOT zero like Jolt's default and
// aver::scene::CSoftBody::compliance's cloth/rag-doll default (Components.hpp: inextensible). A
// perfectly inextensible shell bounces like a taut balloon skin; a liquid has no skin, so letting the
// edges give a little lets it sag and slosh instead. An informed starting point, not measured (no
// GPU Gems-style derivation exists for an XPBD compliance value) -- just the direction: more than 0,
// not so much the shell loses its shape.
constexpr f32 kHeavyLiquidCompliance = 1.0e-4f;

// Jolt's own defaults for SoftBodyCreationSettings::mLinearDamping/::mNumIterations
// (Jolt/Physics/SoftBody/SoftBodyCreationSettings.h), named rather than left as bare numbers, same
// reason as kHeavyLiquidCompliance -- not a behaviour change for existing callers, since
// aver_phys_softbody_create had no way to carry other values down before.
constexpr f32 kDefaultFluidDamping    = 0.1f;
constexpr u32 kDefaultFluidIterations = 5;

// Internal pressure is DERIVED PER VOLUME, not a constant -- see fluidPressureFor() for the
// arithmetic. `pressure` left at this sentinel asks for that derivation; any other non-negative value
// passes to the solver untouched, for an author tuning one particular pool.
constexpr f32 kFluidPressureAuto = -1.0f;

// Fraction of the derived balance point actually used -- MEASURED: the balance assumes pressure alone
// holds the top face up, but the side walls also carry it, so it over-estimates and the shell gains
// volume. Swept on the FirstPerson pool's proportions (6x4x1.2m, 8x8x4, 4s, free-standing on a floor)
// -- final depth vs the 1.2m start:
//
//     0.00 -> 84%   0.25 -> 86%   0.40 -> 95%   0.50 -> 94%
//     0.60 -> 97%   0.75 -> 97%   1.05 -> 134% (ballooning)
//
// 0.4-0.75 holds the shape; past that it inflates. 0.6 sits mid-plateau, clear of the blow-up, far
// enough to survive a shell whose proportions differ from this pool's. SoftBodyTest::testPressureHoldsAShellUp
// pins both ends (collapse and balloon).
constexpr f32 kFluidPressureHeadroom = 0.6f;

// Centimetres to Jolt's metres, for the pressure coefficient specifically: gravity loses a factor of
// 100, volume 1e6, area 1e4 (pressure = gravity * volume / area). Not a general cm->m conversion --
// see fluidPressureFor.
constexpr f32 kFluidCmToJolt = 1.0e-4f;

// Cubic centimetres to cubic metres for a VOLUME alone -- distinct from kFluidCmToJolt above (a
// compound ratio, not interchangeable). 1 cm^3 = (0.01 m)^3 = 1e-6 m^3; fluidParticleMassKg uses this
// to convert desc.halfExtentCm's box into the m^3 a kg/m^3 density multiplies against -- get this one
// wrong and a real-looking density produces a particle mass a million times too large or too small.
constexpr f32 kFluidCmCubedToM3 = 1.0e-6f;

// "No density asked for": the sentinel densityKgM3 defaults to. fluidParticleMassKg treats it (and
// any non-positive value, since a fluid with zero/negative density is not physical) as a request for
// TODAY'S BEHAVIOUR -- mass 1, invMass 1, matching aver_phys_softbody_create's null-invMasses path
// (`v.mInvMass = invMasses ? invMasses[i] : 1.0f`, PhysicsWorld.cpp's buildSoftShared). Defaulting to
// real water (1000) instead would silently make every existing volume ~28,000x heavier than what it
// was tuned against.
constexpr f32 kFluidDensityUnset = -1.0f;

// THE MATERIAL LAYER: real fluid values an author can type -- density and viscosity -- instead of
// FluidVolumeDesc's four raw solver knobs (design brief: "real fluid values that can be entered").
//   densityKgM3 IS REAL: becomes an actual per-particle mass (fluidParticleMassKg), honoured by the
//   solver directly through invMass (testDensityScalesMassAndPressure, FluidVolumeTest.cpp).
//   viscosityPaS IS A CALIBRATED FIT, NOT REAL: Jolt's soft-body solver has no shear-stress term;
//   damping is the closest proxy but removes energy uniformly rather than by shear --
//   fluidDampingForViscosity maps it, see that function's measured calibration.
//   SURFACE TENSION, POUR, SPLIT, MERGE, PUDDLE ARE REFUSED, deliberately -- not merely unimplemented:
//   aver_phys_softbody_create's `indices` never change after a body is built (FluidScene::spawn), so
//   vertex count and edges are FROZEN for its lifetime -- no parameter can make a fixed-topology shell
//   tear, join or reflow.
//
// PRESETS (Water()/Honey()/etc.) are plain factories returning this same struct, not a second
// enum-keyed path; downstream code cannot tell a preset from a hand-typed value. Renamed from
// FluidMaterial (this subsystem briefly had three things called "material": this struct, the surface
// one below, and the `preset` token) to avoid clashing with the SURFACE material a water placement
// also carries (OcWaterPlacement::material, an .ocmat) -- this struct is the solver's only, no
// appearance.
struct FluidPhysicsMaterial {
    // Water's own real figure (design brief 5). Not kFluidDensityUnset's -1 sentinel: unlike
    // FluidVolumeDesc, a FluidPhysicsMaterial only exists once an author or preset has asked for one
    // (via FluidVolumeDesc::material), so it has no "not set" state to protect.
    f32 densityKgM3  = 998.0f;
    // Water's real figure (1.0e-3 Pa*s), also the LOW anchor fluidDampingForViscosity's calibration
    // is pinned to -- see that function's comment for why this exact number, not a round choice.
    f32 viscosityPaS = 1.0e-3f;

    // Real order-of-magnitude figures (design brief 5), each one representative point, not a
    // re-exposed range -- density=/viscosity= are still there to type a different point directly.
    static FluidPhysicsMaterial Water()  { return FluidPhysicsMaterial{998.0f, 1.0e-3f}; }
    // ~900 kg/m^3, ~0.1 Pa*s (SAE-10 oil) -- geometric midpoint of the water/honey viscosity anchors
    // (sqrt(1e-3*10)~=0.1), so also roughly the midpoint of fluidDampingForViscosity's calibrated range.
    static FluidPhysicsMaterial LightOil() { return FluidPhysicsMaterial{900.0f, 0.1f}; }
    // ~1420 kg/m^3; 10.0 Pa*s -- top of the cited 2-10 Pa*s range, chosen to equal
    // fluidDampingForViscosity's HIGH anchor (kViscosityAnchorHighPaS), so Honey() lands on the
    // measured ceiling (damping=3.0) rather than an interpolated point.
    static FluidPhysicsMaterial Honey() { return FluidPhysicsMaterial{1420.0f, 10.0f}; }
    // ~2700-3100 kg/m^3 (basaltic lava); viscosity ~10^2-10^4 Pa*s, here 1000.0, but
    // fluidDampingForViscosity clamps at the Honey() anchor (10 Pa*s, damping=3.0) -- Lava is real,
    // heavier density with a damping presently indistinguishable from Honey's (acknowledged gap).
    static FluidPhysicsMaterial Lava() { return FluidPhysicsMaterial{2900.0f, 1000.0f}; }
};

// One fluid volume's authored placement, size, subdivision and solver tuning -- everything a level
// author or composition root needs to see without opening FluidVolume.cpp.
struct FluidVolumeDesc {
    // World-space centre, in engine centimetres -- hands straight to aver_phys_softbody_create's
    // cx/cy/cz. See generateFluidSeedShell's comment for why the shell is built centred on LOCAL
    // (0,0,0) rather than pre-offset by this point.
    f32 centreCm[3] = {0.0f, 0.0f, 0.0f};

    // Half-extent per axis, cm: shell spans [-halfExtentCm[i], +halfExtentCm[i]] on local axis i
    // (0=X, 1=Y, 2=Z). Defaults to a shallow ~2m x 2m x 1m pool -- a plausible footprint, not a claim
    // about any particular level.
    f32 halfExtentCm[3] = {100.0f, 100.0f, 50.0f};

    // Segments per axis (0=X, 1=Y, 2=Z), clamped to at least 1 by generateFluidSeedShell. Finer on the
    // horizontal footprint than the vertical one by default, since a shallow pool's silhouette reads
    // mostly from its horizontal sloshing, not from vertical detail a camera rarely sees end-on.
    i32 subdivisions[3] = {8, 8, 4};

    // Passed straight through to aver_phys_softbody_create -- see that ABI's own comment for exact
    // semantics. See kHeavyLiquidCompliance for why compliance is not Jolt's cloth-shaped 0, and
    // fluidPressureFor below for why pressure defaults to a sentinel rather than to any number.
    f32 compliance = kHeavyLiquidCompliance;
    f32 pressure   = kFluidPressureAuto;

    // damping (mLinearDamping, 1/s: dv/dt = -damping*v) and iterations (mNumIterations, solver passes
    // per step) -- real Jolt parameters, UNREACHABLE from this struct before aver_phys_softbody_create
    // grew arguments for them. None stands alone for "thickness": compliance sets edge give,
    // iterations sets how crisply that converges, damping sets velocity loss -- a "thick" fluid needs
    // all three. See kDefaultFluidDamping/kDefaultFluidIterations for why these default to Jolt's own
    // numbers.
    f32 damping    = kDefaultFluidDamping;
    u32 iterations = kDefaultFluidIterations;

    // REAL fluid density, kg/m^3 (water ~998, honey ~1420, lava ~2700-3100). Unlike the solver KNOBS
    // above (tuned by feel), this feeds a real per-particle mass (fluidParticleMassKg), honoured
    // directly through invMass. Left at kFluidDensityUnset, a desc gets today's exact behaviour --
    // see that constant's comment for why "unset", not "default to water".
    f32 densityKgM3 = kFluidDensityUnset;

    // THE MATERIAL LAYER: a SECOND, OPTIONAL input on top of densityKgM3/damping above, not a
    // replacement. std::nullopt (default) means the raw knobs alone, untouched -- the same "unset
    // changes nothing" contract as kFluidDensityUnset/kFluidPressureAuto, as an optional rather than
    // a sentinel since FluidPhysicsMaterial is a struct. RESOLVED EXACTLY ONCE, by
    // fluidResolvePhysicsMaterial, called from FluidScene::spawn and nowhere else -- see that
    // function's comment for the precedence rule this field only carries the request for, not
    // adjudicates -- the same split OcWaterPlacement's comment draws between a format struct and its
    // consumer.
    std::optional<FluidPhysicsMaterial> material;
};

// Particle count of the shell generateFluidSeedShell would build for `desc` -- the same
// blockA+blockB+blockC arithmetic that function partitions a box's boundary into, factored out so
// fluidParticleMassKg can divide by it without paying for the vertex array. Shares the formula with
// generateFluidSeedShell's own shellBlockCounts (FluidVolume.cpp) rather than re-deriving it, so the
// two cannot drift apart; FluidVolumeTest checks them against each other.
i32 fluidShellParticleCount(const FluidVolumeDesc& desc);

// Real per-particle mass, in KILOGRAMS -- Jolt's own unit. Confirmed, not assumed: Jolt's gravity
// (`toJoltDir(Vec3(0,0,-980))`, PhysicsWorld.cpp) already runs the same cm->m conversion positions
// get, so a mass in kg needs no further conversion.
//
// mass_total_kg = desc.densityKgM3 * enclosedVolumeM3 (box's 8*hx*hy*hz, the same V as
// fluidPressureFor's derivation, via kFluidCmCubedToM3). mass_per_particle_kg = mass_total_kg /
// fluidShellParticleCount(desc), UNIFORM across the shell -- simpler than fluidPressureFor's
// area-weighted split, since mass (unlike area) is not uneven per particle.
//
// Returns exactly 1.0f (today's implicit mass) when densityKgM3 is at or below kFluidDensityUnset, or
// the desc is too degenerate to divide by (zero particles/volume), rather than a divide-by-zero or a
// made-up number.
f32 fluidParticleMassKg(const FluidVolumeDesc& desc);

// The internal pressure this shell needs, in Jolt's SoftBodyCreationSettings::mPressure units (n R T,
// not force/area -- ApplyPressure in SoftBodyMotionProperties.cpp).
//
// COMPUTED, NOT A CONSTANT: Jolt turns it into a per-face impulse of `pressure * dt / V * area`, so a
// particle's share scales with area/volume, which changes with pool size and subdivision -- no one
// number fits two pools (a hardcoded 20.0 that used to live here was about a millionth of what a
// 6x4x1.2m pool needed, and it collapsed into a puddle within two seconds).
//
// BALANCE, over the WHOLE top face -- exact, unlike balancing a notional per-particle cell, which
// undershoots by (sx+1)(sy+1)/(sx sy), ~27% at the 8x8 a pool actually uses (area share is uneven per
// particle, mass `p` is not) -- for mass `p` per particle (fluidParticleMassKg; reduces to the old
// hardcoded-1 when densityKgM3 is unset). This balance is what changed the moment density became
// real: it is now the only thing standing between pressure holding the shell up and a dense fluid
// puddling regardless of pressure -- the same symptom kFluidPressureHeadroom's sweep documents, but
// from mass, not a bad headroom constant:
//
//     pressure * A / V  ==  gravity * m                 [ balance, over the whole top face ]
//     A = 4 hx hy                                       [ the top face ]
//     m = (sx + 1) (sy + 1) * p                         [ its particles, mass `p` each ]
//     V = 8 hx hy hz                                    [ the box ]
//  => pressure = gravity * V * m / A = 2 * gravity * hz * (sx + 1) (sy + 1) * p
//
// IN METRES: V, a and gravity here are Jolt's, not the engine's cm. Converting costs
// (1/100 gravity) * (1e-6 volume) / (1e-4 area) = 1e-4 overall (kFluidCmToJolt) -- a coefficient
// 10,000x too large once ballooned a 1.2m pool over the camera within four seconds.
//
// INDEPENDENT OF FOOTPRINT (volume and top area scale together and cancel); DEPENDS ON DEPTH and
// subdivision (finer grid means more mass on the same footprint).
//
// `gravityCmPerS2` defaults to PhysicsWorld's startup magnitude -- a world with different gravity
// should pass its own value rather than let an Earth-derived shell float or sink.
f32 fluidPressureFor(const FluidVolumeDesc& desc, f32 gravityCmPerS2 = 980.0f);

// ---------------------------------------------------------------------------------------------
// THE MATERIAL LAYER -- see FluidPhysicsMaterial's comment above for the split this section holds to
// (density real, viscosity a calibrated fit, some things refused outright).

// The two anchors fluidDampingForViscosity's log-log mapping is pinned to -- see that function's
// comment for the measured table; Water()/Honey()'s own viscosityPaS equal these exact numbers so a
// preset anchoring the curve MEASURES the point rather than approximating it.
constexpr f32 kViscosityAnchorLowPaS  = 1.0e-3f;   // water; tau ~ 2.8s at the damping this maps to
constexpr f32 kViscosityAnchorHighPaS = 10.0f;     // honey's own preset value; tau ~ 0.63s
constexpr f32 kDampingAnchorLow  = 0.01f;          // measured: least-damped end of the reliable sweep
constexpr f32 kDampingAnchorHigh = 3.0f;           // measured: the sweep's own tau-minimum, not its
                                                    // noisier, non-monotonic damping=10.0 row

// Jolt's `damping` (mLinearDamping, 1/s) that best reproduces the settling a real fluid of
// `viscosityPaS` would show -- a MEASURED calibration, not a closed form: Jolt's soft-body solver has
// no shear-stress term to derive one from.
//
// MEASUREMENT: tests/physics/src/FluidDampingCalibrationTest.cpp -- spawn the FirstPerson pool shell
// (300x200x60cm, 8x8x4, 258 particles) at production compliance/pressure, settle 3s, hit every
// particle with a 400 cm/s lateral impulse, step 600x1/60s reading aver_phys_softbody_vertices, fit
// mean per-vertex speed to v(t) = v0*exp(-t/tau) by least squares on ln(v) vs t past each run's
// dispersal window (a coherent-kick decay in the shell's pressure/compliance modes, NOT the damping
// signature -- opened once speed first drops below 40 cm/s). Swept across damping:
//
//     damping   tau (s)   v0(fit) cm/s   r^2      points   flag
//     0.01      2.8446    20.224         0.832    293      ok
//     0.03      1.8667    21.850         0.815    234      ok
//     0.10      2.6097    34.586         0.937    359      ok
//     0.30      1.4115    21.789         0.782    175      ok
//     1.00      1.4067    55.165         0.968    274      ok
//     3.00      0.6304    23.180         0.911    121      ok
//     10.00     1.7604    10.883         0.618    248      ok
//
// (damping=0.1 run twice: tau=2.6096778s both times, r^2=0.9365701 both times -- bit-exact, as
// fixed-step Jolt with no randomness should be.)
//
// NOT MONOTONIC PAST damping~3 -- a real finding, not a fit error: damping=10 shows a genuine second
// slosh mode (speed falls 360->3.4 cm/s by t=0.35s, then RISES to 9.4 cm/s at t=2.18s before dying at
// t=4.3s), exactly why that row has the sweep's worst r^2 (0.618). tau falls from 2.84s at 0.01 to a
// MINIMUM of 0.63s at 3.0, then rises and gets noisier at 10.0. The mapping below uses only the
// reliable range [0.01, 3.0] -- the one claim the full sweep supports end-to-end is the direction
// (2.84s vs 1.76s), not a monotonic curve across all seven rows.
//
// THE MAPPING: log-log (power-law) interpolation, clamped outside its anchors --
// kViscosityAnchorLowPaS (water, 1.0e-3 Pa*s) -> kDampingAnchorLow (0.01, tau~2.8s) and
// kViscosityAnchorHighPaS (10 Pa*s, honey) -> kDampingAnchorHigh (3.0, tau~0.63s, the reliable
// ceiling, short of the noisy damping=10.0 row). Anchors and log shape come from the design brief
// (four orders of magnitude is not a linear knob); which two points anchor the curve is this
// function's own choice, from the table above.
//
// CLAMPS RATHER THAN EXTRAPOLATES: a viscosity above kViscosityAnchorHighPaS (lava, see Lava())
// returns the same damping as the high anchor -- an acknowledged undershoot, not an invented formula.
//
// A non-positive viscosityPaS (not physical) returns kDampingAnchorLow rather than NaN from
// log10(<=0) or kDefaultFluidDamping -- the least damping is the honest answer to ~nothing.
f32 fluidDampingForViscosity(f32 viscosityPaS);

// Resolves a named preset (case-insensitive: "water", "lightoil"/"light oil"/"oil", "honey", "lava")
// to the FluidPhysicsMaterial it stands for -- see that struct's factories for the numbers. Returns
// std::nullopt for anything else; caller's job to warn by name, same tolerance
// GraphComponentTree.ApplyKind gives an unrecognised Kind rather than a silent default.
std::optional<FluidPhysicsMaterial> fluidPhysicsMaterialPreset(std::string_view name);

// Applies desc.material onto desc: densityKgM3 -> desc.densityKgM3, viscosityPaS -> desc.damping via
// fluidDampingForViscosity. No-op returning true when desc.material is std::nullopt -- see
// FluidVolumeDesc::material's comment for why "unset" differs from "default to water".
//
// THE PRECEDENCE RULE (design brief section 3), enforced HERE, called from FluidScene::spawn and only
// from there -- the one convergence point regardless of how a fluid was authored: a WATER record via
// game::GameWater::applyLevel (Runtime/include/aver/game/GameWater.hpp -- the editor's own
// applyLevelWater before the editor/runtime split), a graph's `COMP ... Fluid` line via either of the
// framework relay's two providers, or any direct C++ caller.
//
// If desc.material is set AND desc.damping was already changed from kDefaultFluidDamping (a raw
// `damping=` also written), this REFUSES: returns false, leaves desc UNCHANGED (no partial
// application of density without damping, or vice versa), and -- when outConflict is non-null --
// writes a message naming both the implied and conflicting damping. Checked against
// kDefaultFluidDamping rather than a separate "authored" flag, the same sentinel-by-default-value
// convention kFluidPressureAuto/kFluidDensityUnset use.
bool fluidResolvePhysicsMaterial(FluidVolumeDesc& desc, std::string* outConflict = nullptr);

// Builds a closed, subdivided-box triangle shell from `desc`, in LOCAL space -- vertices run from
// -halfExtentCm to +halfExtentCm about local (0,0,0), NOT desc.centreCm, mirroring how
// aver_phys_softbody_create splits placement (cx/cy/cz) from shape (`verticesXyz`, local; see
// modules/render.softbody's CSoftBody wiring). Baking centreCm in here too would double the offset
// once a caller also passes it as cx/cy/cz.
//
// CLOSED: no duplicated seam vertices or T-junctions -- shared corners/edges between the six faces
// are the SAME output vertex. Triangles wind OUTWARD, generalising ChunkMesh.cpp's single-face +Z-up
// convention ("seen from above, (v0, v2, v1) is clockwise and front-facing") to all six faces by
// solving each face's own local u/v axis assignment (worked out algebraically in FluidVolume.cpp).
// UNVERIFIED BY RUNNING ANYTHING: vertex sharing does not depend on winding, so a wrong table entry
// would still produce a CLOSED mesh with one face inside-out, a defect only a per-face winding check
// would catch.
//
// Clears and refills `outPositionsCm` (xyz-interleaved) and `outIndices` (3 per triangle, matching
// aver_phys_softbody_create's `indices`). Pure arithmetic, deterministic, no device/solver/state.
void generateFluidSeedShell(const FluidVolumeDesc& desc,
                             std::vector<f32>& outPositionsCm,
                             std::vector<i32>& outIndices);

// The stateful half: what a renderer reads every frame. Owns the desc a seed shell was built from,
// the topology, and current positions/normals -- the seed shell until the first simulated frame
// arrives, the solver's output after.
class FluidVolume {
public:
    explicit FluidVolume(const FluidVolumeDesc& desc) : desc_(desc) {}

    const FluidVolumeDesc& desc() const { return desc_; }

    // (a) Calls generateFluidSeedShell and keeps the result. Also seeds positionsCm()/normals() with
    // that shell placed at desc().centreCm (LOCAL + centre = WORLD), so a renderer drawing before the
    // physics solver's first callback still gets a correctly-shaped, lit body, not an empty one.
    void generateSeedShell();

    // What a caller hands to aver_phys_softbody_create: the LOCAL seed positions and the closed
    // topology generateSeedShell built. Empty until generateSeedShell has been called.
    const std::vector<f32>& seedPositionsCm() const { return seedPositionsCm_; }
    const std::vector<i32>& indices() const { return indices_; }
    i32 vertexCount() const { return static_cast<i32>(seedPositionsCm_.size() / 3); }

    // (b) Copies this frame's simulated vertex positions -- WORLD-space, xyz-interleaved, matching
    // aver_phys_softbody_vertices' buffer -- into positionsCm(), then recomputes normals(). `vertexCount`
    // should equal this object's own vertexCount() (particle count never changes once built, so a
    // mismatch can only mean the wrong buffer went to the wrong FluidVolume); rather than trust it,
    // only the overlapping prefix is copied, so a wrong buffer shows a partially-stuck body rather
    // than a crash or an out-of-bounds read.
    void updateFromSimulation(const f32* verticesXyz, i32 vertexCount);

    // Current vertex positions (world-space cm) and per-vertex normals, xyz-interleaved, index-parallel
    // with indices() -- what a renderer draws. Recomputed by area-weighted face averaging every time
    // positionsCm() changes (recomputeNormals), since the seed shell's normals stop being correct the
    // instant the body deforms.
    const std::vector<f32>& positionsCm() const { return positionsCm_; }
    const std::vector<f32>& normals() const { return normals_; }

private:
    void recomputeNormals();

    FluidVolumeDesc desc_;
    std::vector<f32> seedPositionsCm_;
    std::vector<i32> indices_;
    std::vector<f32> positionsCm_;
    std::vector<f32> normals_;
};

} // namespace aver::fluids
