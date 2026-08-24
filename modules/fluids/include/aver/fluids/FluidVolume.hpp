#pragma once
// A fluid volume the water module can offer, simulated by the engine's existing Jolt-backed soft-body
// solver -- without this module ever learning that Jolt exists. See README.md's own table row on
// buoyancy for the rule this file is built to satisfy: "this module must never learn what Jolt is",
// and the two halves of anything physical here "meet in the composition root and nowhere else". This
// file is the water-module half; modules/physics/include/aver/physics/physics_abi.h's
// aver_phys_softbody_* block is the physics-module half. Nothing below includes that header, names a
// Jolt type, or calls an aver_phys_ symbol.
//
// FluidVolume does TWO things, and NEITHER of them is physics:
//
//   (a) generateFluidSeedShell BUILDS THE SEED SHAPE: a closed, subdivided-box triangle mesh sized to
//       an authored volume. That is what the solver is BUILT FROM -- fed to aver_phys_softbody_create
//       as its `verticesXyz`/`indices` -- but this file never calls that function; it only produces
//       the plain arrays that function's parameters expect. Pure arithmetic, the same "no device, no
//       solver" spirit GerstnerWave.hpp's math is written in, and directly testable the same way: hand
//       it a FluidVolumeDesc, get back two vectors, check them with no GPU and no physics world
//       involved anywhere.
//
//   (b) FluidVolume::updateFromSimulation ACCEPTS THE RESULT BACK: a flat float array of world-space
//       vertex positions, exactly the shape aver_phys_softbody_vertices writes into a caller's buffer
//       -- and holds them for a renderer to read, alongside per-vertex normals recomputed from the
//       deformed shape every time new positions arrive. This half is necessarily stateful (a renderer
//       needs something to read on a frame where the host does not call in), which is why it lives on
//       a class rather than as another pure function like (a).
//
// The composition root is what turns this into an actual simulated fluid: it is the one piece of code
// that hands generateFluidSeedShell's output to aver_phys_softbody_create, steps the physics world,
// reads aver_phys_softbody_vertices back, and hands THAT to FluidVolume::updateFromSimulation. None of
// that sequencing lives here. This file never creates a soft body, never steps one, and does not know
// a body handle exists.
#include "aver/core/Types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aver::fluids {

// Inverse stiffness of the seed shell's own edge constraints, NOT zero -- unlike Jolt's own default
// and aver::scene::CSoftBody::compliance's "sane default" of 0 (Components.hpp: inextensible), which
// is right for cloth and rag-doll meshes that are meant to hold their authored shape. A pressurised,
// PERFECTLY inextensible shell behaves like a taut balloon skin: push on it and the elastic energy has
// nowhere to go but back, which reads as a bounce. A liquid's surface has no skin at all, so letting
// the edges give a little is what lets the constraint solver settle into a slow sag and slosh instead
// of springing back. This is an informed starting point, not a measured one -- there is no equivalent
// of GPU Gems' steepness derivation to cite for an XPBD compliance value, only the direction: more
// than cloth's 0, not so much the shell loses its shape entirely.
constexpr f32 kHeavyLiquidCompliance = 1.0e-4f;

// Jolt's own defaults for SoftBodyCreationSettings::mLinearDamping and ::mNumIterations
// (Jolt/Physics/SoftBody/SoftBodyCreationSettings.h), named here rather than left as bare numbers
// where addSoftBody assigns them -- the same reason kHeavyLiquidCompliance is a constant and not an
// inline 1.0e-4f. Every fluid volume this engine has ever spawned already runs at these two values:
// aver_phys_softbody_create had no parameter to carry anything else down to Jolt's
// SoftBodyCreationSettings, which itself default-constructs to exactly this pair, so giving
// FluidVolumeDesc these same numbers as its own defaults is not a behaviour change for a single
// caller that existed before this pair of fields did.
constexpr f32 kDefaultFluidDamping    = 0.1f;
constexpr u32 kDefaultFluidIterations = 5;

// Internal pressure is DERIVED PER VOLUME, not carried as a constant -- see fluidPressureFor()
// below for the arithmetic and for the measurement that forced it. A `pressure` left at this sentinel
// asks for that derivation; any other non-negative value is passed to the solver untouched, which is
// what an author tuning one particular pool needs.
constexpr f32 kFluidPressureAuto = -1.0f;

