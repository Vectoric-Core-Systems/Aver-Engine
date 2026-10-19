// The camera modifier stack: FOV kick, zoom, punch, roll lean, arm scale and socket offset, each with a priority and an
// in / hold / out envelope. Pure (Aver.Core only): the system evaluates it once per frame and applies the result.
//
// Shake is NOT here: a shake adds trauma to the global CameraFx, which the host applies last with the player's scales.
#pragma once

#include "aver/camera/CameraRigMath.hpp"

#include <array>

namespace aver::camera {

inline constexpr u32 kModExclusive = 1u << 0;     // while at full strength, lower-priority modifiers of the same kind add nothing
inline constexpr u32 kModMotion = 1u << 1;        // gated by Reduce motion even if the kind is not by default
inline constexpr u32 kModNoMotionGate = 1u << 2;  // not gated even if the kind is by default
inline constexpr u32 kMaxModifiers = 32;

struct CameraModifier {
    u32 id = 0;                       // assigned by push()
    ModKind kind = ModKind::FovKick;
    i32 priority = 0;
    f32 inS = 0, holdS = 0, outS = 0; // holdS < 0 = until removed
    f32 a = 0, b = 0, c = 0, d = 0;   // kind parameters (see the table in docs/CAMERAS.md)
    u32 tag = 0;
    u32 flags = 0;
};

// The summed effect of the stack, in radians where angular.
struct ModResult {
    f32 fovAddRad = 0, fovScale = 1, armScale = 1;
    Vec3 socketAdd;
    f32 punchPitch = 0, punchYaw = 0, roll = 0;
};

// Motion-gated by default: FovKick, Punch, RollLean (they move the camera on their own). FovScale (aim), ArmScale and
// SocketOffset are the player's own action.
inline bool modifierMotionGated(const CameraModifier& m) {
    if (m.flags & kModMotion) return true;
    if (m.flags & kModNoMotionGate) return false;
    return m.kind == ModKind::FovKick || m.kind == ModKind::Punch || m.kind == ModKind::RollLean;
}

class ModifierStack {
public:
    // Returns a never-reused id (a counter advanced in call order), or 0 when the stack is full.
    u32 push(const CameraModifier& m) {
        if (count_ >= kMaxModifiers) return 0;
        Entry& e = entries_[count_++];
        e = Entry{};
        e.mod = m;
        e.mod.id = ++nextId_;
        e.phase = e.mod.inS > 0.0f ? Phase::In : Phase::Hold;
        e.alpha = e.phase == Phase::Hold ? 1.0f : 0.0f;
        return e.mod.id;
    }

    // Starts the blend out of one modifier from its CURRENT strength. outS < 0 = the modifier's own.
    bool remove(u32 id, f32 outS) {
        Entry* e = find(id);
        if (!e) return false;
        beginOut(*e, outS, true);
        return true;
    }

    // Blends out every modifier with this tag (0 = all), each over its own outS. Returns how many.
    u32 clear(u32 tag) {
        u32 n = 0;
        for (u32 i = 0; i < count_; ++i) {
            if (tag != 0 && entries_[i].mod.tag != tag) continue;
            beginOut(entries_[i], -1.0f, false);
            ++n;
        }
        return n;
    }

    // Advances every envelope by dt, drops finished ones, and sums the rest in (priority, id) order.
    ModResult evaluate(f32 dt, bool motionAllowed) {
        if (dt < 0.0f) dt = 0.0f;
        for (u32 i = 0; i < count_; ++i) advance(entries_[i], dt);
        // Drop finished (stable, keeps insertion order).
        u32 w = 0;
        for (u32 i = 0; i < count_; ++i)
            if (!entries_[i].done) { if (w != i) entries_[w] = entries_[i]; ++w; }
        count_ = w;

        std::array<u32, kMaxModifiers> order{};
        for (u32 i = 0; i < count_; ++i) order[i] = i;
        for (u32 i = 1; i < count_; ++i) {
            const u32 v = order[i];
            u32 j = i;
            while (j > 0 && before(entries_[v], entries_[order[j - 1]])) { order[j] = order[j - 1]; --j; }
            order[j] = v;
        }

        // An exclusive modifier at full strength silences lower-priority ones of its kind.
        std::array<i32, kModKindCount> floorPri{};
        std::array<bool, kModKindCount> hasFloor{};
        for (u32 i = 0; i < count_; ++i) {
            const Entry& e = entries_[i];
            if (!(e.mod.flags & kModExclusive) || e.alpha < 0.99f) continue;
            const u32 k = static_cast<u32>(e.mod.kind);
            if (k >= kModKindCount) continue;
            if (!hasFloor[k] || e.mod.priority > floorPri[k]) { floorPri[k] = e.mod.priority; hasFloor[k] = true; }
        }

        ModResult r;
        for (u32 oi = 0; oi < count_; ++oi) {
            const Entry& e = entries_[order[oi]];
            const u32 k = static_cast<u32>(e.mod.kind);
            if (k >= kModKindCount) continue;
            if (hasFloor[k] && e.mod.priority < floorPri[k]) continue;
            if (!motionAllowed && modifierMotionGated(e.mod)) continue;
            const f32 a = e.alpha;
            const CameraModifier& m = e.mod;
            switch (m.kind) {
                case ModKind::FovKick: r.fovAddRad += radians(m.a) * a; break;
                case ModKind::FovScale: r.fovScale *= 1.0f + (m.a - 1.0f) * a; break;
                case ModKind::Punch:
                    r.punchPitch += radians(m.a) * a;
                    r.punchYaw += radians(m.b) * a;
                    r.roll += radians(m.c) * a;
                    break;
                case ModKind::RollLean: r.roll += radians(m.a) * a; break;
                case ModKind::ArmScale: r.armScale *= 1.0f + (m.a - 1.0f) * a; break;
                case ModKind::SocketOffset: r.socketAdd += Vec3{m.a, m.b, m.c} * a; break;
            }
        }
        return r;
    }

