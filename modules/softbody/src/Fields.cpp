// Field shapes, weights and velocity changes. See Fields.hpp.
#include "aver/softbody/Fields.hpp"

#include <algorithm>
#include <cmath>

namespace aver::softbody {
namespace {

Vec3 xformPoint(const Vec3& p, const Mat4& m) {
    return {p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0],
            p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1],
            p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2]};
}

Vec3 xformDir(const Vec3& v, const Mat4& m) {
    return {v.x * m.m[0][0] + v.y * m.m[1][0] + v.z * m.m[2][0],
            v.x * m.m[0][1] + v.y * m.m[1][1] + v.z * m.m[2][1],
            v.x * m.m[0][2] + v.y * m.m[1][2] + v.z * m.m[2][2]};
}

// An integer hash of a 100 cm cell and a step window, the aero's noise: no clock, no rand.
u32 hashCell(i32 cx, i32 cy, i32 cz, u32 window, u32 salt) {
    u32 h = static_cast<u32>(cx) * 0x8DA6B343u ^ static_cast<u32>(cy) * 0xD8163841u ^ static_cast<u32>(cz) * 0xCB1AB31Fu ^
            window * 0x9E3779B1u ^ salt * 0x85EBCA6Bu;
    h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
    return h;
}

f32 noise(i32 cx, i32 cy, i32 cz, u32 window, u32 salt) {
    return static_cast<f32>(hashCell(cx, cy, cz, window, salt) & 0xFFFFFFu) / 8388608.0f - 1.0f;   // [-1, 1)
}

} // namespace

Mat4 fieldFrameFrom(const Vec3& centre, const Vec3& yawPitchRollDeg) {
    // The placement convention: Rz(yaw) * Ry(pitch) * Rx(roll).
    const Quat q = (Quat::fromAxisAngle({0, 0, 1}, radians(yawPitchRollDeg.x)) * Quat::fromAxisAngle({0, 1, 0}, radians(yawPitchRollDeg.y)) *
                    Quat::fromAxisAngle({1, 0, 0}, radians(yawPitchRollDeg.z))).normalized();
    Mat4 m = Mat4::fromQuat(q);
    m.m[3][0] = centre.x; m.m[3][1] = centre.y; m.m[3][2] = centre.z;
    return m;
}

FieldFrame makeFieldFrame(const FieldSpec& spec) {
    FieldFrame f;
    f.spec = spec;
    f.inverse = spec.world.inverse();
    f.centre = Vec3{spec.world.m[3][0], spec.world.m[3][1], spec.world.m[3][2]};
    f.dirWorld = spec.dir.sizeSquared() > 1e-12f ? xformDir(spec.dir, spec.world).getSafeNormal() : Vec3{0, 0, 0};
    return f;
}

f32 fieldDistance(const FieldFrame& f, const Vec3& worldPos) {
    const FieldSpec& s = f.spec;
    if (s.shape == FieldShape::Everywhere) return 0.0f;
    const Vec3 p = xformPoint(worldPos, f.inverse);
    switch (s.shape) {
        case FieldShape::Sphere:
            return p.size() / std::max(s.extents.x, 1e-4f);
        case FieldShape::Box:
            return std::max({std::fabs(p.x) / std::max(s.extents.x, 1e-4f), std::fabs(p.y) / std::max(s.extents.y, 1e-4f),
                             std::fabs(p.z) / std::max(s.extents.z, 1e-4f)});
        case FieldShape::Capsule: {
            const f32 dz = std::max(std::fabs(p.z) - std::max(s.extents.z, 0.0f), 0.0f);
            return std::sqrt(p.x * p.x + p.y * p.y + dz * dz) / std::max(s.extents.x, 1e-4f);
        }
        default: return 0.0f;
    }
}

f32 fieldWeight(const FieldFrame& f, const Vec3& worldPos) {
    const f32 n = fieldDistance(f, worldPos);
    if (!(n < 1.0f)) return 0.0f;
    return std::pow(1.0f - n, std::max(f.spec.falloff, 0.0f));
}

f32 fieldWeight(const FieldSpec& s, const Vec3& worldPos) { return fieldWeight(makeFieldFrame(s), worldPos); }

Vec3 fieldVelocityDelta(const FieldFrame& f, const Vec3& worldPos, f32 weight, f32 seconds, u64 step) {
    const FieldSpec& s = f.spec;
    if (!(weight > 0.0f)) return Vec3{0, 0, 0};
    Vec3 dir;
    if (s.kind == FieldKind::RadialForce) {
        const Vec3 rel = worldPos - f.centre;
        dir = rel.sizeSquared() > 1e-8f ? rel.getSafeNormal() : Vec3{0, 0, 1};
    } else if (s.kind == FieldKind::DirectionalForce) {
        dir = f.dirWorld.sizeSquared() > 0.5f ? f.dirWorld : Vec3{0, 0, 1};
    } else {
        return Vec3{0, 0, 0};
    }
    f32 mag = s.strength * weight * (s.perSecond ? seconds : 1.0f);
    const f32 t = std::clamp(s.turbulence, 0.0f, 1.0f);
    if (t > 0.0f) {
        const i32 cx = static_cast<i32>(std::floor(worldPos.x / 100.0f));
        const i32 cy = static_cast<i32>(std::floor(worldPos.y / 100.0f));
        const i32 cz = static_cast<i32>(std::floor(worldPos.z / 100.0f));
        const u32 window = static_cast<u32>(step / 4);
        mag *= 1.0f + t * noise(cx, cy, cz, window, 0);
        const Vec3 jitter{noise(cx, cy, cz, window, 1), noise(cx, cy, cz, window, 2), noise(cx, cy, cz, window, 3)};
        const Vec3 bent = (dir + jitter * (t * 0.5f)).getSafeNormal();
        if (bent.sizeSquared() > 0.5f) dir = bent;
    }
    return dir * mag;
}

