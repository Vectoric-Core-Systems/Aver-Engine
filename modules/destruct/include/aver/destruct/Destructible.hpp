#pragma once
// Destructible: a pre-fractured mesh (.ocfrac, W7) as rigid bodies in the physics C ABI.
//
// Two modes. COMPOUND (default): the intact object is one body per connected component of the contact graph (a compound of the pieces' convex hulls),
// static when the component holds an anchor, dynamic otherwise. A hit adds strain to the links near it; a link whose strain passes its threshold breaks,
// and a component that falls in two becomes two bodies, each inheriting the parent's motion at its own centre of mass. JOINTED: every piece is its own
// body and every link a fixed joint with a break threshold, so the solver decides what gives way and a partial collapse happens by itself.
//
// Units are the engine's (cm, kg, +Z up); asset coordinates are scaled by `unitScale` and placed by origin and rotation. No scene, no GPU. Pieces keep their
// asset-space vertices, so a renderer draws piece i with piecePose(i) (translation, rotation, and the same unitScale).
#include "aver/core/HitEvent.hpp"
#include "aver/core/Types.hpp"
#include "aver/formats/OcFrac.hpp"

#include <memory>
#include <vector>

namespace aver::destruct {

enum class Mode { Compound = 0, Jointed = 1 };

struct DestructibleDesc {
    f32 originCm[3] = {0, 0, 0};
    f32 rotation[4] = {0, 0, 0, 1};        // x, y, z, w
    f32 unitScale = 1.0f;                  // asset units to centimetres
    f32 densityKgM3 = 2400.0f;             // mass = piece volume * density
    Mode mode = Mode::Compound;
    f32 toughness = 1.0f;                  // multiplies every link threshold
    f32 pointRadiusCm = 60.0f;             // reach of a point hit (a HitEvent with radius 0)
    f32 damageToImpulse = 100.0f;          // kg*cm/s per point of damage when an event carries no impulse
    f32 releaseGain = 0.5f;                // share of a hit's impulse handed to the debris it frees, falling off with distance
    f32 contactThreshold = 0.0f;           // applyContact ignores impulses below this
    i32 maxDebrisPieces = 64;              // pieces in freed bodies; the oldest are removed past this
    f32 sleepDespawnS = 0.0f;              // a freed body that sleeps this long is removed (0 = never)
    f32 jointForcePerThreshold = 1.0f;     // Jointed: newtons per unit of link threshold
    f32 jointTorqueArmM = 0.1f;            // Jointed: break torque = break force * this
    i32 entity = 0;                        // stamped on every body with aver_phys_set_entity when non-zero
};

struct BreakEvent {
    u32 link = 0;
    f32 pointCm[3] = {0, 0, 0};            // world position of the link
    f32 strain = 0;
};

struct HitResult {
    i32 linksBroken = 0;
    i32 bodiesCreated = 0;
    i32 piecesFreed = 0;                   // Compound: pieces that left the anchored mass. Jointed: pieces pushed
};

enum class PieceState : u8 { Attached = 0, Free = 1, Gone = 2 };

class Destructible {
public:
    // The asset must validate. Needs a live physics world; check valid() afterwards.
    Destructible(std::shared_ptr<const fmt::OcFracData> asset, const DestructibleDesc& desc);
    ~Destructible();
    Destructible(const Destructible&) = delete;
    Destructible& operator=(const Destructible&) = delete;

    bool valid() const { return valid_; }

    // The shared damage record (aver/core/HitEvent.hpp); world space, cm. Impulse magnitude, else damage, sets the strength; radius > 0 is an explosion.
    HitResult applyHit(const HitEvent& e);
    // A contact impulse (kg*cm/s) at a world point; ignored below DestructibleDesc::contactThreshold.
    HitResult applyContact(const f32 pointCm[3], f32 impulse);
    // Per frame: removes sleeping debris after sleepDespawnS, and in Jointed mode reads the joints that broke.
    void update(f32 dt);

    u32 pieceCount() const { return static_cast<u32>(state_.size()); }
    u32 linkCount() const { return static_cast<u32>(broken_.size()); }
    bool linkBroken(u32 link) const { return link < broken_.size() && broken_[link]; }
    f32 linkStrain(u32 link) const { return link < strain_.size() ? strain_[link] : 0.0f; }
    PieceState pieceState(u32 piece) const { return piece < state_.size() ? static_cast<PieceState>(state_[piece]) : PieceState::Gone; }
    // The physics body carrying the piece (0 once it is gone). Compound mode: pieces of one component share a body.
    i32 pieceBody(u32 piece) const;
    // World transform to draw the piece with: the body origin and rotation (asset vertices x unitScale go through it). False once gone.
    bool piecePose(u32 piece, f32 outPosCm[3], f32 outQuat[4]) const;
    u32 bodyCount() const;
    u32 debrisPieceCount() const;
    u32 componentCount() const;           // over the links not yet broken, gone pieces excluded

    std::vector<BreakEvent> takeEvents();
    // Jointed mode reads the physics break queue without clearing it (several destructibles may share it); the owner clears it once per frame.
    static void clearBreakQueue();

private:
    struct Group {
        i32 body = 0;
        std::vector<u32> pieces;
        bool anchored = false, debris = false;
        u64 serial = 0;
        f32 idle = 0;
    };
    struct Strike {
        f32 p[3] = {0, 0, 0};              // world point, cm
        f32 dir[3] = {0, 0, 0};            // unit push direction; zero = away from the point
        f32 magnitude = 0;                 // kg*cm/s
        f32 radius = 0;                    // cm
    };
    HitResult strike(const Strike& s);
    i32 pieceShape(u32 piece);
    void linkWorld(u32 link, f32 out[3]) const;
    void groupPose(const Group& g, f32 pos[3], f32 quat[4]) const;
    void markStates();
    void removePiece(u32 piece);
    i32 makeGroup(const std::vector<u32>& pieces, bool anchored, bool debris, const f32 origin[3], const f32 rot[4]);
    void removeGroup(i32 index);
    void resplit(const Strike& s, HitResult& r);
    void enforceDebrisCap();
    void toWorld(const f32 local[3], f32 out[3]) const;

    std::shared_ptr<const fmt::OcFracData> asset_;
    DestructibleDesc desc_;
    bool valid_ = false;
    std::vector<u8> broken_, state_;
    std::vector<f32> strain_;
    std::vector<i32> shape_;               // pooled hull per piece, built on first use
    std::vector<i32> groupOf_;
    std::vector<i32> joint_;               // Jointed: joint per link (0 = none)
    std::vector<Group> groups_;
    std::vector<i32> freeSlots_;
    std::vector<f32> linkCm_;              // asset-space link centres (3 per link), unscaled
    std::vector<BreakEvent> events_;
    u64 serial_ = 0;
};

}  // namespace aver::destruct
