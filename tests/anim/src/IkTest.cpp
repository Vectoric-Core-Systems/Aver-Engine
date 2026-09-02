// IkTest -- two-bone IK, checked by where the chain ENDS UP rather than by return codes.
//
// THE STANDARD HERE IS OBSERVED GEOMETRY. A solver that silently produces the wrong bend, or that
// reaches the goal by stretching a bone, returns true every time -- so almost every check below
// measures a distance or a length after solving, against a number worked out from the fixture rather
// than from the code under test.
//
// The fixture is a straight arm along +Z: shoulder at the origin, elbow 40 cm up, wrist 60 cm above
// that. Segment lengths are therefore 40 and 60, reach is 100, and the chain cannot fold closer to
// the shoulder than |60-40| = 20. Those four numbers are what the assertions are built from.
#include "aver/anim/Ik.hpp"
#include "aver/anim/Pose.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcAnim.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static std::string f2s(f32 v) { char b[64]; std::snprintf(b, sizeof b, "%.2f", v); return b; }
static f32 len(const Vec3& v) { return std::sqrt(dot(v, v)); }
static f32 gap(const Vec3& a, const Vec3& b) { return len(a - b); }

// shoulder -> elbow (40 cm up) -> wrist (60 cm further up).
static fmt::OcSkeleton arm() {
    fmt::OcSkeleton s;
    fmt::OcBone shoulder; shoulder.name = "shoulder"; shoulder.parent = -1;
    fmt::OcBone elbow;    elbow.name    = "elbow";    elbow.parent    = 0; elbow.translation = Vec3{0, 0, 40};
    fmt::OcBone wrist;    wrist.name    = "wrist";    wrist.parent    = 1; wrist.translation = Vec3{0, 0, 60};
    s.bones = {shoulder, elbow, wrist};
    s.rootBone = 0;
    return s;
}

static Vec3 posOf(const fmt::OcSkeleton& s, const anim::Pose& p, u32 bone) {
    Vec3 v{0, 0, 0};
    anim::bonePositionModel(s, p, bone, v);
    return v;
}

