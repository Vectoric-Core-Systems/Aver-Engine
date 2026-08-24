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
};

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
// THE BALANCE POINT. For a particle on the top face, with mass 1 (every particle of a fluid shell
// has invMass 1; aver_phys_softbody_create is passed no mass array) and area share `a`:
//
//     pressure * A / V  ==  gravity * m                 [ balance, over the whole top face ]
//     A = 4 hx hy                                       [ the top face ]
//     m = (sx + 1) (sy + 1)                             [ its particles, one unit of mass each ]
//     V = 8 hx hy hz                                    [ the box ]
//  => pressure = gravity * V * m / A = 2 * gravity * hz * (sx + 1) (sy + 1)
//
// COUNTED OVER THE WHOLE FACE rather than per particle, because a particle's share of the area is
// not uniform -- the ones on the rim own half a cell, the corners a quarter -- while its MASS is one
// unit wherever it sits. Balancing the totals is exact; balancing a notional per-particle cell is
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