// How much of the derived balance point to actually use, and this one IS measured rather than
// argued. The balance below assumes pressure alone holds the top face up; in a real shell the top
// particles are also carried by the constraint network that runs down the side walls to the floor, so
// the full balance is an over-estimate and the shell gains volume. Swept on the FirstPerson pool's
// own proportions (6 x 4 x 1.2 m at 8x8x4, four seconds, free-standing on a floor) -- final depth as
// a fraction of the 1.2 m it started with:
//
//     0.00 -> 84%   0.25 -> 86%   0.40 -> 95%   0.50 -> 94%
//     0.60 -> 97%   0.75 -> 97%   1.05 -> 134% (ballooning)
//
// Anywhere from 0.4 to 0.75 holds the shape; past that it inflates. 0.6 sits in the middle of that
// plateau, far enough from the blow-up to survive a shell whose proportions differ from a pool's.
// SoftBodyTest::testPressureHoldsAShellUp pins both ends -- collapse and balloon -- so a change here
// that reaches either has a test to answer to.
constexpr f32 kFluidPressureHeadroom = 0.6f;

// Centimetres to Jolt's metres, for the pressure coefficient specifically: gravity loses a factor of
// 100, the enclosed volume 1e6, and the face area 1e4, and pressure is gravity * volume / area. Not
// a general cm->m conversion and not interchangeable with one -- see fluidPressureFor.
constexpr f32 kFluidCmToJolt = 1.0e-4f;

// Cubic centimetres to cubic metres, for a VOLUME on its own -- distinct from kFluidCmToJolt just
// above, which is a compound factor for a specific ratio (gravity * volume / area) and is not
// interchangeable with this one. The arithmetic: 1 cm = 0.01 m, so 1 cm^3 = (0.01 m)^3 = 1e-6 m^3.
// This is the conversion fluidParticleMassKg uses to turn desc.halfExtentCm's box (authored in the
// engine's own centimetres, same as everywhere else in this file) into the cubic metres a density in
// kg/m^3 actually multiplies against -- get this one wrong and a real-looking density number produces
// a particle mass a million times too large or too small, the exact "invisible in one direction"
// failure kFluidCmToJolt's own comment already names for pressure.
constexpr f32 kFluidCmCubedToM3 = 1.0e-6f;

// "No density asked for": the sentinel FluidVolumeDesc::densityKgM3 defaults to, and the value
// fluidParticleMassKg treats as a request for TODAY'S EXACT BEHAVIOUR -- every particle at mass 1,
// invMass 1, exactly what aver_phys_softbody_create already does when handed a null invMasses array
// (PhysicsWorld.cpp's buildSoftShared: `v.mInvMass = invMasses ? invMasses[i] : 1.0f`). A caller that
// has never heard of this field must see NO behaviour change; defaulting densityKgM3 to real water
// (1000) instead would have silently made every existing fluid volume in the project ~28,000x heavier
// than the mass=1 particles it was tuned against, which is the same "silent override" failure mode
// the design brief's precedence rule (density vs. a hand-set damping) exists to refuse elsewhere.
// Zero or any other non-positive value is treated the same as this sentinel -- a fluid with zero or
// negative density is not physical, so there is no reading of it worth honouring over the fallback.
constexpr f32 kFluidDensityUnset = -1.0f;

