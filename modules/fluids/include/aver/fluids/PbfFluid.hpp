#pragma once
// A position-based fluid (Macklin & Mueller 2013, "Position Based Fluids") on the CPU: particles with a
// density constraint solved through poly6/spiky kernels, a tensile-instability correction, XSPH
// viscosity and optional vorticity confinement. Pure arithmetic over plain arrays: no device, no
// physics world, no rhi::, so tests run it with nothing else linked and a fixed input gives a fixed
// output (single thread, particles visited in index order, neighbours in a deterministic order).
//
// UNITS: metres and seconds, Z up, all positions LOCAL to the volume's centre (the module converts
// engine centimetres at the boundary: kFluidCmToM). Local keeps float precision for a level far from
// the origin.
//
// COLLISION is two things. The volume's own box walls (PbfBounds: each of the six faces can be a wall
// or open), and a SolidField -- a signed-distance grid of the scene's static colliders voxelised once
// from an overlap query the caller supplies. Particles are spheres of radius collisionRadius; a
// particle inside the field's skin is pushed out along the distance gradient.
#include "aver/core/Types.hpp"

#include <functional>
#include <vector>

namespace aver::fluids {

constexpr f32 kFluidCmToM = 0.01f;
constexpr f32 kFluidMToCm = 100.0f;

// Water's density; only the ratio to the lattice's own kernel density matters to the solver, so this
// is a scale, not a tuning knob.
constexpr f32 kPbfRestDensity = 1000.0f;

// The fixed substep. update() accumulates real time and takes whole steps of this size.
constexpr f32 kPbfFixedDt = 1.0f / 60.0f;
// Most substeps one advance() runs; the rest of the accumulated time is dropped (a hitch slows the
// fluid down rather than spiralling into more work).
constexpr u32 kPbfMaxStepsPerAdvance = 2;

// Signed distance (metres, positive outside solid) of the scene's static colliders over a box.
// Voxelised by `overlap(centre, radius)`: does anything solid touch this sphere? (local metres)
class SolidField {
public:
    using OverlapFn = std::function<bool(const f32 centreM[3], f32 radiusM)>;

    // Builds over [lo, hi] at roughly `cellM` (coarsened so the grid stays <= maxCells). A coarse
    // 4x4x4-cell block that the query reports empty is skipped without querying its cells.
    void build(const f32 lo[3], const f32 hi[3], f32 cellM, const OverlapFn& overlap, u32 maxCells = 4000000u);

    bool empty() const { return phi_.empty(); }
    u32  solidCellCount() const { return solidCells_; }

    // Signed distance at p and (optionally) the unit gradient. Outside the grid: +large, no gradient.
    f32 sample(const f32 p[3], f32* outGrad = nullptr) const;

private:
    f32 at(i32 x, i32 y, i32 z) const;

    f32 origin_[3] = {0, 0, 0};
    f32 cell_ = 0.0f;
    i32 dim_[3] = {0, 0, 0};
    u32 solidCells_ = 0;
    std::vector<f32> phi_;
};

struct PbfBounds {
    f32  lo[3] = {-1, -1, -1};
    f32  hi[3] = { 1,  1,  1};
    bool wallLo[3] = {true, true, true};    // -X, -Y, -Z faces solid
    bool wallHi[3] = {true, true, false};   // +X, +Y solid; +Z (top) open
};

struct PbfSettings {
    PbfBounds bounds;                 // the volume's box, local metres; also where particles are seeded
    bool useFill = false;             // seed only fillLo..fillHi (clamped into bounds) instead of the whole box
    f32  fillLo[3] = {0, 0, 0};
    f32  fillHi[3] = {0, 0, 0};
    f32  spacingM = 0.1f;             // lattice spacing the box is filled at
    u32  maxParticles = 8000;         // spacing is coarsened until the fill fits
    u32  iterations = 4;              // density-solver passes per substep
    f32  gravity[3] = {0.0f, 0.0f, -9.81f};
    f32  xsph = 0.02f;                // XSPH viscosity coefficient c
    f32  vorticity = 0.0f;            // vorticity confinement strength; 0 = off
    f32  tensileK = 0.02f;            // tensile-instability correction strength
    f32  relaxation = 0.05f;          // constraint force mixing, relative to the rest lattice
    f32  wallFriction = 0.1f;         // fraction of tangential velocity lost per contact
    f32  restitution = 0.0f;
};

class PbfFluid {
public:
    // Seeds the box with a lattice of particles (skipping any inside `solid`'s skin) and derives the
    // rest density from that lattice. Returns the particle count.
    u32 init(const PbfSettings& s, const SolidField* solid = nullptr);

    // Adds real time; runs whole kPbfFixedDt steps (at most kPbfMaxStepsPerAdvance). Returns steps run.
    u32 advance(f32 dtSeconds);
    // One fixed step of dt seconds.
    void step(f32 dt);

    u32 count() const { return n_; }
    f32 spacing() const { return spacing_; }
    f32 kernelRadius() const { return h_; }
    // xyz-interleaved, local metres.
    const std::vector<f32>& positions() const { return pos_; }
    const std::vector<f32>& velocities() const { return vel_; }
    const PbfSettings& settings() const { return s_; }
    u64 stepsRun() const { return steps_; }

    // Adds `dv` (m/s) to every particle within radius of c, falling off linearly. Local metres.
    void addImpulse(const f32 c[3], f32 radiusM, const f32 dv[3]);

private:
    void buildNeighbours();
    void collide(u32 i, f32* p) const;
    // Density (per unit mass) and its gradient that the walls' mirrored boundary lattice adds at p.
    f32  wallTerm(const f32* p, f32 outGrad[3]) const;
    f32  kernelW(f32 r2) const;

    PbfSettings s_;
    const SolidField* solid_ = nullptr;

    u32 n_ = 0;
    f32 spacing_ = 0.1f, h_ = 0.2f, h2_ = 0.04f;
    f32 radius_ = 0.045f;          // collision radius
    f32 mass_ = 1.0f, rho0_ = kPbfRestDensity;
    f32 mOverRho0_ = 1.0f;         // m / rho0, folded into the constraint gradient
    f32 eps_ = 1.0f, lambdaRef_ = 1.0f, wq_ = 1.0f;
    f32 poly6C_ = 0.0f, spikyC_ = 0.0f;
    f32 maxSpeed_ = 10.0f;
    static constexpr u32 kWallTable = 64;
    std::vector<f32> wallW_, wallDW_;      // ghost-lattice density and d/dd over distance 0..h

    std::vector<f32> pos_, vel_, pred_, delta_, lambda_, omega_;
    std::vector<u32> nStart_, nIdx_;       // neighbour lists, CSR
    std::vector<u32> cellStart_, sorted_, cellKey_;
    f32 accum_ = 0.0f;
    u64 steps_ = 0;
};

} // namespace aver::fluids