// Segment lengths must never change: an IK that "reaches" by stretching a bone is the failure that
// looks most like success, so every solve is checked for it.
static void checkRigid(const fmt::OcSkeleton& s, const anim::Pose& p, const std::string& what) {
    const f32 a = gap(posOf(s, p, 1), posOf(s, p, 0));
    const f32 b = gap(posOf(s, p, 2), posOf(s, p, 1));
    check(std::fabs(a - 40.0f) < 0.1f && std::fabs(b - 60.0f) < 0.1f,
          what + ": the bones kept their lengths (40/60), got " + f2s(a) + "/" + f2s(b));
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("two-bone IK");

    const fmt::OcSkeleton s = arm();

    AVER_INFO("=== a reachable goal is reached ===");
    {
        anim::Pose p;
        anim::restPose(s, p);
        // Straight out along +X at 80 cm: well inside the 100 cm reach, and nowhere near the rest
        // pose's +Z direction, so a solver that did nothing would miss by a mile.
        const Vec3 goal{80, 0, 0};
        const Vec3 pole{0, -100, 20};
        check(anim::twoBoneIk(s, p, 0, 1, 2, goal, pole), "it solves");
        const f32 miss = gap(posOf(s, p, 2), goal);
        check(miss < 0.5f, "the wrist lands on the goal, missing by " + f2s(miss) + " cm");
        checkRigid(s, p, "reachable");
    }

    AVER_INFO("=== an unreachable goal straightens the chain AT it, and does not tear ===");
    {
        anim::Pose p;
        anim::restPose(s, p);
        // 300 cm away against a 100 cm reach.
        const Vec3 goal{300, 0, 0};
        check(anim::twoBoneIk(s, p, 0, 1, 2, goal, Vec3{0, -100, 20}), "it still solves");
        checkRigid(s, p, "unreachable");

        const Vec3 wrist = posOf(s, p, 2);
        const f32 reach = gap(wrist, posOf(s, p, 0));
        check(reach > 99.0f,
              "the arm is fully extended, reaching " + f2s(reach) + " of its 100 cm -- NOT stretched "
              "to 300, which is the failure this checks for");
        // Extended means the wrist sits on the line from shoulder toward the goal.
        const Vec3 want = Vec3{goal.x, goal.y, goal.z}.getSafeNormal() * reach;
        check(gap(wrist, want) < 1.0f,
              "and points AT the goal, off the shoulder-goal line by " + f2s(gap(wrist, want)) + " cm");
    }

    AVER_INFO("=== the pole decides which way the elbow bends ===");
    {
        // THE CHECK THAT CATCHES A SIGN ERROR. Two mirrored solutions reach any given goal; without
        // this, a solver that always bends the wrong way passes every distance test above.
        const Vec3 goal{70, 0, 0};
        anim::Pose a, b;
        anim::restPose(s, a);
        anim::restPose(s, b);
        anim::twoBoneIk(s, a, 0, 1, 2, goal, Vec3{0, -100, 0});
        anim::twoBoneIk(s, b, 0, 1, 2, goal, Vec3{0, +100, 0});

        const Vec3 ea = posOf(s, a, 1), eb = posOf(s, b, 1);
        check(gap(posOf(s, a, 2), goal) < 0.5f && gap(posOf(s, b, 2), goal) < 0.5f,
              "both poles still put the wrist on the goal");
        check(ea.y < -1.0f, "a pole at -Y puts the elbow on the -Y side, at y = " + f2s(ea.y));
        check(eb.y > +1.0f, "a pole at +Y puts it on the +Y side, at y = " + f2s(eb.y));
        check(gap(ea, eb) > 10.0f,
              "and the two elbows are genuinely different points, " + f2s(gap(ea, eb)) + " cm apart");
    }

    AVER_INFO("=== a goal at the rest position leaves the chain where it was ===");
    {
        anim::Pose p;
        anim::restPose(s, p);
        const Vec3 rest = posOf(s, p, 2);
        check(anim::twoBoneIk(s, p, 0, 1, 2, rest, Vec3{0, -100, 50}), "it solves");
        const f32 moved = gap(posOf(s, p, 2), rest);
        check(moved < 0.5f, "the wrist did not move, by " + f2s(moved) + " cm");
    }

    AVER_INFO("=== a chain hanging off a ROTATED parent, which is the case that broke it ===");
    {
        // THE GAP THIS CLOSES. The `arm` fixture above has a parentless root and identity rest
        // rotations everywhere, and that makes two thirds of applyModelDelta untestable: with no
        // parent there is no model->parent frame conversion to get wrong, and with an identity local
        // rotation the quaternion product commutes so its ORDER cannot matter either. Both were
        // wrong, and every assertion above still passed.
        //
        // What found it was a real humanoid skeleton: the hand landed 1,766 cm from a goal 6,633 cm
        // away, inside a 9,980 cm reach, with the bone lengths perfect. So the shape is reproduced
        // here in a fixture -- a root with a parent, and rest rotations that are not identity.
        fmt::OcSkeleton s2;
        fmt::OcBone base;     base.name = "base";     base.parent = -1;
        // A quarter turn about Z, then a tilt about X: enough that neither the conjugation nor the
        // product order can be got wrong and still land on the goal.
        base.rotation = (Quat::fromAxisAngle(Vec3{0, 0, 1}, 0.9f) *
                         Quat::fromAxisAngle(Vec3{1, 0, 0}, 0.6f)).normalized();
        base.translation = Vec3{10, -20, 30};

        fmt::OcBone shoulder; shoulder.name = "shoulder"; shoulder.parent = 0;
        shoulder.translation = Vec3{0, 0, 15};
        shoulder.rotation = Quat::fromAxisAngle(Vec3{0, 1, 0}, 0.4f).normalized();

        fmt::OcBone elbow;    elbow.name = "elbow";       elbow.parent = 1;
        elbow.translation = Vec3{0, 0, 40};
        elbow.rotation = Quat::fromAxisAngle(Vec3{1, 0, 0}, -0.3f).normalized();

        fmt::OcBone wrist;    wrist.name = "wrist";       wrist.parent = 2;
        wrist.translation = Vec3{0, 0, 60};

        s2.bones = {base, shoulder, elbow, wrist};
        s2.rootBone = 0;

        anim::Pose p;
        anim::restPose(s2, p);

        Vec3 sh{0, 0, 0}, wr{0, 0, 0};
        anim::bonePositionModel(s2, p, 1, sh);
        anim::bonePositionModel(s2, p, 3, wr);
        const f32 reach = 100.0f;   // 40 + 60, unchanged by any rotation

        // A goal comfortably inside the reach, and nowhere near the rest pose.
        const Vec3 goal = sh + Vec3{50, 30, -20};
        const Vec3 pole = sh + Vec3{-60, 40, 40};

        check(anim::twoBoneIk(s2, p, 1, 2, 3, goal, pole), "it solves on a parented, rotated chain");

        Vec3 got{0, 0, 0};
        anim::bonePositionModel(s2, p, 3, got);
        const f32 miss = gap(got, goal);
        check(miss < 0.5f,
              "and the tip lands ON the goal, missing by " + f2s(miss) +
              " cm -- the derived-but-wrong product order missed by a sixth of the reach here, with "
              "every length still perfect");

        Vec3 e2{0, 0, 0};
        anim::bonePositionModel(s2, p, 2, e2);
        check(std::fabs(gap(e2, sh) - 40.0f) < 0.1f && std::fabs(gap(got, e2) - 60.0f) < 0.1f,
              "with both bones still their own length");
        (void)reach; (void)wr;
    }

    AVER_INFO("=== what it refuses ===");
    {
        anim::Pose p;
        anim::restPose(s, p);
        const Vec3 g{50, 0, 0}, pole{0, -100, 0};
        check(!anim::twoBoneIk(s, p, 0, 1, 9, g, pole), "an out-of-range bone index");
        check(!anim::twoBoneIk(s, p, 0, 2, 1, g, pole), "a chain that is not parent-linked");
        check(!anim::twoBoneIk(s, p, 2, 1, 0, g, pole), "and the same chain named backwards");
        check(!anim::twoBoneIk(s, p, 0, 1, 2, posOf(s, p, 0), pole),
              "a goal exactly on the root, which has no direction to aim along");

        anim::Pose wrong;
        wrong.local.resize(2);
        check(!anim::twoBoneIk(s, wrong, 0, 1, 2, g, pole), "a pose whose bone count does not match");
    }

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return g_failures;
}