// THE MATERIAL LAYER: real fluid values an author can type -- density and viscosity -- instead of
// the four solver knobs on FluidVolumeDesc below. See the design brief this struct was specified
// from ("could we add abstraction so the values are real fluid values that can be entered") for the
// honest split this type exists to hold to:
//
//   densityKgM3 IS REAL, NOT A FIT. It becomes an actual per-particle mass (fluidParticleMassKg),
//   which the solver honours directly through invMass -- a denser fluid genuinely has more inertia
//   and sags harder under the same pressure and compliance, checkable the same way
//   testDensityScalesMassAndPressure already checks it (FluidVolumeTest.cpp).
//
//   viscosityPaS IS A CALIBRATED FIT, NOT REAL. Jolt's soft-body solver has no shear-stress term at
//   all; per-vertex linear damping is the closest proxy it has, and damping removes energy
//   uniformly rather than in proportion to shear. fluidDampingForViscosity (below) is the fit this
//   field is mapped through -- see that function's own comment for the measured decay-time-constant
//   table the mapping actually rests on, not an invented closed form dressed up in Pa*s.
//
//   SURFACE TENSION, POUR, SPLIT, MERGE AND PUDDLE ARE REFUSED -- deliberately, and not merely
//   unimplemented. aver_phys_softbody_create's own `indices` never change after a body is built
//   (see FluidScene::spawn), so this shell's vertex count and the edges between them are FROZEN for
//   its whole lifetime. No parameter can make a fixed-topology shell tear, join or reflow, so none
//   is offered -- an author who wants a fluid to pour or merge is asking for a different kind of
//   simulation than a pressurised soft-body shell can ever be, and a knob that pretended otherwise
//   would be a worse answer than no knob at all.
//
// PRESETS RETURN THE SAME STRUCT AN AUTHOR FILLS BY HAND -- FluidMaterial::Water(), ::Honey(), and
// so on are plain factory functions, not a second enum-keyed path through the spawn code. By the
// time anything downstream of these (fluidResolveMaterial, FluidScene::spawn) sees a FluidMaterial,
// it cannot tell whether the numbers came from a preset or were typed by hand, and nothing needs to.
struct FluidMaterial {
    // Water's own real figure (design brief 5). Left as the struct default rather than
    // kFluidDensityUnset's own -1 sentinel: FluidMaterial has no "not set" state of its own --
    // unlike FluidVolumeDesc, which must stay silent about density until asked, a FluidMaterial only
    // ever exists once an author (or a preset) has actually asked for one, via
    // FluidVolumeDesc::material below.
    f32 densityKgM3  = 998.0f;
    // Water's own real figure too (1.0x10^-3 Pa*s), and also the LOW anchor
    // fluidDampingForViscosity's own calibration was measured against -- see that function's
    // comment for why this specific number, not a round 1e-3 chosen for looks, is what the mapping
    // is pinned to.
    f32 viscosityPaS = 1.0e-3f;

    // Real order-of-magnitude figures (design brief 5), each a single representative point rather
    // than a re-exposed range -- an author who wants a different point in a cited range still has
    // density=/viscosity= to type it directly; a preset is a starting point, not the only water.
    static FluidMaterial Water()  { return FluidMaterial{998.0f, 1.0e-3f}; }
    // ~900 kg/m^3, ~0.1 Pa*s (SAE-10 machine oil) -- the geometric midpoint of the water/honey
    // viscosity anchors below (sqrt(1e-3 * 10) ~= 0.1), so LightOil is also roughly the midpoint of
    // fluidDampingForViscosity's own calibrated range, not just of the two named liquids either side.
    static FluidMaterial LightOil() { return FluidMaterial{900.0f, 0.1f}; }
    // ~1420 kg/m^3; viscosity 10.0 Pa*s -- the TOP of the cited 2-10 Pa*s range, chosen deliberately
    // to equal fluidDampingForViscosity's own HIGH anchor (kViscosityAnchorHighPaS) rather than some
    // other point inside that range, so Honey() maps to exactly the calibrated ceiling
    // (damping=3.0), a measured point, instead of landing at an interpolated one.
    static FluidMaterial Honey() { return FluidMaterial{1420.0f, 10.0f}; }
    // ~2700-3100 kg/m^3 (basaltic lava), density figure taken near the middle of that range;
    // viscosity ~10^2-10^4 Pa*s, of which this picks 1000.0 as a representative point -- BUT SEE
    // fluidDampingForViscosity's OWN COMMENT: the calibration's reliable range tops out at the
    // Honey() anchor (10 Pa*s, damping=3.0), so Lava()'s much larger viscosity clamps to that exact
    // same damping. Lava is real density (heavier sag, genuinely) with a damping response that is
    // presently indistinguishable from Honey's -- a real, acknowledged gap, not something to paper
    // over with an extrapolated formula past where anything was ever measured.
    static FluidMaterial Lava() { return FluidMaterial{2900.0f, 1000.0f}; }
};

