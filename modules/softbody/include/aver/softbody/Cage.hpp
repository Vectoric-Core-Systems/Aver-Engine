// The plastic cage: particles, distance constraints ("beams") and an optional triangle skin.
//
// The data model and the deterministic step. Verlet -> damage/plasticity -> Gauss-Seidel PBD, in
// that order and with that float evaluation order, per docs/recon/softbody-solver.md. Core only:
// no scene, no GPU, no Jolt. See docs/SOFTBODY.md.
#pragma once

#include "aver/softbody/Material.hpp"

#include <unordered_map>
#include <vector>

namespace aver::softbody {

inline constexpr u32 kNone = 0xFFFFFFFFu;

struct Particle {
    Vec3 pos{0, 0, 0};
    Vec3 prev{0, 0, 0};      // Verlet: velocity = pos - prev
    Vec3 rest{0, 0, 0};      // un-dented position, for displacement readback
    f32  invMass  = 1.0f;    // only read when StepConfig::useNodeMass
    u32  origin   = 0;       // the particle this one was torn from (itself when original)
    bool pinned   = false;   // kinematic: the solver never moves it, the host may
    bool freed    = false;   // every constraint broken: falls as debris
    bool brittle  = false;   // majority brittle constraints: cracks instead of denting
    bool hadBeams = false;
    u32  intact   = 0;       // unbroken constraints still holding it
};

struct Beam {
    u32 a = 0, b = 0;        // current endpoints (rewritten when a tear duplicates a particle)
    u32 a0 = 0, b0 = 0;      // as authored, for repair
    u32 material = 0;        // index into Cage::materials
    f32 rest     = 0.0f;     // creeps as the beam yields
    f32 rest0    = 0.0f;     // as authored
    f32 plastic  = 0.0f;     // accumulated permanent set, cm
    f32 peak     = 0.0f;     // highest force seen, N (debug)
    bool broken  = false;
};

// One triangle of the skin. `edge[k]` is the beam on (v[k], v[k+1]); tears read the skin through
// these indices, which do not change when a particle is duplicated.
struct Triangle {
    u32 v[3]{0, 0, 0};
    u32 v0[3]{0, 0, 0};
    u32 edge[3]{kNone, kNone, kNone};
};

// A tear duplicated `oldParticle` into `newParticle`; `triangles` moved to the new one.
// The render side replays these (RenderBinding::applySplits) to duplicate vertices.
struct SplitEvent {
    u32 oldParticle = 0;
    u32 newParticle = 0;
    std::vector<u32> triangles;
};

struct Cage {
    Cage() { materials.emplace_back(); }

    std::vector<Material> materials;     // [0] is the default material
    std::vector<Particle> particles;
    std::vector<Beam>     beams;
    std::vector<Triangle> triangles;
    std::vector<u8>       triDead;       // 1 when a triangle spans a gap (>= 2 broken edges)
    std::vector<SplitEvent> splitLog;    // appended by tears; the owner drains it

    u32  baseParticleCount = 0;          // particle count at build(); duplicates come after
    bool built = false;
    bool topologyDirty = false;