Vec3 fieldVelocityDelta(const FieldSpec& s, const Vec3& worldPos, f32 weight, f32 seconds, u64 step) {
    return fieldVelocityDelta(makeFieldFrame(s), worldPos, weight, seconds, step);
}

bool fieldOverlapsBounds(const FieldSpec& s, const Vec3& lo, const Vec3& hi) {
    if (s.shape == FieldShape::Everywhere) return true;
    const Mat4 inv = s.world.inverse();
    Vec3 llo{1e30f, 1e30f, 1e30f}, lhi{-1e30f, -1e30f, -1e30f};
    for (u32 c = 0; c < 8; ++c) {
        const Vec3 corner{(c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z};
        const Vec3 p = xformPoint(corner, inv);
        llo = {std::min(llo.x, p.x), std::min(llo.y, p.y), std::min(llo.z, p.z)};
        lhi = {std::max(lhi.x, p.x), std::max(lhi.y, p.y), std::max(lhi.z, p.z)};
    }
    // Distance from the local origin to a local interval, 0 inside it.
    auto gap = [](f32 a, f32 b) { return a > 0.0f ? a : (b < 0.0f ? -b : 0.0f); };
    switch (s.shape) {
        case FieldShape::Sphere: {
            const f32 gx = gap(llo.x, lhi.x), gy = gap(llo.y, lhi.y), gz = gap(llo.z, lhi.z);
            return gx * gx + gy * gy + gz * gz < s.extents.x * s.extents.x;
        }
        case FieldShape::Box:
            return llo.x < s.extents.x && lhi.x > -s.extents.x && llo.y < s.extents.y && lhi.y > -s.extents.y &&
                   llo.z < s.extents.z && lhi.z > -s.extents.z;
        case FieldShape::Capsule: {
            const f32 gx = gap(llo.x, lhi.x), gy = gap(llo.y, lhi.y);
            // The axis segment is [-hz, hz]; the interval's gap to it is zero when they overlap.
            const f32 hz = std::max(s.extents.z, 0.0f);
            const f32 gz = lhi.z < -hz ? (-hz - lhi.z) : (llo.z > hz ? (llo.z - hz) : 0.0f);
            return gx * gx + gy * gy + gz * gz < s.extents.x * s.extents.x;
        }
        default: return true;
    }
}

void fieldBeamWeights(const Cage& c, const FieldSpec& s, std::vector<f32>& out) {
    const FieldFrame f = makeFieldFrame(s);
    out.assign(c.beams.size(), 0.0f);
    for (u32 bi = 0; bi < c.beams.size(); ++bi) {
        const Beam& b = c.beams[bi];
        if (b.broken) continue;
        out[bi] = fieldWeight(f, (c.particles[b.a].pos + c.particles[b.b].pos) * 0.5f);
    }
}

u32 anchorsInField(const Cage& c, const FieldSpec& s, std::vector<u32>& particles) {
    const FieldFrame f = makeFieldFrame(s);
    particles.clear();
    for (u32 i = 0; i < c.particles.size(); ++i) {
        const Particle& p = c.particles[i];
        if (!p.pinned) continue;
        if (fieldDistance(f, p.pos) < 1.0f) particles.push_back(i);
    }
    return static_cast<u32>(particles.size());
}

const char* fieldKindKey(FieldKind k) {
    switch (k) {
        case FieldKind::RadialForce:      return "radial";
        case FieldKind::DirectionalForce: return "directional";
        case FieldKind::Damage:           return "damage";
        case FieldKind::ReleaseAnchors:   return "release";
        case FieldKind::Kill:             return "kill";
        default:                          return "sleep";
    }
}

bool fieldKindFromKey(std::string_view key, FieldKind& out) {
    for (u32 i = 0; i < kFieldKindCount; ++i)
        if (key == fieldKindKey(static_cast<FieldKind>(i))) { out = static_cast<FieldKind>(i); return true; }
    return false;
}

const char* fieldShapeKey(FieldShape s) {
    switch (s) {
        case FieldShape::Sphere:  return "sphere";
        case FieldShape::Box:     return "box";
        case FieldShape::Capsule: return "capsule";
        default:                  return "all";
    }
}

bool fieldShapeFromKey(std::string_view key, FieldShape& out) {
    for (u32 i = 0; i < kFieldShapeCount; ++i)
        if (key == fieldShapeKey(static_cast<FieldShape>(i))) { out = static_cast<FieldShape>(i); return true; }
    return false;
}

bool fieldTargetsFromText(std::string_view text, u32& mask) {
    u32 m = 0;
    usize i = 0;
    while (i <= text.size()) {
        usize j = text.find(',', i);
        if (j == std::string_view::npos) j = text.size();
        std::string_view w = text.substr(i, j - i);
        while (!w.empty() && w.front() == ' ') w.remove_prefix(1);
        while (!w.empty() && w.back() == ' ') w.remove_suffix(1);
        if (w == "cloth") m |= kFieldTargetCloth;
        else if (w == "destructible") m |= kFieldTargetDestructible;
        else if (w == "debris") m |= kFieldTargetDebris;
        else return false;
        i = j + 1;
    }
    mask = m;
    return m != 0;
}

std::string fieldTargetsToText(u32 mask) {
    std::string out;
    const auto add = [&](u32 bit, const char* word) {
        if (!(mask & bit)) return;
        if (!out.empty()) out += ',';
        out += word;
    };
    add(kFieldTargetCloth, "cloth");
    add(kFieldTargetDestructible, "destructible");
    add(kFieldTargetDebris, "debris");
    return out;
}

} // namespace aver::softbody