// One fluid volume's authored placement, size, subdivision and solver tuning -- everything a level
// author or composition root needs to see without opening FluidVolume.cpp.
struct FluidVolumeDesc {
    // World-space centre, in engine centimetres. This is exactly what a caller hands to
    // aver_phys_softbody_create's cx/cy/cz -- see generateFluidSeedShell's own comment for why the
    // shell it builds is centred on LOCAL (0,0,0) rather than pre-offset by this point.
    f32 centreCm[3] = {0.0f, 0.0f, 0.0f};

    // Half-extent along each axis, in centimetres: the shell spans [-halfExtentCm[i], +halfExtentCm[i]]
    // along local axis i (0=X, 1=Y, 2=Z). Defaults to a shallow, roughly square pool -- 2 m by 2 m by
    // 1 m -- a plausible starting footprint rather than a claim about any particular level.
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

    // damping (SoftBodyCreationSettings::mLinearDamping, 1/s: dv/dt = -damping * v) and iterations
    // (::mNumIterations, the solver passes run per physics step) -- both real Jolt parameters that
    // were UNREACHABLE from this struct before aver_phys_softbody_create grew arguments for them.
    // Not "thickness" or "viscosity" on their own: compliance alone sets how much the shell's edges
    // give, iterations alone sets how crisply that compliance converges rather than looking rubbery,
    // and damping alone is the single biggest lever on how fast a disturbed particle loses velocity
    // -- an author asking for a "thick" fluid is asking for a combination of the three, and no one of
    // them stands in for the others. See kDefaultFluidDamping/kDefaultFluidIterations above for why
    // these two default to Jolt's own numbers rather than anything this module chose.
    f32 damping    = kDefaultFluidDamping;
    u32 iterations = kDefaultFluidIterations;

    // REAL fluid density, kg/m^3 -- water is ~998, honey ~1420, lava ~2700-3100. Unlike compliance/
    // pressure/damping/iterations above (solver KNOBS, tuned by feel), this is a genuinely physical
    // quantity: it feeds a real per-particle mass (see fluidParticleMassKg below), and mass is
    // something the solver honours directly through invMass, not an approximation dressed up in SI
    // units. Left at kFluidDensityUnset, a desc gets today's exact behaviour -- see that constant's
    // own comment for why the sentinel is "unset", not "default to water".
    f32 densityKgM3 = kFluidDensityUnset;

    // THE MATERIAL LAYER: a SECOND, OPTIONAL input on top of densityKgM3 and damping above, not a
    // replacement for either. std::nullopt (the default) means exactly what an author who has never
    // heard of FluidMaterial already gets: the raw knobs above, alone, untouched -- the identical
    // "unset changes nothing" contract kFluidDensityUnset and kFluidPressureAuto already hold for
    // their own fields, just expressed as an optional rather than a sentinel because FluidMaterial is
    // a struct, not a single number a magic value can hide inside.
    //
    // RESOLVED EXACTLY ONCE, BY fluidResolveMaterial, CALLED FROM FluidScene::spawn AND NOWHERE ELSE
    // -- see that function's own comment for why THAT call site, not this field, is where the
    // design brief's precedence rule (a material and a hand-set raw damping on the same desc is a
    // refusal, not a silent pick) is actually enforced. This field only carries what was asked for;
    // it does not adjudicate anything, the same division of labour OcWaterPlacement's own comment
    // already draws between a format struct and its consumer.
    std::optional<FluidMaterial> material;
};

// Particle count of the shell generateFluidSeedShell would build for `desc` -- the SAME
// blockA+blockB+blockC arithmetic that function partitions a box's boundary into (see its own
// top-of-file comment), factored out so fluidParticleMassKg below can answer "how many particles
// will share this fluid's total mass" without paying for the vertex array itself. Sharing the exact
// block-size formula with generateFluidSeedShell (FluidVolume.cpp's own shellBlockCounts helper)
// rather than re-deriving it here is what keeps the two from silently drifting apart; FluidVolumeTest
// checks them against each other directly for that reason.
i32 fluidShellParticleCount(const FluidVolumeDesc& desc);

