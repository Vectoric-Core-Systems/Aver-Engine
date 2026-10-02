// ViewportPick.hpp's rayUprightCapsule: the Player Start's click test, capsule standing on the origin.
#include "ViewportPick.hpp"

#include "aver/core/Log.hpp"

#include <cmath>
#include <string>

using namespace aver;
using namespace aver::editor;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    AVER_ERROR("  FAIL  {}", what);
    ++g_failures;
}

int main() {
    AVER_INFO("ViewportPickCapsuleTest");
    // The Player Start's own capsule: radius 40, 184 tall, seams at z = 40 and z = 144.
    constexpr f32 r = 40.0f, h = 184.0f;
    f32 t = 0.0f;

    check(rayUprightCapsule(Vec3{-500, 0, 92}, Vec3{1, 0, 0}, r, h, 1e30f, t) && std::fabs(t - 460.0f) < 1e-3f,
          "side wall at mid height: entry at x = -40");
    check(rayUprightCapsule(Vec3{-500, 0, 170}, Vec3{1, 0, 0}, r, h, 1e30f, t) &&
              std::fabs(t - (500.0f - std::sqrt(r*r - 26.0f*26.0f))) < 1e-3f,
          "upper dome, where the old cube stand-in never reached");
    check(rayUprightCapsule(Vec3{0, 0, 1000}, Vec3{0, 0, -1}, r, h, 1e30f, t) && std::fabs(t - 816.0f) < 1e-3f,
          "straight down onto the top pole");
    check(!rayUprightCapsule(Vec3{-500, 0, 200}, Vec3{1, 0, 0}, r, h, 1e30f, t),
          "above the capsule misses");
    check(!rayUprightCapsule(Vec3{-500, 60, 92}, Vec3{1, 0, 0}, r, h, 1e30f, t),
          "beside the capsule misses");
    check(!rayUprightCapsule(Vec3{0, 0, 92}, Vec3{1, 0, 0}, r, h, 1e30f, t),
          "a ray starting inside is no hit");
    check(!rayUprightCapsule(Vec3{500, 0, 92}, Vec3{1, 0, 0}, r, h, 1e30f, t),
          "a capsule behind the ray origin is no hit");
    check(!rayUprightCapsule(Vec3{-500, 0, 92}, Vec3{1, 0, 0}, r, h, 100.0f, t),
          "a hit beyond tMax is not taken");
    check(rayUprightCapsule(Vec3{-500, 0, 92}, Vec3{10, 0, 0}, r, h, 1e30f, t) && std::fabs(t - 46.0f) < 1e-3f,
          "t is parametric along an unnormalised direction");

    AVER_INFO("ViewportPickCapsuleTest: {} of {} checks passed", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
