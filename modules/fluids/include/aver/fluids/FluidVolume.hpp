#pragma once
// A fluid volume the water module offers, simulated on the CPU by a position-based fluid (PbfFluid.hpp)
// rather than a Jolt soft-body shell: a pressurised shell is a balloon, and no single pressure holds
// every shape (docs/FLUIDS.md). This header holds the authored description (FluidVolumeDesc), the
// real-unit material layer, the mapping from a desc to solver settings, and the box "seed shell" the
// graph editor draws as a preview of the volume's extent. The solver is PbfFluid.hpp, the surface mesh
// FluidSurface.hpp, and FluidScene.hpp joins them to the rasteriser. Nothing here names Jolt.
#include "aver/core/Types.hpp"
#include "aver/fluids/PbfFluid.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aver::fluids {

// Defaults of the fields the solver no longer reads. `compliance`/`pressure` were Jolt soft-body knobs
// (edge give, shell pressure); a desc that still sets them is accepted and logged once, then ignored.
// `damping` and `iterations` still mean something: see fluidPbfSettings.
constexpr f32 kFluidLegacyCompliance = 1.0e-4f;
constexpr f32 kDefaultFluidDamping    = 0.1f;
constexpr u32 kDefaultFluidIterations = 5;

// "Not authored" for the retired `pressure` field -- the same sentinel a WATER record and the managed
// relay already pass, kept so existing callers and files parse and spawn unchanged.
constexpr f32 kFluidPressureAuto = -1.0f;

// "No density asked for": the sentinel densityKgM3 defaults to (any non-positive value is the same,
// since a fluid with zero/negative density is not physical).
constexpr f32 kFluidDensityUnset = -1.0f;

// THE MATERIAL LAYER: real fluid values an author can type -- density and viscosity -- instead of
// FluidVolumeDesc's four raw solver knobs (design brief: "real fluid values that can be entered").
//   densityKgM3 is kept for the record and for preset identity; a PBF fluid is incompressible at one
//   rest density, so it does not change how the particles move (gravity acts the same on any density).
//   viscosityPaS IS A CALIBRATED FIT, NOT REAL: it maps to `damping` (fluidDampingForViscosity, a table
//   measured on the old solver) and `damping` maps to the XSPH viscosity coefficient
//   (fluidXsphForDamping) -- a thicker fluid shears less, which is the direction, not a unit.
//   SURFACE TENSION is not modelled. POUR, SPLIT and MERGE need nothing special now: it is particles.
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
    // World-space centre, in engine centimetres. The solver works in metres LOCAL to this point; the
    // preview shell (generateFluidSeedShell) is likewise built about local (0,0,0).
    f32 centreCm[3] = {0.0f, 0.0f, 0.0f};

    // Half-extent per axis, cm: shell spans [-halfExtentCm[i], +halfExtentCm[i]] on local axis i
    // (0=X, 1=Y, 2=Z). Defaults to a shallow ~2m x 2m x 1m pool -- a plausible footprint, not a claim
    // about any particular level.
    f32 halfExtentCm[3] = {100.0f, 100.0f, 50.0f};

    // Segments per axis (0=X, 1=Y, 2=Z), clamped to at least 1 by generateFluidSeedShell. Finer on the
    // horizontal footprint than the vertical one by default, since a shallow pool's silhouette reads
    // mostly from its horizontal sloshing, not from vertical detail a camera rarely sees end-on.
    i32 subdivisions[3] = {8, 8, 4};

    // Retired Jolt soft-body knobs (edge give, shell pressure): still parsed and carried so existing
    // .ocworld/.ocgraph/script calls keep working, but the particle solver has no use for them.
    f32 compliance = kFluidLegacyCompliance;
    f32 pressure   = kFluidPressureAuto;

    // `damping` (1/s) is now the fluid's thickness: fluidXsphForDamping maps it to the XSPH viscosity
    // coefficient. `iterations` is the density-solver passes per substep (clamped to 2..12).
    f32 damping    = kDefaultFluidDamping;
    u32 iterations = kDefaultFluidIterations;

    // Particle fill. The volume's box is filled with a lattice of particles at `particleSpacingCm`;
    // 0 derives it from `subdivisions`: 2 particles per subdivision per axis (8 per cell), so the
    // authored resolution still means "finer is more particles". Spacing is coarsened until the count
    // fits maxParticles, the cost cap (about 1.5 ms per 1000 particles per substep, docs/FLUIDS.md).
    f32 particleSpacingCm = 0.0f;
    u32 maxParticles      = 4000;

    // The volume's own box as container: side walls and floor on, top open (a pool). Scene colliders
    // are collided against in addition (FluidScene voxelises them at spawn).
    bool containSides = true;
    bool containFloor = true;
    bool containTop   = false;

    // REAL fluid density, kg/m^3 (water ~998, honey ~1420, lava ~2700-3100); see the material layer
    // above for what it does and does not change. Left at kFluidDensityUnset it is simply unspecified.
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