// Real per-particle mass, in KILOGRAMS -- Jolt's own mass unit. Confirmed, not assumed: Jolt's own
// gravity is set from `toJoltDir(Vec3(0,0,-980))` (PhysicsWorld.cpp), i.e. -9.8 m/s^2 after the same
// cm->m conversion positions get, so this module's whole physics side is already running in ordinary
// SI units and a mass in kg needs no further conversion once the VOLUME feeding it has one (see
// kFluidCmCubedToM3).
//
// mass_total_kg = desc.densityKgM3 * enclosedVolumeM3, where enclosedVolumeM3 is the box's own
// 8 * hx * hy * hz -- exactly the V that already appears in fluidPressureFor's derivation, just
// converted out of the centimetres desc.halfExtentCm is authored in.
//
// mass_per_particle_kg = mass_total_kg / fluidShellParticleCount(desc), UNIFORM across every
// particle regardless of where it sits on the shell -- deliberately simpler than fluidPressureFor's
// own area-weighted split. That function has to respect that a rim particle owns less surface area
// than an interior one, because pressure acts on AREA; this one does not, because every particle IS
// one particle wherever it sits, and "mass = density * volume / particleCount" has nothing else to
// weight an equal split by.
//
// Returns exactly 1.0f -- today's implicit mass, unconditionally -- when desc.densityKgM3 is at or
// below kFluidDensityUnset, or when the desc is too degenerate to divide by (zero particles, zero
// volume): a caller that never asked for a real density, or asked for a nonsensical one, gets the
// behaviour it had before this function existed rather than a divide-by-zero or a silently made-up
// number.
f32 fluidParticleMassKg(const FluidVolumeDesc& desc);

// The internal pressure this shell needs, in the units Jolt's SoftBodyCreationSettings::mPressure
// takes (n R T, not a force per area -- see ApplyPressure in SoftBodyMotionProperties.cpp).
//
// WHY THIS IS COMPUTED AND NOT A CONSTANT. Jolt turns the coefficient into a per-face impulse of
// `pressure * dt / V * area` along the face normal, so what a particle actually feels scales with
// its share of surface area DIVIDED BY the enclosed volume. Both of those change with the pool's
// size and with how finely it is subdivided, so one number cannot be right for two different pools:
// a constant tuned for a bathtub is a rounding error inside a reservoir. The 20.0 that used to live
// here was a rounding error inside a 6 m x 4 m x 1.2 m pool -- about a millionth of what that shell
// needed -- and the volume collapsed into a puddle on the basin floor within two seconds.
//
// THE BALANCE POINT. For a particle on the top face, with mass `p` (see fluidParticleMassKg below --
// this USED to be hardcoded 1 here, back when aver_phys_softbody_create was passed no mass array at
// all and every particle got Jolt's own implicit invMass 1; a desc with densityKgM3 left at
// kFluidDensityUnset still gets `p` == 1.0f from that function, so this derivation reduces to
// exactly its old self for every caller that predates density) and area share `a`:
//
//     pressure * A / V  ==  gravity * m                 [ balance, over the whole top face ]
//     A = 4 hx hy                                       [ the top face ]
//     m = (sx + 1) (sy + 1) * p                         [ its particles, mass `p` each ]
//     V = 8 hx hy hz                                    [ the box ]
//  => pressure = gravity * V * m / A = 2 * gravity * hz * (sx + 1) (sy + 1) * p
//
// WHY THIS HAD TO CHANGE THE MOMENT DENSITY BECAME REAL. Once fluidParticleMassKg can return
// anything other than 1, this balance is the ONLY thing standing between "pressure holds the shell
// up" and "pressure was tuned for particles a fraction of their real weight, so a dense fluid
// puddles regardless of pressure" -- the same symptom kFluidPressureHeadroom's own sweep comment
// already documents, but from a cause that sweep never had to account for (mass), not from a bad
// headroom constant.
//
// COUNTED OVER THE WHOLE FACE rather than per particle, because a particle's share of the area is
// not uniform -- the ones on the rim own half a cell, the corners a quarter -- while its MASS is `p`
// wherever it sits. Balancing the totals is exact; balancing a notional per-particle cell is
// only asymptotically right, and undershoots by (sx+1)(sy+1)/(sx sy) -- 27% at the 8x8 a pool
// actually uses.
//
// IN METRES, NOT CENTIMETRES, and this is the whole of the arithmetic that is easy to get wrong.
// Jolt stores positions in metres and velocities in m/s, so V, a and gravity in that balance are all
// Jolt's, not the engine's. Converting a desc written in centimetres costs a factor of
// (1/100 gravity) * (1e-6 volume) / (1e-4 area) = 1e-4 overall, which is where kFluidCmToJolt
// comes from. Getting this wrong is not subtle in one direction and invisible in the other: a
// coefficient 10,000x too large turned a 1.2 m deep pool into a balloon that swallowed the camera
// inside four seconds.
//
// INDEPENDENT OF THE FOOTPRINT, which is surprising and is right: widening the pool adds enclosed
// volume and top-face area in the same proportion, so they cancel. What it does depend on is depth
// -- a deeper volume needs more pressure to hold the same surface up -- and on the horizontal
// subdivision, because every particle carries the same mass however much area it is responsible for,
// so a finer grid means more mass sitting on the same footprint.
//
// `gravityCmPerS2` is a magnitude, defaulted to the value PhysicsWorld installs at startup. A world
// that changed its gravity should pass the new magnitude rather than let a shell derived for Earth
// float or sink.
f32 fluidPressureFor(const FluidVolumeDesc& desc, f32 gravityCmPerS2 = 980.0f);

