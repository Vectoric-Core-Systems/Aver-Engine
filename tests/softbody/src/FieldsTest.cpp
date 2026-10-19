// Soft-body fields: shape weights, velocity changes, the broad phase, beam weights and anchors.
// Pure arithmetic; no GPU, no scene, no Jolt.
#include "aver/core/Log.hpp"
#include "aver/softbody/Fields.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace aver;
namespace sb = aver::softbody;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool approx(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }
static bool approx(const Vec3& a, const Vec3& b, f32 eps = 1e-3f) { return dist(a, b) <= eps; }

static sb::FieldSpec specAt(sb::FieldShape shape, Vec3 centre, Vec3 extents) {
    sb::FieldSpec s;
    s.shape = shape;
    s.world = Mat4::translation(centre);
    s.extents = extents;
    return s;
}

int main() {
    AVER_INFO("FieldsTest");

    AVER_INFO("the four shapes: 1 on the centre line, 0 outside");
    {
        const sb::FieldSpec sphere = specAt(sb::FieldShape::Sphere, Vec3{100, 0, 0}, Vec3{50, 0, 0});
        check(approx(sb::fieldWeight(sphere, Vec3{100, 0, 0}), 1.0f), "sphere: 1 at the centre");
        check(approx(sb::fieldWeight(sphere, Vec3{125, 0, 0}), 0.5f), "sphere: 0.5 halfway at falloff 1");
        check(sb::fieldWeight(sphere, Vec3{151, 0, 0}) == 0.0f && sb::fieldWeight(sphere, Vec3{100, 0, 60}) == 0.0f, "sphere: 0 outside");

        const sb::FieldSpec box = specAt(sb::FieldShape::Box, Vec3{0, 0, 0}, Vec3{100, 50, 20});
        check(approx(sb::fieldWeight(box, Vec3{0, 0, 0}), 1.0f), "box: 1 at the centre");
        check(approx(sb::fieldWeight(box, Vec3{50, 0, 0}), 0.5f) && approx(sb::fieldWeight(box, Vec3{0, 25, 0}), 0.5f) && approx(sb::fieldWeight(box, Vec3{0, 0, 10}), 0.5f),
              "box: the distance is the largest normalised axis");
        check(sb::fieldWeight(box, Vec3{101, 0, 0}) == 0.0f && sb::fieldWeight(box, Vec3{0, 0, 21}) == 0.0f, "box: 0 outside");

        const sb::FieldSpec cap = specAt(sb::FieldShape::Capsule, Vec3{0, 0, 0}, Vec3{30, 0, 100});
        check(approx(sb::fieldWeight(cap, Vec3{0, 0, 80}), 1.0f), "capsule: 1 along the axis segment");
        check(approx(sb::fieldWeight(cap, Vec3{15, 0, 0}), 0.5f), "capsule: radial falloff off the axis");
        check(approx(sb::fieldWeight(cap, Vec3{0, 0, 115}), 0.5f), "capsule: the end cap is a hemisphere past the segment");
        check(sb::fieldWeight(cap, Vec3{0, 0, 131}) == 0.0f && sb::fieldWeight(cap, Vec3{31, 0, 0}) == 0.0f, "capsule: 0 outside");

        sb::FieldSpec all;
        all.shape = sb::FieldShape::Everywhere;
        check(approx(sb::fieldWeight(all, Vec3{1e6f, -3e5f, 7.0f}), 1.0f), "everywhere: 1 anywhere");
    }

    AVER_INFO("falloff is monotone");
    {
        sb::FieldSpec s = specAt(sb::FieldShape::Sphere, Vec3{0, 0, 0}, Vec3{100, 0, 0});
        const Vec3 p{50, 0, 0};
        s.falloff = 0.0f; const f32 w0 = sb::fieldWeight(s, p);
        s.falloff = 1.0f; const f32 w1 = sb::fieldWeight(s, p);
        s.falloff = 2.0f; const f32 w2 = sb::fieldWeight(s, p);
        s.falloff = 4.0f; const f32 w4 = sb::fieldWeight(s, p);
        check(approx(w0, 1.0f) && w0 > w1 && w1 > w2 && w2 > w4 && w4 > 0.0f, "a higher falloff only lowers the weight inside");
        s.falloff = 3.0f;
        check(sb::fieldWeight(s, Vec3{30, 0, 0}) > sb::fieldWeight(s, Vec3{60, 0, 0}), "and the weight falls with distance");
    }

    AVER_INFO("a rotated, non-uniformly scaled box");
    {
        // 90 degrees about +Z (row vectors: x -> y), then scale (2, 1, 1), then a translation: a 100 x 50 x 20 box whose long
        // axis points along world +Y after the rotation and is doubled by the scale.
        const Mat4 world = Mat4::scale(Vec3{2, 1, 1}) * Mat4::fromQuat(Quat::fromAxisAngle(Vec3{0, 0, 1}, kPi * 0.5f)) * Mat4::translation(Vec3{10, 20, 30});
        sb::FieldSpec s;
        s.shape = sb::FieldShape::Box;
        s.world = world;
        s.extents = Vec3{50, 25, 10};
        s.falloff = 1.0f;
        check(approx(sb::fieldWeight(s, Vec3{10, 20, 30}), 1.0f), "the centre is the translation row");
        // Local x (scaled by 2) maps to world y: half extent 50 -> 100 cm of reach along world +Y.
        check(approx(sb::fieldWeight(s, Vec3{10, 70, 30}), 0.5f, 1e-3f), "along the rotated, doubled axis: 50 cm of 100 is half weight");
        check(sb::fieldWeight(s, Vec3{10, 125, 30}) == 0.0f, "and 0 past 100 cm");
        // Local y (half extent 25, unscaled) maps to world -x: 25 cm of reach.
        check(approx(sb::fieldWeight(s, Vec3{22.5f, 20, 30}), 0.5f, 1e-3f), "across it: 12.5 cm of 25 is half weight");
        check(sb::fieldWeight(s, Vec3{40, 20, 30}) == 0.0f, "and 0 past 25 cm");
    }

    AVER_INFO("radial and directional velocity changes");
    {
        sb::FieldSpec s = specAt(sb::FieldShape::Sphere, Vec3{0, 0, 0}, Vec3{100, 0, 0});
        s.kind = sb::FieldKind::RadialForce;
        s.strength = 600.0f;
        s.falloff = 1.0f;
        const Vec3 p{40, 0, 0};
        const f32 w = sb::fieldWeight(s, p);
        Vec3 dv = sb::fieldVelocityDelta(s, p, w, 0.016f, 1);
        check(approx(dv, Vec3{600.0f * w, 0, 0}), "radial pushes away from the centre by strength x weight");
        s.strength = -600.0f;
        dv = sb::fieldVelocityDelta(s, p, w, 0.016f, 1);
        check(dv.x < 0.0f && approx(dv.y, 0.0f) && approx(dv.z, 0.0f), "a negative strength pulls");
        s.strength = 600.0f;
        dv = sb::fieldVelocityDelta(s, Vec3{0, 0, 0}, 1.0f, 0.016f, 1);
        check(approx(dv, Vec3{0, 0, 600.0f}), "at the exact centre a radial field uses +Z");

        sb::FieldSpec d = specAt(sb::FieldShape::Everywhere, Vec3{0, 0, 0}, Vec3{1, 1, 1});
        d.kind = sb::FieldKind::DirectionalForce;
        d.dir = Vec3{1, 0, 0};
        d.strength = 300.0f;
        check(approx(sb::fieldVelocityDelta(d, Vec3{5, 5, 5}, 1.0f, 0.016f, 1), Vec3{300, 0, 0}), "directional follows its local axis");
        d.world = Mat4::fromQuat(Quat::fromAxisAngle(Vec3{0, 0, 1}, kPi * 0.5f));
        check(approx(sb::fieldVelocityDelta(d, Vec3{5, 5, 5}, 1.0f, 0.016f, 1), Vec3{0, 300, 0}), "and the entity's rotation: x turned onto y");
        d.dir = Vec3{0, 0, 0};
        check(approx(sb::fieldVelocityDelta(d, Vec3{5, 5, 5}, 1.0f, 0.016f, 1), Vec3{0, 0, 300}), "a zero axis reads +Z");

        d.kind = sb::FieldKind::Damage;
        check(approx(sb::fieldVelocityDelta(d, Vec3{5, 5, 5}, 1.0f, 0.016f, 1), Vec3{0, 0, 0}), "other kinds change no velocity");
    }

    AVER_INFO("per second scales with the step time");
    {
        sb::FieldSpec s = specAt(sb::FieldShape::Everywhere, Vec3{0, 0, 0}, Vec3{1, 1, 1});
        s.kind = sb::FieldKind::DirectionalForce;
        s.dir = Vec3{0, 0, 1};
        s.strength = 120.0f;
        s.perSecond = true;
        check(approx(sb::fieldVelocityDelta(s, Vec3{0, 0, 0}, 1.0f, 0.5f, 3).z, 60.0f), "a rate over half a second");
        check(approx(sb::fieldVelocityDelta(s, Vec3{0, 0, 0}, 1.0f, 1.0f / 60.0f, 3).z, 2.0f), "and over one step");
        s.perSecond = false;
        check(approx(sb::fieldVelocityDelta(s, Vec3{0, 0, 0}, 1.0f, 0.5f, 3).z, 120.0f), "a pulse ignores the time");
    }

    AVER_INFO("turbulence is deterministic and absent at 0");
    {
        sb::FieldSpec s = specAt(sb::FieldShape::Everywhere, Vec3{0, 0, 0}, Vec3{1, 1, 1});
        s.kind = sb::FieldKind::DirectionalForce;
        s.dir = Vec3{1, 0, 0};
        s.strength = 100.0f;
        const Vec3 plain = sb::fieldVelocityDelta(s, Vec3{250, 40, 7}, 1.0f, 1.0f, 8);
        check(approx(plain, Vec3{100, 0, 0}), "no turbulence: exactly the strength along the axis");
        s.turbulence = 0.8f;
        const Vec3 a = sb::fieldVelocityDelta(s, Vec3{250, 40, 7}, 1.0f, 1.0f, 8);
        const Vec3 b = sb::fieldVelocityDelta(s, Vec3{250, 40, 7}, 1.0f, 1.0f, 8);
        check(std::memcmp(&a, &b, sizeof a) == 0, "the same point and step give identical bits");
        f32 moved = 0.0f;
        for (u32 k = 0; k < 8; ++k) {
            const Vec3 q{250.0f + 100.0f * static_cast<f32>(k), 40.0f, 7.0f};
            s.turbulence = 0.8f;
            const Vec3 noisy = sb::fieldVelocityDelta(s, q, 1.0f, 1.0f, 8);
            s.turbulence = 0.0f;
            moved = std::max(moved, dist(noisy, sb::fieldVelocityDelta(s, q, 1.0f, 1.0f, 8)));
        }
        s.turbulence = 0.8f;
        check(moved > 1.0f, "and turbulence moves the result");
        const Vec3 sameWindow = sb::fieldVelocityDelta(s, Vec3{250, 40, 7}, 1.0f, 1.0f, 11);
        check(std::memcmp(&a, &sameWindow, sizeof a) == 0, "steps 8..11 share one window of 4");
    }

    AVER_INFO("the broad phase never misses a piece");
    {
        const Mat4 rot = Mat4::fromQuat(Quat::fromAxisAngle(Vec3{1, 1, 0}, 0.7f)) * Mat4::translation(Vec3{30, -10, 40});
        const sb::FieldShape shapes[3] = {sb::FieldShape::Sphere, sb::FieldShape::Box, sb::FieldShape::Capsule};
        u32 total = 0, bad = 0, positives = 0;
        for (const sb::FieldShape shape : shapes)
            for (u32 variant = 0; variant < 2; ++variant) {
                sb::FieldSpec s;
                s.shape = shape;
                s.world = variant == 0 ? Mat4::translation(Vec3{30, -10, 40}) : rot;
                s.extents = Vec3{40, 25, 60};
                for (i32 bx = -120; bx <= 120; bx += 40)
                    for (i32 by = -120; by <= 120; by += 40)
                        for (i32 bz = -100; bz <= 160; bz += 52) {
                            const Vec3 lo{static_cast<f32>(bx), static_cast<f32>(by), static_cast<f32>(bz)};
                            const Vec3 hi = lo + Vec3{30, 20, 25};
                            const bool broad = sb::fieldOverlapsBounds(s, lo, hi);
                            bool brute = false;
                            for (u32 k = 0; k < 6 * 6 * 6 && !brute; ++k) {
                                const Vec3 p{lo.x + (hi.x - lo.x) * static_cast<f32>(k % 6) / 5.0f, lo.y + (hi.y - lo.y) * static_cast<f32>((k / 6) % 6) / 5.0f,
                                             lo.z + (hi.z - lo.z) * static_cast<f32>(k / 36) / 5.0f};
                                brute = sb::fieldWeight(s, p) > 0.0f;
                            }
                            ++total;
                            positives += brute ? 1u : 0u;
                            if (brute && !broad) ++bad;
                        }
            }
        check(total > 300 && positives > 20, "the sweep covers both overlapping and clear boxes");
        check(bad == 0, "no overlapping box is rejected (no false negatives)");

        sb::FieldSpec all;
        all.shape = sb::FieldShape::Everywhere;
        check(sb::fieldOverlapsBounds(all, Vec3{1e5f, 1e5f, 1e5f}, Vec3{1e5f + 1, 1e5f + 1, 1e5f + 1}), "everywhere overlaps any box");
        const sb::FieldSpec distant = specAt(sb::FieldShape::Sphere, Vec3{0, 0, 0}, Vec3{10, 0, 0});
        check(!sb::fieldOverlapsBounds(distant, Vec3{100, 100, 100}, Vec3{110, 110, 110}), "a distant box is rejected");
    }

    AVER_INFO("beam weights and anchors");
    {
        sb::Cage c;
        sb::GridSpec g;
        g.cols = 5; g.rows = 5;
        g.du = Vec3{10, 0, 0}; g.dv = Vec3{0, 0, 10};
        g.material = sb::addMaterial(c, sb::Material{});
        const std::vector<u32> idx = sb::addGrid(c, g);
        sb::build(c);
        c.particles[idx[0]].pinned = true;     // (0, 0)
        c.particles[idx[24]].pinned = true;    // (40, 40)
        c.particles[idx[4]].pinned = true;     // (40, 0)

        sb::FieldSpec s = specAt(sb::FieldShape::Sphere, Vec3{0, 0, 0}, Vec3{15, 0, 0});
        std::vector<f32> w;
        sb::fieldBeamWeights(c, s, w);
        check(w.size() == c.beams.size(), "one weight per beam");
        bool anyIn = false, outsideZero = true;
        for (u32 bi = 0; bi < c.beams.size(); ++bi) {
            const Vec3 mid = (c.particles[c.beams[bi].a].pos + c.particles[c.beams[bi].b].pos) * 0.5f;
            const bool in = mid.size() < 15.0f;
            if (in) anyIn = anyIn || w[bi] > 0.0f;
            else outsideZero = outsideZero && w[bi] == 0.0f;
        }
        check(anyIn && outsideZero, "beams in reach have weight and the rest have none");
        for (u32 bi = 0; bi < c.beams.size(); ++bi) c.beams[bi].broken = true;
        sb::fieldBeamWeights(c, s, w);
        bool zero = true;
        for (const f32 v : w) zero = zero && v == 0.0f;
        check(zero, "broken beams carry no weight");

        std::vector<u32> anchors;
        check(sb::anchorsInField(c, s, anchors) == 1 && anchors.size() == 1 && anchors[0] == idx[0], "exactly the pinned particles inside the sphere");
        s.extents = Vec3{1000, 0, 0};
        check(sb::anchorsInField(c, s, anchors) == 3, "a bigger sphere takes all three pins");
        sb::FieldSpec all;
        all.shape = sb::FieldShape::Everywhere;
        check(sb::anchorsInField(c, all, anchors) == 3, "everywhere takes every pin");
    }

    AVER_INFO("fieldFrameFrom composes yaw, pitch, roll");
    {
        const Mat4 m = sb::fieldFrameFrom(Vec3{1, 2, 3}, Vec3{90, 0, 0});
        check(approx(Vec3{m.m[3][0], m.m[3][1], m.m[3][2]}, Vec3{1, 2, 3}), "the translation row is the centre");
        sb::FieldSpec s;
        s.world = m;
        s.kind = sb::FieldKind::DirectionalForce;
        s.shape = sb::FieldShape::Everywhere;
        s.dir = Vec3{1, 0, 0};
        s.strength = 10.0f;
        check(approx(sb::fieldVelocityDelta(s, Vec3{0, 0, 0}, 1.0f, 1.0f, 0), Vec3{0, 10, 0}, 1e-3f), "yaw 90 turns +X onto +Y");
    }

    AVER_INFO("{} failure(s)", g_failures);
    return g_failures == 0 ? 0 : 1;
}