// PARTICLE SOLVER SETTINGS FROM A DESC.
//
// Fill spacing in cm: desc.particleSpacingCm when positive, else the spacing at which 8 particles
// fill each authored subdivision cell (never below 1 cm). PbfFluid::init may coarsen it further to
// respect maxParticles.
f32 fluidParticleSpacingCm(const FluidVolumeDesc& desc);

// XSPH viscosity coefficient for a `damping` value: 0.01 + 0.1 * damping, clamped to [0.01, 0.5].
// Water's mapped damping (0.01) is the thinnest end; honey's (3.0) is visibly thick but still flows.
f32 fluidXsphForDamping(f32 damping);

// The solver settings a desc asks for: bounds = the volume's box in metres LOCAL to its centre (cm to
// m via kFluidCmToM), walls per the contain* flags, spacing from fluidParticleSpacingCm, iterations
// and viscosity from the desc. Gravity is Earth's, Z down.
PbfSettings fluidPbfSettings(const FluidVolumeDesc& desc);

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

// The `damping` (1/s) that best reproduces the settling a real fluid of `viscosityPaS` would show --
// a MEASURED calibration, not a closed form, taken on the previous Jolt soft-body solver (which has no
// shear-stress term to derive one from) and kept as the monotonic viscosity -> damping scale that
// fluidXsphForDamping now turns into the PBF viscosity.
//
// MEASUREMENT (old solver): tests/physics/src/FluidDampingCalibrationTest.cpp -- spawn the FirstPerson pool shell
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
// -halfExtentCm to +halfExtentCm about local (0,0,0), NOT desc.centreCm. This is the box the graph
// editor previews for a Fluid component; the simulated volume's own surface comes from the particles.
//
// CLOSED: no duplicated seam vertices or T-junctions -- shared corners/edges between the six faces
// are the SAME output vertex. Triangles wind OUTWARD, generalising ChunkMesh.cpp's single-face +Z-up
// convention ("seen from above, (v0, v2, v1) is clockwise and front-facing") to all six faces by
// solving each face's own local u/v axis assignment (worked out algebraically in FluidVolume.cpp).
// UNVERIFIED BY RUNNING ANYTHING: vertex sharing does not depend on winding, so a wrong table entry
// would still produce a CLOSED mesh with one face inside-out, a defect only a per-face winding check
// would catch.
//
// Clears and refills `outPositionsCm` (xyz-interleaved) and `outIndices` (3 per triangle). Pure
// arithmetic, deterministic, no device/solver/state.
void generateFluidSeedShell(const FluidVolumeDesc& desc,
                             std::vector<f32>& outPositionsCm,
                             std::vector<i32>& outIndices);

// The desc plus its preview shell: what the graph editor draws for a Fluid component (a box at the
// desc's centre, with outward normals). Not the simulated surface -- see FluidScene.
class FluidVolume {
public:
    explicit FluidVolume(const FluidVolumeDesc& desc) : desc_(desc) {}

    const FluidVolumeDesc& desc() const { return desc_; }

    // Calls generateFluidSeedShell and keeps the result, and fills positionsCm()/normals() with that
    // shell placed at desc().centreCm (LOCAL + centre = WORLD).
    void generateSeedShell();

    // The LOCAL seed positions and the closed topology generateSeedShell built. Empty until it has run.
    const std::vector<f32>& seedPositionsCm() const { return seedPositionsCm_; }
    const std::vector<i32>& indices() const { return indices_; }
    i32 vertexCount() const { return static_cast<i32>(seedPositionsCm_.size() / 3); }

    // Preview vertex positions (world-space cm) and per-vertex normals, xyz-interleaved, index-parallel
    // with indices().
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