// ---------------------------------------------------------------------------------------------
// THE MATERIAL LAYER -- see FluidMaterial's own comment above for the honest split this section
// holds to (density real, viscosity a calibrated fit, some things refused outright).

// The two anchor points fluidDampingForViscosity's log-log mapping is pinned to -- see that
// function's own comment for the measured table these are read off, and FluidMaterial::Water()/
// ::Honey() for why those two presets' own viscosityPaS equal these exact numbers rather than some
// other point in a cited range: a preset that anchors the curve should MEASURE the calibrated
// point, not approximate it.
constexpr f32 kViscosityAnchorLowPaS  = 1.0e-3f;   // water; tau ~ 2.8s at the damping this maps to
constexpr f32 kViscosityAnchorHighPaS = 10.0f;     // honey's own preset value; tau ~ 0.63s
constexpr f32 kDampingAnchorLow  = 0.01f;          // measured: least-damped end of the reliable sweep
constexpr f32 kDampingAnchorHigh = 3.0f;           // measured: the sweep's own tau-minimum, not its
                                                    // noisier, non-monotonic damping=10.0 row

// Jolt's `damping` (SoftBodyCreationSettings::mLinearDamping, 1/s -- see FluidVolumeDesc::damping's
// own comment) that best reproduces the settling behaviour a real fluid of `viscosityPaS` would
// show, per a MEASURED calibration -- not a formula derived from first principles, because Jolt's
// soft-body solver has no shear-stress term for a closed form to derive from in the first place.
//
// THE MEASUREMENT. tests/physics/src/FluidDampingCalibrationTest.cpp: spawn the FirstPerson pool
// shell (300x200x60cm, 8x8x4, 258 particles) at production compliance and pressure, settle 3s, hit
// every particle with the same 400 cm/s lateral impulse at once, then step 600x1/60s (10s) reading
// aver_phys_softbody_vertices every step and fitting mean per-vertex speed to v(t) = v0 * exp(-t/tau)
// by least squares on ln(v) vs t, discarding each run's own dispersal window (a coherent-kick decay
// into the shell's pressure/compliance modes, NOT the damping signature -- opened only once speed
// first drops below a 40 cm/s threshold). Swept across damping:
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
// NOT MONOTONIC PAST damping~3, AND THAT IS A REAL FINDING, NOT A FIT ERROR: the trace at
// damping=10 shows a genuine second slosh mode (speed falls 360->3.4 cm/s by t=0.35s, then RISES to
// 9.4 cm/s at t=2.18s before finally dying at t=4.3s), which is exactly why that row has the
// sweep's worst r^2 (0.618) -- a single exponential is a weaker fit precisely where a second mode is
// visible. tau falls from 2.84s at damping=0.01 to a MINIMUM of 0.63s at damping=3.0, then rises and
// gets noisier at 10.0: beyond ~3, more damping does not reliably buy a shorter settle, it buys a
// less predictable one. So the mapping below is built ONLY on the reliable damping in [0.01, 3.0]
// -- the one claim the full sweep supports end-to-end is the direction (lightest damping fits a
// longer tau than heaviest: 2.84s vs 1.76s), not a monotonic curve across all seven rows.
//
// THE MAPPING is a log-log (power-law) interpolation between two anchors, clamped outside them:
// kViscosityAnchorLowPaS (water, 1.0e-3 Pa*s) -> kDampingAnchorLow (0.01, tau~2.8s, a long slosh)
// and kViscosityAnchorHighPaS (10 Pa*s, at the honey end of the brief's cited 2-10 Pa*s range) ->
// kDampingAnchorHigh (3.0, tau~0.63s, settles almost at once) -- the RELIABLE ceiling the sweep
// above supports, deliberately short of the swept-but-noisy damping=10.0 row. Anchor values and the
// log-scale shape both come from the design brief (four orders of magnitude between water and honey
// kinematic viscosity is not a linear knob); which two measured points anchor the curve, and where
// it clamps, is this function's own choice, made from the table above.
//
// CLAMPS RATHER THAN EXTRAPOLATES past either anchor: a viscosity above kViscosityAnchorHighPaS
// (lava's 10^2-10^4 Pa*s -- see FluidMaterial::Lava()'s own comment) returns the SAME damping as
// the high anchor, not a larger number nothing measured. That undershoot is a real, acknowledged
// gap this comment records rather than paper over with an invented formula reaching past kDampingAnchorHigh.
//
// A non-positive viscosityPaS (not physical -- even water is not 0 Pa*s) returns kDampingAnchorLow,
// the least-damped end of the measured range, rather than NaN from log10(<=0) or Jolt's own
// kDefaultFluidDamping: this function's whole contract is "translate a viscosity", and the least
// damping is the closest honest answer to a viscosity of (approximately) nothing.
f32 fluidDampingForViscosity(f32 viscosityPaS);

