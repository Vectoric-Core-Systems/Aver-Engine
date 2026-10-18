#pragma once
// Camera feel: trauma-driven shake and a speed-driven head bob, so "reduce motion" gates something real.
// Header-only, deterministic (seeded value noise, no wall clock), no allocation. Nothing calls it by itself: a
// character script calls AddShake / SetBob, and the host applies it right before pushCamera (GameApp, SandboxApp
// play). docs/ACCESSIBILITY.md section 2.5. Trauma model: Squirrel Eiserloh, GDC 2016.

#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"

#include <cmath>

namespace aver::fw {

class CameraFx {
public:
    static constexpr f32 kDecayPerSec = 1.5f;
    static constexpr f32 kNoiseHz = 17.0f;
    static constexpr f32 kYawDeg = 1.5f, kPitchDeg = 1.2f, kOffsetCm = 1.5f;
    static constexpr f32 kBobVertCm = 2.2f, kBobLatCm = 1.1f, kBobCmPerCycle = 320.0f;

    // Adds trauma; saturates at 1.
    void addShake(f32 trauma) {
        if (!(trauma > 0)) return;
        trauma_ += trauma;
        if (trauma_ > 1.0f) trauma_ = 1.0f;
    }
    // Call each frame while moving; amount 0 or speed 0 stops it.
    void setBob(f32 amount01, f32 speedCmPerSec) {
        amount_ = amount01 < 0 ? 0 : (amount01 > 1 ? 1 : amount01);
        speed_ = speedCmPerSec < 0 ? 0 : speedCmPerSec;
    }
    void setSeed(u32 s) { seed_ = s; }
    void reset() { *this = CameraFx{}; }

    f32 trauma() const { return trauma_; }
    f32 phase() const { return phase_; }
    f32 bobAmount() const { return amount_; }

    // Offsets pos / yaw / pitch (radians) in place. Both scales come from "reduce ? 0 : slider". With both zero it
    // returns false before touching any state, so a game that never uses it is bit-identical.
    bool apply(Vec3& pos, f32& yaw, f32& pitch, f32 dt, f32 shakeScale, f32 bobScale) {
        const bool shakeOn = shakeScale > 0, bobOn = bobScale > 0;
        if (!shakeOn && !bobOn) return false;
        if (!(dt > 0)) dt = 0;
        time_ += dt;
        bool touched = false;

        const f32 baseYaw = yaw;
        if (shakeOn && trauma_ > 0) {
            const f32 k = trauma_ * trauma_ * (shakeScale > 1 ? 1.0f : shakeScale);
            const f32 t = time_ * kNoiseHz;
            yaw   += radians(kYawDeg)   * k * noise(t, seed_ + 1);
            pitch += radians(kPitchDeg) * k * noise(t, seed_ + 2);
            pos.x += kOffsetCm * k * noise(t, seed_ + 3);
            pos.y += kOffsetCm * k * noise(t, seed_ + 4);
            pos.z += kOffsetCm * k * noise(t, seed_ + 5);
            trauma_ -= kDecayPerSec * dt;
            if (trauma_ < 0) trauma_ = 0;
            touched = true;
        }
        if (bobOn && amount_ > 0 && speed_ > 0) {
            phase_ += speed_ / kBobCmPerCycle * dt;
            phase_ -= std::floor(phase_);
            const f32 a = amount_ * (bobScale > 1 ? 1.0f : bobScale);
            const f32 two = 6.28318530718f;
            const f32 vert = std::sin(two * phase_) * kBobVertCm * a;
            const f32 lat = std::sin(0.5f * two * phase_) * kBobLatCm * a;
            pos.z += vert;
            pos.x += -std::sin(baseYaw) * lat;   // right vector: +Y at yaw 0
            pos.y += std::cos(baseYaw) * lat;
            touched = true;
        }
        return touched;
    }

private:
    static f32 hash(u32 n) {
        n = n * 747796405u + 2891336453u;
        n = ((n >> ((n >> 28u) + 4u)) ^ n) * 277803737u;
        n = (n >> 22u) ^ n;
        return static_cast<f32>(n) * (2.0f / 4294967295.0f) - 1.0f;
    }
    static f32 value(f32 x, u32 seed) {
        const f32 fl = std::floor(x);
        const f32 f = x - fl;
        const u32 i = static_cast<u32>(static_cast<i32>(fl));
        const f32 s = f * f * (3.0f - 2.0f * f);
        const f32 a = hash(i * 1013904223u + seed * 2654435761u), b = hash((i + 1) * 1013904223u + seed * 2654435761u);
        return a + (b - a) * s;
    }
    // Two octaves, normalised to [-1, 1].
    static f32 noise(f32 t, u32 seed) { return (value(t, seed) + 0.5f * value(t * 2.0f, seed + 101u)) * (1.0f / 1.5f); }

    f32 trauma_ = 0, amount_ = 0, speed_ = 0, phase_ = 0, time_ = 0;
    u32 seed_ = 0x9E3779B9u;
};

} // namespace aver::fw
