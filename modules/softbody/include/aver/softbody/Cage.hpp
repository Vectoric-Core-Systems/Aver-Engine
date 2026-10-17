// The soft-body cage: particles, distance constraints ("beams"), dihedral bends and a triangle skin.
//
// Topology and damage state only. Jolt integrates the particles (modules/physics, aver_phys_sb_*);
// the host mirrors their positions into `Particle::pos`, the damage step (Damage.hpp) edits the
// beams, and tears are resolved here into duplicate particles. Core only: no scene, no GPU, no
// Jolt. See docs/SOFTBODY.md.
#pragma once

#include "aver/softbody/Material.hpp"

#include <unordered_map>
#include <vector>

namespace aver::softbody {

inline constexpr u32 kNone = 0xFFFFFFFFu;

struct Particle {
    Vec3 pos{0, 0, 0};       // current position, mirrored from the simulation
    Vec3 rest{0, 0, 0};      // un-dented position, for displacement readback
    f32  mass     = 1.0f;    // kg
    f32  invMass  = 1.0f;    // 0 when pinned
    f32  reachCm  = -1.0f;   // skin reach: < 0 free, 0 hard skin
    u32  simIndex = kNone;   // index in the current Jolt body, written by the builder
    u32  origin   = 0;       // the particle this one was torn from (itself when original)
    bool pinned   = false;   // kinematic: skinned to its bind pose
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
    f32 peak     = 0.0f;     // largest strain seen (debug)
    f32 damage   = 0.0f;     // J accumulated by deposits
    u32 bend     = kNone;    // index into Cage::bends, or kNone
    u32 slot     = kNone;    // final Jolt edge index, written by the builder
    bool broken  = false;
};

// A dihedral bend across a shared edge: v[0..1] the edge, v[2] and v[3] the opposite corners (the
// Jolt DihedralBend layout).
struct Bend {
    u32 v[4]{0, 0, 0, 0};
    u32 beam = kNone;        // the shared edge's beam
    f32 compliance = 0.0f;
    u32 slot = kNone;        // final Jolt bend index, written by the builder
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
    std::vector<Bend>     bends;
    std::vector<Triangle> triangles;
    std::vector<u8>       triDead;       // 1 when a triangle spans a gap (>= 2 broken edges)
    std::vector<SplitEvent> splitLog;    // appended by tears; the owner drains it

    u32  baseParticleCount = 0;          // particle count at build(); duplicates come after
    bool built = false;
    bool topologyDirty = false;

    // Builder-only edge lookup, dropped by build().
    std::unordered_map<u64, u32> edgeIndex;
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

// Freezes the topology and captures the rest pose: rest lengths from the current positions, brittle
// classification, bends from the triangles. Call once, after building.
void build(Cage& c);
// Undo every plastic change, damage and tear: rest lengths, broken flags, duplicated particles,
// positions, bends.
void repair(Cage& c);

// Rebuilds `bends` from the live triangles and unbroken beams (4 distinct particles, two live
// triangles on the shared edge), keeping the compliance of the beam's material. Returns the count.
u32 rebuildBends(Cage& c);

// Particle mass from the live triangles: areaDensity x a third of each adjacent triangle's rest area
// (cm^2 -> m^2). A particle with no live triangle keeps its mass. Pinned particles get invMass 0.
void recomputeMasses(Cage& c);

// Marks a beam broken without recoil (scripted or editor cuts). Tears resolve with resolveTears().
void breakBeam(Cage& c, u32 beam);

// Splits particles whose triangle fans a broken beam has disconnected, duplicating them; updates
// freed flags and dead triangles. Returns the number of particles created.
u32 resolveTears(Cage& c);

// ---- queries -------------------------------------------------------------------------------------

// Connected components over unbroken beams. `ids` (optional) receives each particle's component.
u32 countPieces(const Cage& c, std::vector<u32>* ids = nullptr);
u32 brokenBeamCount(const Cage& c);
// Largest |length - rest0| / rest0 over unbroken beams.
f32 maxStrain(const Cage& c);

} // namespace aver::softbody
