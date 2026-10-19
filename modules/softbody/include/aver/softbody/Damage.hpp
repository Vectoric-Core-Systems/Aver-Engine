// Damage on positions Jolt produced: yield creep, snap, energy deposits and scripted cuts.
//
// No integration here. The host copies the simulated particle positions into the cage, calls these,
// and then pushes the changed rest lengths back into the body (dents) or rebuilds it (tears).
// Deterministic: the same cage and inputs give the same bits. See docs/SOFTBODY.md.
#pragma once

#include "aver/softbody/Cage.hpp"

#include <vector>

namespace aver::softbody {

struct DamageConfig {
    f32  dt = 1.0f / 60.0f;      // the interval since the last evaluation, bounds how fast a yielded beam creeps
    f32  creepRate = 8.0f;       // 1/s
    bool tearable = true;        // false (plain cloth): nothing snaps, only creep
};

struct DamageResult {
    u32 yielded = 0;                   // beams whose rest length changed
    u32 broken = 0;                    // beams snapped this call
    f32 maxStrain = 0.0f;              // largest |strain| over live beams
    std::vector<u32> dirtyBeams;       // rest changed
    std::vector<u32> brokenBeams;      // snapped, ascending
};

// Per live beam: strain = (len - rest) / rest0. Creeps `rest` past the yield strain (deforming
// materials), snaps past breakStrain, past the accumulated-set cap, or at the yield strain for
// brittle materials. Sets Cage::topologyDirty on a snap.
DamageResult evaluateStrain(Cage& c, const DamageConfig& cfg);

enum class DepositKind : u8 { Impact = 0, Explosion = 1, Gameplay = 2 };

struct DamageDeposit {
    Vec3 point{0, 0, 0};         // world cm
    Vec3 dir{0, 0, 0};           // the way the energy travels; zero = radial
    f32 radiusCm = 30.0f;
    f32 energyJ = 0.0f;
    u8 kind = 0;                 // DepositKind
};

struct DepositResult {
    f32 kickSpeedCmS = 0.0f;     // velocity to add to the particles in radius (aver_phys_sb_impulse)
    f32 massKg = 0.0f;           // mass of the movable particles in radius
    u32 beamsDamaged = 0;
    u32 broken = 0;
    std::vector<u32> brokenBeams;
};

// Spreads energyJ over the live beams by `beamWeight` (one per beam, 0 = untouched) in proportion to the weight and
// breaks a beam when its accumulated damage reaches the material's toughnessJ (brittle materials, or toughnessJ > 0).
// The one break rule behind impacts, explosions and fields. The kick is min(maxKick, sqrt(2 * efficiency * E / M) * 100)
// cm/s with M the movable mass of the particles on the weighted beams (pinned mass when nothing moves).
DepositResult depositWeighted(Cage& c, const std::vector<f32>& beamWeight, f32 energyJ, f32 impulseEfficiency, f32 maxKickCmS);

// The sphere case of depositWeighted: weights (1 - d/r)^2 on the beam midpoint within radius. The kick uses the mass of
// every particle in radius. `skipBeam` (optional, one byte per beam) leaves those beams out, for a baked piece.
DepositResult depositEnergy(Cage& c, const DamageDeposit& d, f32 impulseEfficiency, f32 maxKickCmS,
                            const std::vector<u8>* skipBeam = nullptr);

// A scripted cut: breaks the live beams whose midpoint is in radius. Returns how many.
u32 tearWithin(Cage& c, Vec3 point, f32 radiusCm, std::vector<u32>* brokenBeams = nullptr);

// Every dent, break, tear and accumulated damage undone.
void repairAll(Cage& c);

} // namespace aver::softbody
