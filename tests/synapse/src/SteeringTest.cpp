// Steering behaviours: seek, arrive, flee, wander, separation, path following, blending and the
// acceleration limit. Pure arithmetic, no scene.
#include "SynapseAiTestUtil.hpp"

#include "aver/synapse/Steering.hpp"

#include <cmath>

using namespace aver;
using namespace aver::synapse;
using aitest::check;

static bool approx(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }

int main() {
    AVER_INFO("SteeringTest");

    AVER_INFO("seek heads straight at the target at full speed");
    {
        const V2 v = steerSeek({0, 0}, {300, 400}, 100.0f);
        check(approx(v.x, 60.0f) && approx(v.y, 80.0f), "(300,400) from the origin at 100 cm/s is (60,80)");
        const V2 z = steerSeek({5, 5}, {5, 5}, 100.0f);
        check(approx(z.x, 0.0f) && approx(z.y, 0.0f), "on the target there is no direction, so no velocity");
    }

    AVER_INFO("arrive slows inside the slow radius and stops inside the stop radius");
    {
        const V2 distant = steerArrive({0, 0}, {1000, 0}, 200.0f, 300.0f, 20.0f);
        check(approx(distant.x, 200.0f), "outside the slow radius: full speed");
        const V2 mid = steerArrive({0, 0}, {160, 0}, 200.0f, 300.0f, 20.0f);
        check(mid.x > 0.0f && mid.x < 200.0f, "inside the slow radius: slower than full");
        const V2 closer = steerArrive({0, 0}, {60, 0}, 200.0f, 300.0f, 20.0f);
        check(closer.x < mid.x, "closer is slower still");
        const V2 stop = steerArrive({0, 0}, {15, 0}, 200.0f, 300.0f, 20.0f);
        check(approx(stop.x, 0.0f), "inside the stop radius: stopped");
    }

    AVER_INFO("flee runs away, tapering to nothing at the panic radius");
    {
        const V2 v = steerFlee({100, 0}, {0, 0}, 200.0f, 500.0f);
        check(v.x > 0.0f && approx(v.y, 0.0f), "away from the threat along +X");
        check(approx(len2(v), 200.0f * (1.0f - 100.0f / 500.0f)), "speed tapers with distance");
        const V2 out = steerFlee({600, 0}, {0, 0}, 200.0f, 500.0f);
        check(approx(len2(out), 0.0f), "beyond the panic radius it does not flee");
        const V2 on = steerFlee({0, 0}, {0, 0}, 200.0f, 0.0f);
        check(approx(len2(on), 200.0f), "panic radius 0 always flees, even from a coincident threat");
    }

    AVER_INFO("wander is deterministic for a seed and wanders forward");
    {
        WanderState a, b;
        a.rng = b.rng = 12345u;
        bool same = true, forward = true;
        for (int i = 0; i < 50; ++i) {
            const V2 va = steerWander({1, 0}, a, 100.0f, 200.0f, 80.0f, 4.0f, 0.033f);
            const V2 vb = steerWander({1, 0}, b, 100.0f, 200.0f, 80.0f, 4.0f, 0.033f);
            same = same && va.x == vb.x && va.y == vb.y;
            forward = forward && va.x > 0.0f && approx(len2(va), 100.0f, 0.01f);
        }
        check(same, "two walks from the same seed match exactly");
        check(forward, "every sample faces forward at the requested speed");
        WanderState c;
        c.rng = 999u;
        bool differs = false;
        WanderState d;
        d.rng = 12345u;
        for (int i = 0; i < 20 && !differs; ++i) {
            const V2 vc = steerWander({1, 0}, c, 100.0f, 200.0f, 80.0f, 4.0f, 0.033f);
            const V2 vd = steerWander({1, 0}, d, 100.0f, 200.0f, 80.0f, 4.0f, 0.033f);
            differs = vc.y != vd.y;
        }
        check(differs, "a different seed walks differently");
    }

    AVER_INFO("separation pushes away from close neighbours only");
    {
        const V2 others[] = {{50, 0}, {500, 0}};
        const V2 v = steerSeparation({0, 0}, others, 2, 100.0f, 100.0f);
        check(v.x < 0.0f && approx(v.y, 0.0f), "pushed away from the neighbour at +50, ignoring the far one");
        const V2 none = steerSeparation({0, 0}, others + 1, 1, 100.0f, 100.0f);
        check(approx(len2(none), 0.0f), "a neighbour outside the radius pushes nothing");
        const V2 both[] = {{50, 0}, {-50, 0}};
        const V2 cancel = steerSeparation({0, 0}, both, 2, 100.0f, 100.0f);
        check(approx(len2(cancel), 0.0f), "equal and opposite neighbours cancel");
    }

    AVER_INFO("path following advances through waypoints and arrives at the last");
    {
        const V2 pts[] = {{100, 0}, {100, 100}, {200, 100}};
        u32 idx = 0;
        V2 pos{0, 0};
        int steps = 0;
        while (idx < 3 && steps++ < 2000) {
            const V2 v = steerPathFollow(pos, pts, 3, idx, 20.0f, 100.0f, 80.0f);
            pos += v * 0.05f;
        }
        check(idx == 3, "reaches the end of the polyline");
        check(dist2(pos, {200, 100}) <= 25.0f, "and stands at the last point");
        u32 past = 3;
        const V2 after = steerPathFollow(pos, pts, 3, past, 20.0f, 100.0f, 80.0f);
        check(approx(len2(after), 0.0f), "past the end it asks for nothing");
    }

    AVER_INFO("blend clamps to max speed; acceleration is limited");
    {
        const SteerTerm terms[] = {{{100, 0}, 1.0f}, {{0, 100}, 1.0f}};
        const V2 v = blendSteering(terms, 2, 100.0f);
        check(approx(len2(v), 100.0f), "the sum is clamped to the max speed");
        check(v.x > 0.0f && v.y > 0.0f, "and keeps the blended direction");
        const SteerTerm zero[] = {{{100, 0}, 0.0f}};
        check(approx(len2(blendSteering(zero, 1, 100.0f)), 0.0f), "zero weight contributes nothing");

        const V2 a = accelerateToward({0, 0}, {100, 0}, 200.0f, 0.1f);
        check(approx(a.x, 20.0f), "200 cm/s^2 for 0.1 s changes velocity by 20");
        const V2 reach = accelerateToward({90, 0}, {100, 0}, 200.0f, 0.1f);
        check(approx(reach.x, 100.0f), "it does not overshoot the desired velocity");
    }

    AVER_INFO("steerRandNext never sticks at zero");
    {
        u32 s = 0;
        check(steerRandNext(s) != 0u && s != 0u, "a zero state is replaced");
    }

    return aitest::g_failures == 0 ? 0 : 1;
}
