// HairCurves.cpp -- ImportedHairCurves to explicit strands. See HairCurves.hpp.
#include "aver/hair/HairCurves.hpp"

#include "aver/core/Math.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace aver::hair {
namespace {

bool fail(std::string* why, std::string m) {
    if (why) *why = std::move(m);
    return false;
}

struct P {
    Vec3 p;
    f32 w = 0.0f;
};

// n points evenly spaced by arc length along a polyline.
std::vector<P> resample(const std::vector<P>& in, u32 n) {
    std::vector<f32> cum(in.size(), 0.0f);
    for (usize i = 1; i < in.size(); ++i) cum[i] = cum[i - 1] + dist(in[i].p, in[i - 1].p);
    const f32 total = cum.back();
    std::vector<P> out;
    out.reserve(n);
    usize seg = 0;
    for (u32 k = 0; k < n; ++k) {
        const f32 target = total * static_cast<f32>(k) / static_cast<f32>(n - 1);
        while (seg + 2 < in.size() && cum[seg + 1] < target) ++seg;
        const f32 span = cum[seg + 1] - cum[seg];
        const f32 t = span > 0.0f ? std::clamp((target - cum[seg]) / span, 0.0f, 1.0f) : 0.0f;
        P q;
        q.p = lerp(in[seg].p, in[seg + 1].p, t);
        q.w = in[seg].w + (in[seg + 1].w - in[seg].w) * t;
        out.push_back(q);
    }
    return out;
}

} // namespace

bool curvesToStrands(const fmt::ImportedHairCurves& in, fmt::OcHairStrands& out, const CurveImportOptions& opt, CurveImportReport* report,
                     std::string* why) {
    CurveImportReport rep;
    out = fmt::OcHairStrands{};
    const usize n = in.strandCount();
    if (n == 0) return fail(why, "hair curves: the file has no line strands");
    if (in.strandStart.back() != in.pointCount() || in.material.size() != n) return fail(why, "hair curves: the curve tables are inconsistent");
    const bool hasWidth = !in.pointWidthCm.empty();
    if (hasWidth && in.pointWidthCm.size() != in.pointCount()) return fail(why, "hair curves: the width table does not match the points");
    const u32 maxPoints = std::clamp<u32>(opt.maxPoints, fmt::kOcHairMinStrandVertices, fmt::kOcHairMaxStrandVertices);

    std::vector<f32> widths;
    for (usize s = 0; s < n; ++s) {
        ++rep.strandsIn;
        std::vector<P> pts;
        for (u32 i = in.strandStart[s]; i < in.strandStart[s + 1]; ++i) {
            P q;
            q.p = Vec3(in.points[static_cast<usize>(i) * 3], in.points[static_cast<usize>(i) * 3 + 1], in.points[static_cast<usize>(i) * 3 + 2]);
            q.w = hasWidth ? in.pointWidthCm[i] : 0.0f;
            if (!std::isfinite(q.p.x) || !std::isfinite(q.p.y) || !std::isfinite(q.p.z)) { pts.clear(); break; }
            if (!pts.empty() && dist(pts.back().p, q.p) < opt.minSegmentCm) { ++rep.droppedPoints; continue; }
            pts.push_back(q);
        }
        if (pts.size() < fmt::kOcHairMinStrandVertices) { ++rep.droppedShort; continue; }
        if (pts.size() > maxPoints) { pts = resample(pts, maxPoints); ++rep.resampled; }

        out.start.push_back(static_cast<u32>(out.points.size() / 3));
        f32 wsum = 0.0f;
        for (const P& q : pts) {
            out.points.push_back(q.p.x);
            out.points.push_back(q.p.y);
            out.points.push_back(q.p.z);
            wsum += q.w;
        }
        u32 m = in.material[s];
        if (m > fmt::kOcHairMaxMaterials) { m = fmt::kOcHairMaxMaterials; ++rep.materialsClamped; }
        out.material.push_back(static_cast<u8>(m));
        widths.push_back(hasWidth ? wsum / static_cast<f32>(pts.size()) : opt.defaultWidthCm);
        ++rep.strandsOut;
    }
    if (rep.strandsOut == 0) {
        if (report) *report = rep;
        return fail(why, "hair curves: no strand has 3 usable points");
    }
    out.start.push_back(static_cast<u32>(out.points.size() / 3));
    if (hasWidth || opt.defaultWidthCm > 0.0f) out.width = std::move(widths);
    if (report) *report = rep;
    return true;
}

} // namespace aver::hair