    // Builder-only edge lookup, dropped by build().
    std::unordered_map<u64, u32> edgeIndex;
    // Per-step scratch, kept here so a step allocates nothing in steady state.
    std::vector<Vec3> scratchStart;
};

// ---- building ------------------------------------------------------------------------------------

// Registers a material; returns its index (>= 1).
u32 addMaterial(Cage& c, const Material& m);
// A particle at `p`. A pinned one is kinematic.
u32 addParticle(Cage& c, const Vec3& p, bool pinned = false);
// A distance constraint, rest length = the current distance. Returns the existing beam when the
// pair is already joined. kNone for a bad index or a == b.
u32 addBeam(Cage& c, u32 a, u32 b, u32 material = 0);
// A skin triangle. Missing edges get a beam of `material`. kNone for a bad index.
u32 addTriangle(Cage& c, u32 a, u32 b, u32 d, u32 material = 0);

// A cols x rows sheet of triangles, row-major, origin + du * x + dv * y, diagonal alternating per
// quad. Returns the particle indices (row-major).
struct GridSpec {
    u32  cols = 2, rows = 2;
    Vec3 origin{0, 0, 0};
    Vec3 du{10, 0, 0};
    Vec3 dv{0, 0, 10};
    u32  material = 0;
};
std::vector<u32> addGrid(Cage& c, const GridSpec& g);

// Freezes the topology and captures the rest pose (the spec's InitSolver): prev = pos, rest lengths
// from the current positions, brittle classification, skin edge indices. Call once, after building.
void build(Cage& c);
// Undo every plastic change and tear: rest lengths, broken flags, duplicated particles, positions.
void repair(Cage& c);

// ---- stepping ------------------------------------------------------------------------------------

struct StepConfig {
    f32  dt              = 1.0f / 64.0f;
    i32  substeps        = 4;
    i32  iterations      = 8;
    f32  velocityDamping = 0.99f;    // per substep
    f32  maxNodeSpeed    = 1500.0f;  // cm/s
    f32  damageRate      = 8.0f;     // 1/s: caps how fast a yielded beam creeps
    f32  breakKick       = 2.0f;     // cm of recoil on a snap
    bool enableDamage    = true;     // false: a pure elastic cage
    Vec3 gravity{0, 0, -980.0f};     // cm/s^2
    bool gravityAll      = false;    // false: only freed debris falls (a cage riding a chassis)
    bool useNodeMass     = false;    // weight the relaxation by Particle::invMass
    f32  settleThresholdCm = 0.05f;
};

struct StepResult {
    bool anyBreak   = false;
    u32  broken     = 0;     // constraints broken this step
    u32  newParticles = 0;   // particles created by tears this step
    f32  maxMoveCm  = 0.0f;  // largest per-substep particle motion
    bool settled    = false; // maxMoveCm <= StepConfig::settleThresholdCm
};

// One fixed step of `cfg.dt`. Deterministic: same cage + config + commands give the same bits.
StepResult step(Cage& c, const StepConfig& cfg);

// Marks a beam broken without recoil (scripted or editor cuts). Tears resolve on the next step, or
// by calling resolveTears().
void breakBeam(Cage& c, u32 beam);

// Splits particles whose triangle fans a broken beam has disconnected, duplicating them; updates
// freed flags and dead triangles. Returns the number of particles created.
u32 resolveTears(Cage& c);

// ---- impacts -------------------------------------------------------------------------------------

struct Impact {
    Vec3 point{0, 0, 0};       // cage-local
    Vec3 direction{0, 0, -1};  // the way the impactor moves into the cage
    f32  depthCm   = 10.0f;    // raw depth, before the crush curve
    f32  radiusCm  = 60.0f;
    f32  tearRadiusFrac = 0.5f;
    f32  tearMinDisp    = 2.0f;
};

struct ImpactResult {
    f32 disp = 0.0f;
    f32 tearRadius = 0.0f;
    u32 touched = 0;
    u32 broken = 0;
};

// Seeds a dent: pushes particles within the radius along `direction` through the crush curve
// (brittle ones crack instead), and snaps brittle beams within the tear radius.
ImpactResult applyImpact(Cage& c, const Impact& im, const CrushParams& crush = {});

// ---- queries -------------------------------------------------------------------------------------

// Connected components over unbroken beams. `ids` (optional) receives each particle's component.
u32 countPieces(const Cage& c, std::vector<u32>* ids = nullptr);
u32 brokenBeamCount(const Cage& c);
// Largest |length - rest0| / rest0 over unbroken beams.
f32 maxStrain(const Cage& c);
// Sum of squared per-substep displacement of unpinned particles: a kinetic-energy proxy.
f32 kineticProxy(const Cage& c);

} // namespace aver::softbody
