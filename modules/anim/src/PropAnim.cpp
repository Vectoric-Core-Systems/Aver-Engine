// Property-track sampling (see aver/anim/PropAnim.hpp). Written from the documented behaviour of animation players.

#include "aver/anim/PropAnim.hpp"

#include <algorithm>
#include <cmath>

namespace aver::anim {
namespace {

using namespace fmt;

bool approxEq(double a, double b) { return std::fabs(a - b) <= kPropEps * std::max(1.0, std::fabs(a)); }
double fposmod(double x, double y) {
    if (y == 0) return 0;
    double r = std::fmod(x, y);
    if (r < 0) r += y;
    return r;
}
double lerp(double a, double b, double t) { return a + (b - a) * t; }

int comps(OcPropKind k) {
    switch (k) {
        case OcPropKind::Bool: case OcPropKind::Int: case OcPropKind::Float: return 1;
        case OcPropKind::Vec2: return 2;
        case OcPropKind::Vec3: return 3;
        case OcPropKind::Vec4: case OcPropKind::Quat: case OcPropKind::Color: return 4;
        default: return 0;
    }
}

// ---- quaternions ----
struct Q { double x = 0, y = 0, z = 0, w = 1; };
Q fromValue(const PropValue& v) { return {v.n[0], v.n[1], v.n[2], v.n[3]}; }
PropValue toValue(const Q& q) {
    PropValue v; v.kind = OcPropKind::Quat; v.n[0] = q.x; v.n[1] = q.y; v.n[2] = q.z; v.n[3] = q.w; return v;
}
Q mul(const Q& a, const Q& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y + a.y * b.w + a.z * b.x - a.x * b.z,
            a.w * b.z + a.z * b.w + a.x * b.y - a.y * b.x, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
double dot(const Q& a, const Q& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
Q neg(const Q& a) { return {-a.x, -a.y, -a.z, -a.w}; }
Q norm(const Q& a) {
    const double l = std::sqrt(dot(a, a));
    return l > 0 ? Q{a.x / l, a.y / l, a.z / l, a.w / l} : Q{};
}
Q inv(const Q& a) { const double d = dot(a, a); return d > 0 ? Q{-a.x / d, -a.y / d, -a.z / d, a.w / d} : Q{}; }
Q slerp(const Q& a, const Q& b, double t) {
    double c = dot(a, b);
    Q to = b;
    if (c < 0) { c = -c; to = neg(b); }
    double s0, s1;
    if (1.0 - c > 1e-6) {
        const double om = std::acos(std::min(c, 1.0)), so = std::sin(om);
        s0 = std::sin((1.0 - t) * om) / so;
        s1 = std::sin(t * om) / so;
    } else { s0 = 1.0 - t; s1 = t; }
    return {s0 * a.x + s1 * to.x, s0 * a.y + s1 * to.y, s0 * a.z + s1 * to.z, s0 * a.w + s1 * to.w};
}
// The rotation vector (axis * angle) of a unit quaternion, in a quaternion with w = 0, and its inverse.
Q qlog(const Q& q) {
    const double s = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
    if (s < 1e-12) return {0, 0, 0, 0};
    const double angle = 2.0 * std::atan2(s, q.w);
    const double k = angle / s;
    return {q.x * k, q.y * k, q.z * k, 0};
}
Q qexp(const Q& v) {
    const double theta = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (theta < 1e-12) return {};
    const double s = std::sin(theta * 0.5) / theta;
    return {v.x * s, v.y * s, v.z * s, std::cos(theta * 0.5)};
}

// Barry-Goldman non-uniform Catmull-Rom: the keys' times matter (pre_t <= 0 < to_t <= post_t for a forward segment).
double cubicInTime(double from, double to, double pre, double post, double w, double toT, double preT, double postT) {
    const double t = toT * w;
    const double a1 = lerp(pre, from, preT == 0 ? 0 : (t - preT) / -preT);
    const double a2 = lerp(from, to, toT == 0 ? 0.5 : t / toT);
    const double a3 = lerp(to, post, postT - toT == 0 ? 1 : (t - toT) / (postT - toT));
    const double b1 = lerp(a1, a2, toT - preT == 0 ? 0 : (t - preT) / (toT - preT));
    const double b2 = lerp(a2, a3, postT == 0 ? 1 : t / postT);
    return lerp(b1, b2, toT == 0 ? 0.5 : t / toT);
}

// Shortest-way angle helpers.
double lerpAngle(double a, double b, double t) {
    const double diff = std::fmod(b - a, kPropTau);
    const double dist = std::fmod(2.0 * diff, kPropTau) - diff;
    return a + dist * t;
}
double cubicAngleInTime(double from, double to, double pre, double post, double w, double toT, double preT, double postT) {
    const double fromRot = std::fmod(from, kPropTau);
    const double preDiff = std::fmod(pre - fromRot, kPropTau);
    const double preRot = fromRot + std::fmod(2.0 * preDiff, kPropTau) - preDiff;
    const double toDiff = std::fmod(to - fromRot, kPropTau);
    const double toRot = fromRot + std::fmod(2.0 * toDiff, kPropTau) - toDiff;
    const double postDiff = std::fmod(post - toRot, kPropTau);
    const double postRot = toRot + std::fmod(2.0 * postDiff, kPropTau) - postDiff;
    return cubicInTime(fromRot, toRot, preRot, postRot, w, toT, preT, postT);
}

// Rotation about the same axis as a cubic in "log space" (a spherical cubic).
Q sphericalCubic(Q from, Q to, Q pre, Q post, double w, double toT, double preT, double postT) {
    from = norm(from); pre = norm(pre); to = norm(to); post = norm(post);
    if (dot(from, pre) < 0) pre = neg(pre);
    const bool flip2 = dot(from, to) < 0;
    if (flip2) to = neg(to);
    const bool flip3 = flip2 ? dot(to, post) <= 0 : dot(to, post) < 0;
    if (flip3) post = neg(post);

    auto ex = [&](const Q& base, const Q& qFrom, const Q& qTo, const Q& qPre, const Q& qPost) {
        const Q bi = inv(base);
        const Q lf = qlog(mul(bi, qFrom)), lt = qlog(mul(bi, qTo)), lp = qlog(mul(bi, qPre)), lq = qlog(mul(bi, qPost));
        Q ln;
        ln.x = cubicInTime(lf.x, lt.x, lp.x, lq.x, w, toT, preT, postT);
        ln.y = cubicInTime(lf.y, lt.y, lp.y, lq.y, w, toT, preT, postT);
        ln.z = cubicInTime(lf.z, lt.z, lp.z, lq.z, w, toT, preT, postT);
        ln.w = 0;
        return mul(base, qexp(ln));
    };
    const Q q1 = ex(from, from, to, pre, post);
    const Q q2 = ex(to, from, to, pre, post);
    return slerp(q1, q2, w);
}

PropValue lerpValue(const PropValue& a, const PropValue& b, double c, bool angle) {
    if (!propInterpolable(a.kind) || a.kind != b.kind) return a;
    PropValue r = a;
    if (a.kind == OcPropKind::Quat) { return toValue(slerp(fromValue(a), fromValue(b), c)); }
    const int n = comps(a.kind);
    for (int i = 0; i < n; ++i) r.n[i] = lerp(a.n[i], b.n[i], c);
    if (angle && (a.kind == OcPropKind::Float || a.kind == OcPropKind::Int)) r.n[0] = fposmod(lerpAngle(a.n[0], b.n[0], c), kPropTau);
    if (a.kind == OcPropKind::Int) r.n[0] = std::trunc(r.n[0]);
    return r;
}

PropValue cubicValue(const PropValue& from, const PropValue& to, const PropValue& pre, const PropValue& post, double w,
                     double toT, double preT, double postT, bool angle) {
    if (!propInterpolable(from.kind) || from.kind != to.kind || from.kind != pre.kind || from.kind != post.kind) return lerpValue(from, to, w, angle);
    if (from.kind == OcPropKind::Quat) return toValue(sphericalCubic(fromValue(from), fromValue(to), fromValue(pre), fromValue(post), w, toT, preT, postT));
    PropValue r = from;
    const int n = comps(from.kind);
    for (int i = 0; i < n; ++i)
        r.n[i] = (angle && n == 1) ? fposmod(cubicAngleInTime(from.n[0], to.n[0], pre.n[0], post.n[0], w, toT, preT, postT), kPropTau)
                                   : cubicInTime(from.n[i], to.n[i], pre.n[i], post.n[i], w, toT, preT, postT);
    if (from.kind == OcPropKind::Int) r.n[0] = std::trunc(r.n[0]);
    return r;
}

// Last key index with time <= t (+tolerance), -1 if none, among the first `n` keys.
int lastAtOrBefore(const std::vector<OcPropKey>& k, int n, double t) {
    int lo = 0, hi = n - 1, r = -1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (k[usize(mid)].time <= t + kPropEps * std::max(1.0, std::fabs(t))) { r = mid; lo = mid + 1; } else hi = mid - 1;
    }
    return r;
}

} // namespace

double propEase(double x, double c) {
    if (x < 0) x = 0; else if (x > 1) x = 1;
    if (c > 0) {
        if (c < 1) return 1.0 - std::pow(1.0 - x, 1.0 / c);
        return std::pow(x, c);
    }
    if (c < 0) {
        if (x < 0.5) return std::pow(x * 2.0, -c) * 0.5;
        return (1.0 - std::pow(1.0 - (x - 0.5) * 2.0, -c)) * 0.5 + 0.5;
    }
    return 0;
}

bool propInterpolable(OcPropKind k) {
    switch (k) {
        case OcPropKind::Int: case OcPropKind::Float: case OcPropKind::Vec2: case OcPropKind::Vec3:
        case OcPropKind::Vec4: case OcPropKind::Quat: case OcPropKind::Color: return true;
        default: return false;
    }
}

PropSample propSample(const OcAnimation& clip, const OcPropTrack& t, double time, bool backward) {
    PropSample r;
    const auto& K = t.keys;
    const double length = clip.duration;
    int n = lastAtOrBefore(K, int(K.size()), length) + 1;   // keys inside the clip
    if (n <= 0) return r;
    r.ok = true;
    if (n == 1) { r.value = K[0].value; return r; }
    const bool wrap = t.loopWrap && clip.propLoop == OcPropLoop::Linear && length > 0;
    const OcPropInterp interp = t.update == OcPropUpdate::Discrete ? OcPropInterp::Nearest : t.interp;

    struct Ep { const OcPropKey* k; double shift; };
    auto at = [&](int i) -> Ep {
        double shift = 0;
        if (wrap) {
            while (i < 0) { i += n; shift -= length; }
            while (i >= n) { i -= n; shift += length; }
        } else i = std::clamp(i, 0, n - 1);
        return {&K[usize(i)], shift};
    };

    const int idx = lastAtOrBefore(K, n, time);
    int ia, ib;
    if (idx < 0) {
        if (!wrap) { r.value = K[0].value; return r; }
        ia = -1; ib = 0;
    } else if (idx >= n - 1) {
        if (!wrap) { r.value = K[usize(n - 1)].value; return r; }
        ia = n - 1; ib = n;
    } else { ia = idx; ib = idx + 1; }

    const Ep A = at(ia), B = at(ib);
    const double ta = A.k->time + A.shift, tb = B.k->time + B.shift, delta = tb - ta;
    double c = delta <= 1e-12 ? 0.0 : (time - ta) / delta;
    c = std::clamp(c, 0.0, 1.0);

    const Ep& from = backward ? B : A;
    const Ep& to = backward ? A : B;
    const double tr = from.k->transition;
    if (tr == 0 || interp == OcPropInterp::Nearest) { r.value = from.k->value; return r; }
    double w = backward ? 1.0 - c : c;
    if (tr != 1.0) w = propEase(w, tr);

    const bool angle = interp == OcPropInterp::LinearAngle || interp == OcPropInterp::CubicAngle;
    if (interp == OcPropInterp::Linear || interp == OcPropInterp::LinearAngle) {
        r.value = lerpValue(from.k->value, to.k->value, w, angle);
        return r;
    }
    // Cubic: the neighbours past each end of the segment, in the direction of travel from `from`.
    const Ep pre = backward ? at(ib + 1) : at(ia - 1);
    const Ep post = backward ? at(ia - 1) : at(ib + 1);
    const double fromT = from.k->time + from.shift;
    r.value = cubicValue(from.k->value, to.k->value, pre.k->value, post.k->value, w, to.k->time + to.shift - fromT,
                         pre.k->time + pre.shift - fromT, post.k->time + post.shift - fromT, angle);
    return r;
}

double propSampleBezier(const OcPropTrack& t, double time) {
    const auto& K = t.keys;
    const int n = int(K.size());
    if (n == 0) return 0;
    const int idx = lastAtOrBefore(K, n, time);
    if (idx < 0) return K[0].value.n[0];
    if (idx >= n - 1) return K[usize(n - 1)].value.n[0];
    const OcPropKey& a = K[usize(idx)];
    const OcPropKey& b = K[usize(idx + 1)];
    const double dur = b.time - a.time;
    if (dur <= 1e-12) return a.value.n[0];
    // Handles are offsets from their key; the time offsets are clamped into the segment so x(t) stays monotone.
    double ox = std::max(0.0, a.handle[2]), oy = a.handle[3];
    double ix = std::min(0.0, b.handle[0]), iy = b.handle[1];
    if (ox > dur) { oy *= dur / ox; ox = dur; }
    if (-ix > dur) { iy *= dur / -ix; ix = -dur; }
    const double x0 = 0, y0 = a.value.n[0];
    const double x1 = ox, y1 = a.value.n[0] + oy;
    const double x2 = dur + ix, y2 = b.value.n[0] + iy;
    const double x3 = dur, y3 = b.value.n[0];
    const double target = time - a.time;
    auto bez = [](double p0, double p1, double p2, double p3, double u) {
        const double m = 1 - u;
        return m * m * m * p0 + 3 * m * m * u * p1 + 3 * m * u * u * p2 + u * u * u * p3;
    };
    double lo = 0, hi = 1;
    for (int i = 0; i < 50; ++i) {
        const double mid = (lo + hi) * 0.5;
        if (bez(x0, x1, x2, x3, mid) < target) lo = mid; else hi = mid;
    }
    return bez(y0, y1, y2, y3, (lo + hi) * 0.5);
}

int propFindKey(const OcPropTrack& t, double time, PropFind mode, bool backward, double length) {
    const auto& K = t.keys;
    const int n = int(K.size());
    if (n == 0) return -1;
    int idx = -1;
    if (mode == PropFind::Exact) {
        const int f = lastAtOrBefore(K, n, time);
        if (f >= 0 && approxEq(K[usize(f)].time, time)) idx = f;
        else return -1;
    } else if (!backward) {
        idx = lastAtOrBefore(K, n, time);
    } else {
        int lo = 0, hi = n - 1, r = -1;   // first key at or after time
        while (lo <= hi) {
            const int mid = (lo + hi) / 2;
            if (K[usize(mid)].time >= time - kPropEps * std::max(1.0, std::fabs(time))) { r = mid; hi = mid - 1; } else lo = mid + 1;
        }
        idx = r;
    }
    if (idx < 0 || idx >= n) return -1;
    const double kt = K[usize(idx)].time;
    if (kt < -kPropEps || kt > length + kPropEps) return -1;
    return idx;
}

void propKeysInSegments(const OcPropTrack& t, const std::vector<PropSegment>& segs, std::vector<int>& out) {
    const auto& K = t.keys;
    for (const PropSegment& s : segs) {
        if (s.b >= s.a) {
            for (int i = 0; i < int(K.size()); ++i) {
                const double kt = K[usize(i)].time;
                const bool afterStart = s.incStart ? kt >= s.a - kPropEps : kt > s.a + kPropEps;
                if (afterStart && kt <= s.b + kPropEps) out.push_back(i);
            }
        } else {
            for (int i = int(K.size()) - 1; i >= 0; --i) {
                const double kt = K[usize(i)].time;
                const bool beforeStart = s.incStart ? kt <= s.a + kPropEps : kt < s.a - kPropEps;
                if (beforeStart && kt >= s.b - kPropEps) out.push_back(i);
            }
        }
    }
}

} // namespace aver::anim