    u32 count() const { return count_; }
    // The strength (0..1) of one modifier, -1 when it is not on the stack.
    f32 alphaOf(u32 id) const {
        for (u32 i = 0; i < count_; ++i) if (entries_[i].mod.id == id) return entries_[i].alpha;
        return -1.0f;
    }
    // Drops everything and restarts the id counter (Play stop).
    void reset() { count_ = 0; nextId_ = 0; }

private:
    enum class Phase : u8 { In, Hold, Out };
    struct Entry {
        CameraModifier mod;
        Phase phase = Phase::In;
        f32 t = 0;            // seconds into the current phase
        f32 alpha = 0;
        f32 outFrom = 1;      // strength when the out phase began
        f32 outDur = 0;
        bool done = false;
    };

    static f32 smooth(f32 x) { x = clamp01(x); return x * x * (3.0f - 2.0f * x); }

    static bool before(const Entry& a, const Entry& b) {
        return a.mod.priority != b.mod.priority ? a.mod.priority < b.mod.priority : a.mod.id < b.mod.id;
    }

    Entry* find(u32 id) {
        for (u32 i = 0; i < count_; ++i) if (entries_[i].mod.id == id) return &entries_[i];
        return nullptr;
    }

    // An entry already blending out keeps its schedule unless `restart` (an explicit remove) asks for a new one from the
    // current strength.
    static void beginOut(Entry& e, f32 outS, bool restart) {
        if (e.phase == Phase::Out && !restart) return;
        e.phase = Phase::Out;
        e.t = 0;
        e.outFrom = e.alpha;
        e.outDur = outS < 0.0f ? e.mod.outS : outS;
        if (e.outDur < 0.0f) e.outDur = 0.0f;
    }

    // Moves the envelope by dt, carrying leftover time across phase boundaries.
    static void advance(Entry& e, f32 dt) {
        f32 left = dt;
        for (int guard = 0; guard < 4; ++guard) {
            if (e.phase == Phase::In) {
                const f32 need = e.mod.inS - e.t;
                if (left < need) { e.t += left; e.alpha = smooth(e.t / e.mod.inS); return; }
                left -= need > 0.0f ? need : 0.0f;
                e.phase = Phase::Hold;
                e.t = 0;
                e.alpha = 1.0f;
            } else if (e.phase == Phase::Hold) {
                if (e.mod.holdS < 0.0f) { e.alpha = 1.0f; return; }
                const f32 need = e.mod.holdS - e.t;
                if (left < need) { e.t += left; e.alpha = 1.0f; return; }
                left -= need > 0.0f ? need : 0.0f;
                e.phase = Phase::Out;
                e.t = 0;
                e.outFrom = 1.0f;
                e.outDur = e.mod.outS > 0.0f ? e.mod.outS : 0.0f;
            } else {
                const f32 need = e.outDur - e.t;
                if (e.outDur <= 0.0f || left >= need) { e.alpha = 0.0f; e.done = true; return; }
                e.t += left;
                e.alpha = e.outFrom * (1.0f - smooth(e.t / e.outDur));
                return;
            }
        }
    }

    std::array<Entry, kMaxModifiers> entries_{};
    u32 count_ = 0, nextId_ = 0;
};

}  // namespace aver::camera
