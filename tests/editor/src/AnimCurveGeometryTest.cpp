// AnimCurveGeometryTest -- the animation curve editor's headless core: screen<->curve mapping, key/
// tangent-handle layout, and hit-testing. See sandbox/src/AnimCurveGeometry.hpp/.cpp for what is
// under test, and AnimEditor.cpp's drawCurves() for the ImGui half this feeds -- the actual mouse
// drag is untestable outside a real window (see that file's own comment); everything decidable by
// arithmetic lives here instead, and this is what proves it.
//
// No ImGui, no Engine, no scene:: -- the same "headless first" precedent GraphEditorGeometryTest sets
// for the node editor, applied to the curve widget.
#include "AnimCurveGeometry.hpp"

#include "aver/core/Log.hpp"
#include "aver/formats/OcAnim.hpp"

#include <cmath>
#include <string>

using namespace aver;
using namespace aver::editor;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("   PASS  {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static void checkNear(f32 got, f32 want, f32 eps, const std::string& what) {
    if (std::fabs(got - want) <= eps) { AVER_INFO("   PASS  {} ({:.5f})", what, got); return; }
    AVER_ERROR("   FAIL  {} (got {:.6f}, want {:.6f})", what, got, want);
    ++g_failures;
}

// A plain 400x200 widget mapping time [0,4] and value [-2,2] onto it -- large enough that a
// one-pixel rounding error would never masquerade as a passing test.
static CurveView sampleView() {
    CurveView v;
    v.rectMin = Vec2{10.0f, 10.0f};
    v.rectMax = Vec2{410.0f, 210.0f};
    v.tMin = 0.0f; v.tMax = 4.0f;
    v.vMin = -2.0f; v.vMax = 2.0f;
    return v;
}