// Resolves a named preset (case-insensitive: "water", "lightoil"/"light oil"/"oil", "honey",
// "lava") to the FluidMaterial it stands for -- see that struct's own static factories for the
// actual numbers. Returns std::nullopt for anything else; the caller's job to warn by name, the
// same tolerance GraphComponentTree.ApplyKind already gives an unrecognised component Kind rather
// than silently falling back to some default material.
std::optional<FluidMaterial> fluidMaterialPreset(std::string_view name);

// Applies desc.material onto desc itself: densityKgM3 becomes desc.densityKgM3 (feeding
// fluidParticleMassKg/fluidPressureFor exactly as if an author had typed it by hand), and
// viscosityPaS becomes desc.damping via fluidDampingForViscosity above. A no-op returning true when
// desc.material is std::nullopt -- see FluidVolumeDesc::material's own comment for why "unset" and
// "default to water" are different things, the identical reasoning kFluidDensityUnset already gives.
//
// THE PRECEDENCE RULE (design brief section 3), enforced HERE, called from FluidScene::spawn AND
// ONLY FROM THERE -- the one place every fluid request converges regardless of how it was authored:
// a WATER record via SandboxApp::applyLevelWater's direct construction, a graph's `COMP ... Fluid`
// line via either of the framework relay's two providers, or any future direct C++ caller of
// FluidScene::spawn. Nowhere upstream of that one call needs to remember this rule for it to hold.
//
// If desc.material is set AND desc.damping has already been changed from kDefaultFluidDamping (the
// author ALSO wrote a raw `damping=`), this REFUSES rather than picking a winner: returns false,
// leaves desc entirely UNCHANGED (no partial application of density without damping, or vice
// versa), and -- when outConflict is non-null -- writes a message naming both the material's own
// implied damping and the conflicting raw value, so a caller can log which of the two the author
// probably meant. Checked against kDefaultFluidDamping rather than a separate "was this authored"
// flag because FluidVolumeDesc carries none, the same sentinel-by-default-value convention
// kFluidPressureAuto and kFluidDensityUnset both already use for their own fields.
bool fluidResolveMaterial(FluidVolumeDesc& desc, std::string* outConflict = nullptr);

// Builds a closed, subdivided-box triangle shell from `desc`, in LOCAL (object) space -- vertices run
// from -halfExtentCm to +halfExtentCm about local (0,0,0), NOT about desc.centreCm. That split mirrors
// how aver_phys_softbody_create itself splits placement from shape: `verticesXyz` is local, and a
// separate cx/cy/cz places it in world space (see modules/render.softbody's own CSoftBody wiring,
// which builds its body from a mesh's bind-pose positions and offsets by centre exactly this way).
// Baking desc.centreCm into every vertex here as well would double the offset the moment a caller also
// passes it as cx/cy/cz.
//
// CLOSED means no duplicated seam vertices and no T-junctions: shared corners and edges between the
// box's six faces are the SAME output vertex, not six independently-generated grids glued together by
// coincidence. Every quad's two triangles are wound OUTWARD -- this generalises the winding convention
// modules/landscape/src/ChunkMesh.cpp establishes for a single +Z-up face ("seen from above, (v0, v2,
// v1) is clockwise and front-facing") to all six faces of a box, by solving each face's own local u/v
// axis assignment so the SAME index pattern that convention uses is outward-facing on that face too.
// That is worked out algebraically in FluidVolume.cpp's own comments, not merely asserted -- but it
// has not been checked by running anything; a wrong entry in that table would still produce a CLOSED
// mesh (vertex sharing does not depend on winding) with exactly one face turned inside out, which is
// precisely the kind of defect a closedness check alone would miss and only a per-face winding check
// would catch.
//
// Clears and refills `outPositionsCm` (xyz-interleaved) and `outIndices` (3 per triangle, matching
// aver_phys_softbody_create's own `indices` parameter). Pure arithmetic: no device, no solver, no
// randomness, no persisted state -- the same inputs always produce the same output, callable with
// nothing but a value-constructed FluidVolumeDesc.
void generateFluidSeedShell(const FluidVolumeDesc& desc,
                             std::vector<f32>& outPositionsCm,
                             std::vector<i32>& outIndices);

// The stateful half: what a renderer reads every frame. Owns the desc a seed shell was built from (so
// a caller keeps one object around rather than a desc plus a separately-tracked topology), the
// topology itself, and whichever positions/normals are current -- the just-generated seed shell until
// the first simulated frame arrives, the solver's own output after.
class FluidVolume {
public:
    explicit FluidVolume(const FluidVolumeDesc& desc) : desc_(desc) {}

    const FluidVolumeDesc& desc() const { return desc_; }

    // (a) Calls generateFluidSeedShell and keeps the result. Also seeds positionsCm()/normals() with
    // that shell placed at desc().centreCm (LOCAL + centre = WORLD) and normals computed from THAT --
    // see updateFromSimulation's own comment for why this exists: a renderer that draws before the
    // physics solver's first callback still gets a correctly-shaped, correctly-lit body rather than an
    // empty one.
    void generateSeedShell();

    // What a caller hands to aver_phys_softbody_create: the LOCAL seed positions and the closed
    // topology generateSeedShell built. Empty until generateSeedShell has been called.
    const std::vector<f32>& seedPositionsCm() const { return seedPositionsCm_; }
    const std::vector<i32>& indices() const { return indices_; }
    i32 vertexCount() const { return static_cast<i32>(seedPositionsCm_.size() / 3); }

    // (b) Copies this frame's simulated vertex positions -- WORLD-space, xyz-interleaved, exactly the
    // buffer aver_phys_softbody_vertices fills -- into positionsCm(), then recomputes normals() from
    // them. `vertexCount` is expected to equal this object's own vertexCount(): the solver never adds
    // or removes particles once a soft body is built, so a mismatch can only mean the wrong buffer was
    // handed to the wrong FluidVolume. Rather than trust it, only the overlapping prefix is copied and
    // the rest of positionsCm() is left exactly as it was, so a caller wiring things up wrong sees a
    // partially-stuck body instead of a crash or a read past the end of its own buffer.
    void updateFromSimulation(const f32* verticesXyz, i32 vertexCount);

    // Current vertex positions (world-space cm) and per-vertex normals, xyz-interleaved and
    // index-parallel with indices() -- what a renderer draws. Normals are recomputed by area-weighted
    // face averaging every time positionsCm() changes (see FluidVolume.cpp's recomputeNormals), because
    // the seed shell's own normals stop being correct the instant the body deforms -- that is the whole
    // point of simulating it.
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