int main() {
    AVER_INFO("AnimCurveGeometryTest");

    AVER_INFO("=== curveToScreen / screenToCurve: the mapping is invertible ===");
    {
        const CurveView v = sampleView();

        // The four corners of the curve-space window land on the four corners of the rect, with Y
        // INVERTED (a higher value draws HIGHER on screen, i.e. a SMALLER y).
        const Vec2 topLeft = curveToScreen(v, v.tMin, v.vMax);
        checkNear(topLeft.x, v.rectMin.x, 1e-3f, "(tMin,vMax) lands at the rect's left edge");
        checkNear(topLeft.y, v.rectMin.y, 1e-3f, "and its TOP -- the max value draws highest");
        const Vec2 bottomRight = curveToScreen(v, v.tMax, v.vMin);
        checkNear(bottomRight.x, v.rectMax.x, 1e-3f, "(tMax,vMin) lands at the rect's right edge");
        checkNear(bottomRight.y, v.rectMax.y, 1e-3f, "and its BOTTOM -- the min value draws lowest");

        // The centre of the window lands at the centre of the rect.
        const Vec2 mid = curveToScreen(v, 2.0f, 0.0f);
        checkNear(mid.x, (v.rectMin.x + v.rectMax.x) * 0.5f, 1e-3f, "the midpoint centres on X");
        checkNear(mid.y, (v.rectMin.y + v.rectMax.y) * 0.5f, 1e-3f, "and on Y");

        // A ROUND TRIP at several points, including off-centre and non-integer ones, proves
        // screenToCurve is curveToScreen's actual inverse rather than merely agreeing at the corners.
        const f32 times[]  = {0.0f, 0.37f, 1.5f, 2.0f, 3.9f, 4.0f};
        const f32 values[] = {-2.0f, -1.1f, 0.0f, 0.6f, 1.9f, 2.0f};
        for (usize i = 0; i < 6; ++i) {
            const Vec2 screen = curveToScreen(v, times[i], values[i]);
            f32 t = 0.0f, val = 0.0f;
            screenToCurve(v, screen, t, val);
            checkNear(t, times[i], 1e-3f, "round-trip recovers time " + std::to_string(times[i]));
            checkNear(val, values[i], 1e-3f, "and value " + std::to_string(values[i]));
        }

        // A DEGENERATE WINDOW (zero span on an axis) does not divide by zero: it reads back as a
        // finite number rather than a NaN or an Inf.
        CurveView degenerate = v;
        degenerate.tMax = degenerate.tMin;   // zero time span
        const Vec2 p = curveToScreen(degenerate, 1.0f, 0.0f);
        check(std::isfinite(p.x) && std::isfinite(p.y), "a degenerate time window still maps to a finite point");
        f32 dt = 0.0f, dv = 0.0f;
        screenToCurve(degenerate, p, dt, dv);
        check(std::isfinite(dt) && std::isfinite(dv), "and reads back finite too");
    }

    AVER_INFO("=== computeCurveLayout: keys and tangent handles land where the format says they should ===");
    {
        const CurveView v = sampleView();
        fmt::OcCurve c;
        c.name = "Test";
        c.interp = fmt::OcInterp::CubicSpline;
        c.times  = {0.0f, 2.0f, 4.0f};
        c.values = {0.0f, 1.0f, -1.0f};
        c.inTangents  = {0.0f, -0.5f, 2.0f};
        c.outTangents = {1.0f,  0.5f, 0.0f};

        const auto layout = computeCurveLayout(c, v, 0.25f);
        check(layout.size() == 3, "one layout entry per key");
        for (usize i = 0; i < layout.size(); ++i) {
            check(layout[i].index == i, "layout entries stay in key order");
            check(layout[i].hasTangents, "a curve with matching-length tangents reports hasTangents");
            const Vec2 wantKey = curveToScreen(v, c.times[i], c.values[i]);
            checkNear(layout[i].keyScreen.x, wantKey.x, 1e-3f, "key " + std::to_string(i) + " X matches curveToScreen");
            checkNear(layout[i].keyScreen.y, wantKey.y, 1e-3f, "key " + std::to_string(i) + " Y matches curveToScreen");

            // The OUT handle sits handleSeconds AFTER the key at value + outTangent*handleSeconds --
            // exactly the "value + slope*dt" relationship hermite()'s own b0 term uses.
            const Vec2 wantOut = curveToScreen(v, c.times[i] + 0.25f, c.values[i] + c.outTangents[i] * 0.25f);
            checkNear(layout[i].outHandleScreen.x, wantOut.x, 1e-3f, "key " + std::to_string(i) + " OUT handle X");
            checkNear(layout[i].outHandleScreen.y, wantOut.y, 1e-3f, "key " + std::to_string(i) + " OUT handle Y");
            // The IN handle is the mirror image, handleSeconds BEFORE the key.
            const Vec2 wantIn = curveToScreen(v, c.times[i] - 0.25f, c.values[i] - c.inTangents[i] * 0.25f);
            checkNear(layout[i].inHandleScreen.x, wantIn.x, 1e-3f, "key " + std::to_string(i) + " IN handle X");
            checkNear(layout[i].inHandleScreen.y, wantIn.y, 1e-3f, "key " + std::to_string(i) + " IN handle Y");
        }

        // A curve with NO tangent data (empty, or short) reports hasTangents=false on every key --
        // matching sampleCurve's own "no CTAN chunk, reads as before" rule -- and its handle points
        // are simply unused rather than garbage a caller might accidentally draw.
        fmt::OcCurve noTangents = c;
        noTangents.inTangents.clear();
        noTangents.outTangents.clear();
        const auto plainLayout = computeCurveLayout(noTangents, v, 0.25f);
        check(plainLayout.size() == 3, "a curve with no tangents still lays out its keys");
        for (const CurveKeyLayout& k : plainLayout)
            check(!k.hasTangents, "but reports no tangent handles for key " + std::to_string(k.index));

        // A MALFORMED curve (times/values disagree) lays out to nothing, rather than reading past
        // the shorter array.
        fmt::OcCurve malformed = c;
        malformed.values.pop_back();
        check(computeCurveLayout(malformed, v, 0.25f).empty(),
              "a curve whose times and values disagree lays out to nothing");
    }

    AVER_INFO("=== curveHitTest: a click resolves to the key or handle nearest it, within radius ===");
    {
        const CurveView v = sampleView();
        fmt::OcCurve c;
        c.interp = fmt::OcInterp::CubicSpline;
        c.times  = {0.0f, 2.0f};
        c.values = {0.0f, 1.0f};
        c.inTangents  = {0.0f, -1.0f};
        c.outTangents = {1.0f, 0.0f};
        const auto layout = computeCurveLayout(c, v, 0.3f);
        check(layout.size() == 2, "fixture has two keys");
        const f32 r = 8.0f;

        // Dead centre of each target hits, comfortably outside radius misses.
        CurveHitResult hit = curveHitTest(layout, layout[0].keyScreen, r);
        check(hit.kind == CurveHitKind::Key && hit.keyIndex == 0, "clicking key 0 exactly picks key 0");

        hit = curveHitTest(layout, layout[1].keyScreen, r);
        check(hit.kind == CurveHitKind::Key && hit.keyIndex == 1, "clicking key 1 exactly picks key 1");

        hit = curveHitTest(layout, layout[0].outHandleScreen, r);
        check(hit.kind == CurveHitKind::OutHandle && hit.keyIndex == 0, "clicking key 0's out-handle picks it, not the key");

        hit = curveHitTest(layout, layout[1].inHandleScreen, r);
        check(hit.kind == CurveHitKind::InHandle && hit.keyIndex == 1, "clicking key 1's in-handle picks it");

        // A NEAR miss, just inside the radius, still hits -- proving this is a generous circular
        // target and not a single exact pixel.
        hit = curveHitTest(layout, Vec2{layout[0].keyScreen.x + r * 0.5f, layout[0].keyScreen.y}, r);
        check(hit.kind == CurveHitKind::Key && hit.keyIndex == 0, "a near miss inside the radius still hits");

        // Empty space, far from anything, hits nothing.
        hit = curveHitTest(layout, Vec2{v.rectMin.x + 1.0f, v.rectMin.y + 1.0f}, r);
        check(hit.kind == CurveHitKind::None, "empty space picks nothing");

        // KEYS WIN OVER HANDLES: place a query exactly on key 1's screen point, which (in this
        // fixture) is far from any handle, so this mainly documents the ordering contract -- keys are
        // tested in full before any handle -- rather than exercising a coincidental overlap.
        hit = curveHitTest(layout, layout[1].keyScreen, r);
        check(hit.kind == CurveHitKind::Key, "a key point resolves to the key even though handles are tested too");
    }

    AVER_INFO("=== tangentSlopeFromHandle: dragging a handle recovers the slope that placed it ===");
    {
        const CurveView v = sampleView();
        const Vec2 key = curveToScreen(v, 1.0f, 0.5f);

        // OUT handle: build one at a known slope via curveToScreen directly (the same formula
        // computeCurveLayout uses), then recover that slope from the handle's screen position.
        const f32 outSlope = 0.75f;
        const Vec2 outHandle = curveToScreen(v, 1.0f + 0.2f, 0.5f + outSlope * 0.2f);
        checkNear(tangentSlopeFromHandle(v, key, outHandle, /*isOutHandle=*/true), outSlope, 1e-3f,
                  "an out-handle's slope round-trips");

        // IN handle: same idea, mirrored to the left of the key.
        const f32 inSlope = -1.25f;
        const Vec2 inHandle = curveToScreen(v, 1.0f - 0.2f, 0.5f - inSlope * 0.2f);
        checkNear(tangentSlopeFromHandle(v, key, inHandle, /*isOutHandle=*/false), inSlope, 1e-3f,
                  "an in-handle's slope round-trips");

        // A FULL round trip through computeCurveLayout itself: lay a real curve out, then feed one of
        // its own handle positions back in and recover the tangent the format actually stores.
        fmt::OcCurve c;
        c.interp = fmt::OcInterp::CubicSpline;
        c.times = {0.0f, 3.0f};
        c.values = {2.0f, -1.0f};
        c.inTangents = {0.0f, 4.0f};
        c.outTangents = {-2.0f, 0.0f};
        const auto layout = computeCurveLayout(c, v, 0.5f);
        const f32 recoveredOut = tangentSlopeFromHandle(v, layout[0].keyScreen, layout[0].outHandleScreen, true);
        checkNear(recoveredOut, c.outTangents[0], 1e-3f, "key 0's OWN out-handle recovers key 0's OWN out-tangent");
        const f32 recoveredIn = tangentSlopeFromHandle(v, layout[1].keyScreen, layout[1].inHandleScreen, false);
        checkNear(recoveredIn, c.inTangents[1], 1e-3f, "key 1's OWN in-handle recovers key 1's OWN in-tangent");

        // DEGENERATE: a handle dragged ONTO or PAST its own key does not divide by zero or flip
        // sign -- the minSeconds floor holds it on the correct side.
        const f32 onKey = tangentSlopeFromHandle(v, key, key, /*isOutHandle=*/true);
        check(std::isfinite(onKey), "an out-handle dragged exactly onto its key stays finite");
        const Vec2 pastKey = curveToScreen(v, 1.0f - 0.1f, 0.5f + 5.0f);   // dragged to the WRONG side
        const f32 flipped = tangentSlopeFromHandle(v, key, pastKey, /*isOutHandle=*/true);
        check(std::isfinite(flipped), "an out-handle dragged past its key to the wrong side stays finite");
        // The floored dt keeps the SIGN an out-handle is supposed to have: dv is positive (value 0.5
        // -> 5.5) and dt is floored to a small POSITIVE number, so the slope comes out large and
        // positive, not negative from a naive (negative) dt.
        check(flipped > 0.0f, "and keeps the sign an out-handle's slope should have");
    }

    AVER_INFO(g_failures == 0 ? "AnimCurveGeometryTest: PASS" : "AnimCurveGeometryTest: {} FAILURES", g_failures);
    return g_failures ? 1 : 0;
}
