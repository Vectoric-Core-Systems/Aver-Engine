// Style box tessellation: Flat (rounded rectangle with borders, shadow, anti-aliasing fringe, skew), Texture (9-patch), Line.
#include "aver/ui/UiStyleBox.hpp"

#include <algorithm>
#include <cmath>

namespace aver::ui {

namespace {

constexpr f32 kPi = 3.14159265358979f;

// Which side (0 L, 1 T, 2 R, 3 B) a corner's arc starts on and ends on.
constexpr int kArcFrom[4] = {0, 1, 2, 3};
constexpr int kArcTo[4]   = {1, 2, 3, 0};

struct Loop {
    std::vector<UiVec2> pts;
    std::vector<i8> corner;   // 0..3 for an arc point, -1 never (every point belongs to a corner)
    std::vector<i32> arcIdx;  // index within the corner's points
    std::vector<i32> arcLen;  // points in that corner
};

u32 lerpColor(u32 a, u32 b, f32 t) {
    u32 out = 0;
    for (int s = 0; s <= 24; s += 8) {
        const f32 ca = static_cast<f32>((a >> s) & 0xFF), cb = static_cast<f32>((b >> s) & 0xFF);
        out |= static_cast<u32>(ca + (cb - ca) * t + 0.5f) << s;
    }
    return out;
}

u32 withAlpha0(u32 c) { return c & 0x00FFFFFFu; }
u32 alphaOf(u32 c) { return c >> 24; }

// Builds the outline with `counts[c]` points on corner c (1 = a square corner, detail + 1 = an arc).
Loop buildLoop(const UiRect& r, const f32 rx[4], const f32 ry[4], const int counts[4], i32 detail) {
    Loop L;
    // Arc centres and angle ranges, clockwise on a y-down screen.
    const UiVec2 centre[4] = {{r.x + rx[0], r.y + ry[0]},
                              {r.x + r.w - rx[1], r.y + ry[1]},
                              {r.x + r.w - rx[2], r.y + r.h - ry[2]},
                              {r.x + rx[3], r.y + r.h - ry[3]}};
    const f32 a0[4] = {kPi, 1.5f * kPi, 0.0f, 0.5f * kPi};
    const UiVec2 sharp[4] = {{r.x, r.y}, {r.x + r.w, r.y}, {r.x + r.w, r.y + r.h}, {r.x, r.y + r.h}};
    for (int c = 0; c < 4; ++c) {
        const int n = counts[c];
        for (int j = 0; j < n; ++j) {
            UiVec2 p;
            if (n == 1) p = sharp[c];
            else {
                const f32 a = a0[c] + 0.5f * kPi * static_cast<f32>(j) / static_cast<f32>(detail);
                p = {centre[c].x + std::cos(a) * rx[c], centre[c].y + std::sin(a) * ry[c]};
            }
            L.pts.push_back(p);
            L.corner.push_back(static_cast<i8>(c));
            L.arcIdx.push_back(j);
            L.arcLen.push_back(n);
        }
    }
    return L;
}

// The outward direction at each point times `d`, for the anti-aliasing fringe and the shadow's feather.
std::vector<UiVec2> offsetLoop(const Loop& L, const f32 rx[4], const f32 ry[4], i32 detail, f32 d) {
    std::vector<UiVec2> out(L.pts.size());
    const f32 a0[4] = {kPi, 1.5f * kPi, 0.0f, 0.5f * kPi};
    const f32 sx[4] = {-1, 1, 1, -1}, sy[4] = {-1, -1, 1, 1};
    for (usize i = 0; i < L.pts.size(); ++i) {
        const int c = L.corner[i];
        if (L.arcLen[i] == 1 || rx[c] <= 0.0f || ry[c] <= 0.0f) {
            out[i] = {L.pts[i].x + sx[c] * d, L.pts[i].y + sy[c] * d};
            continue;
        }
        const f32 a = a0[c] + 0.5f * kPi * static_cast<f32>(L.arcIdx[i]) / static_cast<f32>(detail);
        f32 nx = std::cos(a) / rx[c], ny = std::sin(a) / ry[c];
        const f32 len = std::sqrt(nx * nx + ny * ny);
        nx /= len; ny /= len;
        out[i] = {L.pts[i].x + nx * d, L.pts[i].y + ny * d};
    }
    return out;
}

struct SegCol { u32 a0, a1, b0, b1; };

u32 vtx(UiMesh& m, UiVec2 p, u32 rgba) {
    m.verts.push_back(UiVertex{p.x, p.y, 0.0f, 0.0f, rgba});
    return static_cast<u32>(m.verts.size() - 1);
}

void tri(UiMesh& m, u32 a, u32 b, u32 c) { m.idx.push_back(a); m.idx.push_back(b); m.idx.push_back(c); }

// Quads between two loops of equal size, one per segment, coloured per segment.
template <class ColorFn>
void strip(UiMesh& m, const std::vector<UiVec2>& A, const std::vector<UiVec2>& B, ColorFn colors) {
    const usize n = A.size();
    for (usize i = 0; i < n; ++i) {
        const usize j = (i + 1) % n;
        const SegCol sc = colors(i);
        if (alphaOf(sc.a0) == 0 && alphaOf(sc.a1) == 0 && alphaOf(sc.b0) == 0 && alphaOf(sc.b1) == 0) continue;
        // Skip a segment with no area at all (a side with no border).
        const f32 d0 = std::fabs(A[i].x - B[i].x) + std::fabs(A[i].y - B[i].y), d1 = std::fabs(A[j].x - B[j].x) + std::fabs(A[j].y - B[j].y);
        if (d0 < 1e-5f && d1 < 1e-5f) continue;
        const u32 v0 = vtx(m, A[i], sc.a0), v1 = vtx(m, A[j], sc.a1), v2 = vtx(m, B[j], sc.b1), v3 = vtx(m, B[i], sc.b0);
        tri(m, v0, v1, v2);
        tri(m, v0, v2, v3);
    }
}

void fan(UiMesh& m, const std::vector<UiVec2>& P, UiVec2 centre, u32 rgba) {
    if (P.size() < 3 || alphaOf(rgba) == 0) return;
    const u32 c = vtx(m, centre, rgba);
    const u32 first = static_cast<u32>(m.verts.size());
    for (const UiVec2& p : P) vtx(m, p, rgba);
    const u32 n = static_cast<u32>(P.size());
    for (u32 i = 0; i < n; ++i) tri(m, c, first + i, first + (i + 1) % n);
}

// Scales the radii so neighbouring corners never overlap (the usual way).
void adaptRadii(const UiRect& r, f32 rad[4]) {
    f32 k = 1.0f;
    const f32 top = rad[0] + rad[1], bot = rad[3] + rad[2], lft = rad[0] + rad[3], rgt = rad[1] + rad[2];
    if (top > r.w && top > 0) k = std::min(k, r.w / top);
    if (bot > r.w && bot > 0) k = std::min(k, r.w / bot);
    if (lft > r.h && lft > 0) k = std::min(k, r.h / lft);
    if (rgt > r.h && rgt > 0) k = std::min(k, r.h / rgt);
    if (k < 1.0f) for (int i = 0; i < 4; ++i) rad[i] *= k;
}

void flatMesh(const UiStyleBox& sb, const UiRect& rect, UiMesh& out) {
    const UiRect box = uiStyleBoxDrawRect(sb, rect);
    if (box.w <= 0.0f || box.h <= 0.0f) return;
    const i32 detail = std::max(1, sb.cornerDetail);
    f32 rad[4] = {std::max(0.0f, sb.radiusTL), std::max(0.0f, sb.radiusTR), std::max(0.0f, sb.radiusBR), std::max(0.0f, sb.radiusBL)};
    adaptRadii(box, rad);
    const bool aa = (sb.flags & kSbAntiAlias) != 0 && sb.aaSize > 0.0f;
    const usize firstVert = out.verts.size();

    // Shadow: a copy of the box shifted by the offset, with a feather of `shadowSize` outside it.
    if (sb.shadowSize > 0.0f && alphaOf(sb.shadowColor) > 0) {
        const UiRect s{box.x + sb.shadowOffX, box.y + sb.shadowOffY, box.w, box.h};
        const int counts[4] = {detail + 1, detail + 1, detail + 1, detail + 1};
        const f32 rx[4] = {rad[0], rad[1], rad[2], rad[3]};
        Loop body = buildLoop(s, rx, rx, counts, detail);
        fan(out, body.pts, s.center(), sb.shadowColor);
        const UiRect g{s.x - sb.shadowSize, s.y - sb.shadowSize, s.w + 2 * sb.shadowSize, s.h + 2 * sb.shadowSize};
        const f32 gr[4] = {rad[0] + sb.shadowSize, rad[1] + sb.shadowSize, rad[2] + sb.shadowSize, rad[3] + sb.shadowSize};
        Loop feather = buildLoop(g, gr, gr, counts, detail);
        const u32 c0 = sb.shadowColor, c1 = withAlpha0(sb.shadowColor);
        strip(out, body.pts, feather.pts, [&](usize) { return SegCol{c0, c0, c1, c1}; });
    }

    // Borders, clamped so they never cross.
    f32 bw[4] = {std::max(0.0f, sb.borderL), std::max(0.0f, sb.borderT), std::max(0.0f, sb.borderR), std::max(0.0f, sb.borderB)};
    if (bw[0] + bw[2] > box.w && bw[0] + bw[2] > 0) { const f32 k = box.w / (bw[0] + bw[2]); bw[0] *= k; bw[2] *= k; }
    if (bw[1] + bw[3] > box.h && bw[1] + bw[3] > 0) { const f32 k = box.h / (bw[1] + bw[3]); bw[1] *= k; bw[3] *= k; }
    const bool border = bw[0] > 0 || bw[1] > 0 || bw[2] > 0 || bw[3] > 0;
    const UiRect inner{box.x + bw[0], box.y + bw[1], std::max(0.0f, box.w - bw[0] - bw[2]), std::max(0.0f, box.h - bw[1] - bw[3])};

    int counts[4];
    for (int c = 0; c < 4; ++c) counts[c] = rad[c] > 0.0f ? detail + 1 : 1;
    const f32 orx[4] = {rad[0], rad[1], rad[2], rad[3]};
    Loop outer = buildLoop(box, orx, orx, counts, detail);
    // Inner radii are elliptical: the corner radius less the border across each axis.
    const f32 irx[4] = {std::max(0.0f, rad[0] - bw[0]), std::max(0.0f, rad[1] - bw[2]), std::max(0.0f, rad[2] - bw[2]), std::max(0.0f, rad[3] - bw[0])};
    const f32 iry[4] = {std::max(0.0f, rad[0] - bw[1]), std::max(0.0f, rad[1] - bw[1]), std::max(0.0f, rad[2] - bw[3]), std::max(0.0f, rad[3] - bw[3])};
    Loop in = buildLoop(border ? inner : box, border ? irx : orx, border ? iry : orx, counts, detail);

    if ((sb.flags & kSbDrawCenter) && alphaOf(sb.bgColor) > 0) {
        const UiRect& fr = border ? inner : box;
        fan(out, in.pts, fr.center(), sb.bgColor);
    }

    const u32 sideColor[4] = {sb.borderColorL, sb.borderColorT, sb.borderColorR, sb.borderColorB};
    // Colour at both ends of segment i of the outer loop: the side it lies on, or a blend / split across a corner.
    const auto segmentColors = [&](usize i) {
        const usize j = (i + 1) % outer.pts.size();
        const int c = outer.corner[i];
        if (outer.corner[j] == c && outer.arcIdx[j] == outer.arcIdx[i] + 1) {
            const u32 A = sideColor[kArcFrom[c]], B = sideColor[kArcTo[c]];
            const f32 n = static_cast<f32>(outer.arcLen[i] - 1);
            const f32 t0 = static_cast<f32>(outer.arcIdx[i]) / n, t1 = static_cast<f32>(outer.arcIdx[j]) / n;
            if (sb.flags & kSbBorderBlend) return std::pair<u32, u32>{lerpColor(A, B, t0), lerpColor(A, B, t1)};
            const u32 h = (t0 + t1) * 0.5f < 0.5f ? A : B;
            return std::pair<u32, u32>{h, h};
        }
        // A straight edge: from the end of corner c to the start of the next corner: Top after TL, Right after TR, Bottom after BR, Left after BL.
        const u32 col = sideColor[kArcTo[c]];
        return std::pair<u32, u32>{col, col};
    };
    if (border) {
        strip(out, outer.pts, in.pts, [&](usize i) {
            const auto [c0, c1] = segmentColors(i);
            return SegCol{c0, c1, c0, c1};
        });
    }

    if (aa) {
        const std::vector<UiVec2> fringe = offsetLoop(outer, orx, orx, detail, sb.aaSize);
        strip(out, outer.pts, fringe, [&](usize i) {
            u32 c0, c1;
            if (border) { const auto cc = segmentColors(i); c0 = cc.first; c1 = cc.second; }
            else c0 = c1 = sb.bgColor;
            return SegCol{c0, c1, withAlpha0(c0), withAlpha0(c1)};
        });
    }

    if (sb.skewX != 0.0f || sb.skewY != 0.0f) {
        const UiVec2 c = box.center();
        for (usize i = firstVert; i < out.verts.size(); ++i) {
            UiVertex& v = out.verts[i];
            const f32 dy = (v.y - c.y) / (box.h > 0 ? box.h : 1.0f), dx = (v.x - c.x) / (box.w > 0 ? box.w : 1.0f);
            const f32 nx = v.x - sb.skewX * dy, ny = v.y + sb.skewY * dx;
            v.x = nx; v.y = ny;
        }
    }
}

void lineMesh(const UiStyleBox& sb, const UiRect& rect, UiMesh& out) {
    const UiRect box = uiStyleBoxDrawRect(sb, rect);
    if (alphaOf(sb.lineColor) == 0 || sb.thickness <= 0.0f) return;
    UiRect q;
    if (sb.flags & kSbVertical) q = {box.x + (box.w - sb.thickness) * 0.5f, box.y - sb.growBegin, sb.thickness, box.h + sb.growBegin + sb.growEnd};
    else q = {box.x - sb.growBegin, box.y + (box.h - sb.thickness) * 0.5f, box.w + sb.growBegin + sb.growEnd, sb.thickness};
    if (q.w <= 0.0f || q.h <= 0.0f) return;
    const u32 a = vtx(out, {q.x, q.y}, sb.lineColor), b = vtx(out, {q.x + q.w, q.y}, sb.lineColor),
              c = vtx(out, {q.x + q.w, q.y + q.h}, sb.lineColor), d = vtx(out, {q.x, q.y + q.h}, sb.lineColor);
    tri(out, a, b, c);
    tri(out, a, c, d);
}

} // namespace

UiRect uiStyleBoxDrawRect(const UiStyleBox& sb, const UiRect& rect) {
    return {rect.x - sb.expandL, rect.y - sb.expandT, rect.w + sb.expandL + sb.expandR, rect.h + sb.expandT + sb.expandB};
}

UiInsets uiStyleBoxMargins(const UiStyleBox& sb) {
    UiInsets m;
    switch (static_cast<UiStyleBoxKind>(sb.kind)) {
        case UiStyleBoxKind::Flat: m = {sb.borderL, sb.borderT, sb.borderR, sb.borderB}; break;
        case UiStyleBoxKind::Texture: m = {sb.patchL, sb.patchT, sb.patchR, sb.patchB}; break;
        default: break;
    }
    if (sb.contentL >= 0) m.left = sb.contentL;
    if (sb.contentT >= 0) m.top = sb.contentT;
    if (sb.contentR >= 0) m.right = sb.contentR;
    if (sb.contentB >= 0) m.bottom = sb.contentB;
    return m;
}

UiVec2 uiStyleBoxMinimumSize(const UiStyleBox& sb) {
    const UiInsets m = uiStyleBoxMargins(sb);
    return {m.horizontal(), m.vertical()};
}

UiRect uiStyleBoxContentRect(const UiStyleBox& sb, const UiRect& rect) {
    const UiInsets m = uiStyleBoxMargins(sb);
    return {rect.x + m.left, rect.y + m.top, std::max(0.0f, rect.w - m.horizontal()), std::max(0.0f, rect.h - m.vertical())};
}

std::vector<UiVec2> uiRoundedOutline(const UiRect& r, const f32 rx[4], const f32 ry[4], i32 detail) {
    int counts[4];
    for (int c = 0; c < 4; ++c) counts[c] = (rx[c] > 0.0f && ry[c] > 0.0f) ? std::max(1, detail) + 1 : 1;
    return buildLoop(r, rx, ry, counts, std::max(1, detail)).pts;
}

namespace {

// Cuts [0, len] into pieces for one axis of the middle: the pieces' destination extents and the 0..1 source fraction each shows.
struct Piece { f32 at, size, s0, s1; };

std::vector<Piece> axisPieces(f32 dstStart, f32 dstLen, f32 srcLen, UiAxisStretch mode) {
    std::vector<Piece> out;
    if (dstLen <= 0.0f) return out;
    if (mode == UiAxisStretch::Stretch || srcLen <= 0.0f) {
        out.push_back({dstStart, dstLen, 0.0f, 1.0f});
    } else if (mode == UiAxisStretch::Tile) {
        for (f32 at = 0.0f; at < dstLen - 1e-4f; at += srcLen) {
            const f32 sz = std::min(srcLen, dstLen - at);
            out.push_back({dstStart + at, sz, 0.0f, sz / srcLen});
        }
    } else {   // TileFit: the whole number of tiles that comes closest, each stretched to fill
        const int n = std::max(1, static_cast<int>(std::lround(dstLen / srcLen)));
        const f32 sz = dstLen / static_cast<f32>(n);
        for (int i = 0; i < n; ++i) out.push_back({dstStart + sz * static_cast<f32>(i), sz, 0.0f, 1.0f});
    }
    return out;
}

} // namespace

std::vector<UiPatchSlice> uiNinePatchSlices(const UiStyleBox& sb, const UiRect& rect) {
    std::vector<UiPatchSlice> out;
    if (rect.w <= 0.0f || rect.h <= 0.0f || sb.texW <= 0.0f || sb.texH <= 0.0f) return out;
    // Source region in pixels.
    f32 rx = sb.regionX, ry = sb.regionY, rw = sb.regionW, rh = sb.regionH;
    if (rw <= 0.0f || rh <= 0.0f) { rx = 0; ry = 0; rw = sb.texW; rh = sb.texH; }
    const f32 mL = std::max(0.0f, sb.patchL), mT = std::max(0.0f, sb.patchT), mR = std::max(0.0f, sb.patchR), mB = std::max(0.0f, sb.patchB);
    // Destination margins shrink together when the destination is smaller than their sum.
    const f32 kx = (mL + mR) > rect.w ? rect.w / (mL + mR) : 1.0f;
    const f32 ky = (mT + mB) > rect.h ? rect.h / (mT + mB) : 1.0f;
    const f32 dL = mL * kx, dR = mR * kx, dT = mT * ky, dB = mB * ky;
    const f32 dcw = rect.w - dL - dR, dch = rect.h - dT - dB;
    const f32 scw = std::max(0.0f, rw - mL - mR), sch = std::max(0.0f, rh - mT - mB);
    // Columns and rows: {dst start, dst size, src start px, src size px, tiling}
    struct Span { f32 at, size, src, srcSize; bool middle; };
    const Span cols[3] = {{rect.x, dL, rx, mL, false}, {rect.x + dL, dcw, rx + mL, scw, true}, {rect.x + dL + dcw, dR, rx + mL + scw, mR, false}};
    const Span rows[3] = {{rect.y, dT, ry, mT, false}, {rect.y + dT, dch, ry + mT, sch, true}, {rect.y + dT + dch, dB, ry + mT + sch, mB, false}};
    const bool drawCenter = (sb.flags & kSbDrawCenter) != 0;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            if (r == 1 && c == 1 && !drawCenter) continue;
            const Span& C = cols[c];
            const Span& R = rows[r];
            if (C.size <= 0.0f || R.size <= 0.0f || C.srcSize <= 0.0f || R.srcSize <= 0.0f) continue;
            const std::vector<Piece> px = C.middle ? axisPieces(C.at, C.size, C.srcSize, static_cast<UiAxisStretch>(sb.axisH))
                                                    : std::vector<Piece>{{C.at, C.size, 0.0f, 1.0f}};
            const std::vector<Piece> py = R.middle ? axisPieces(R.at, R.size, R.srcSize, static_cast<UiAxisStretch>(sb.axisV))
                                                    : std::vector<Piece>{{R.at, R.size, 0.0f, 1.0f}};
            for (const Piece& y : py) {
                for (const Piece& x : px) {
                    UiPatchSlice s;
                    s.dst = {x.at, y.at, x.size, y.size};
                    const f32 sx0 = C.src + C.srcSize * x.s0, sx1 = C.src + C.srcSize * x.s1;
                    const f32 sy0 = R.src + R.srcSize * y.s0, sy1 = R.src + R.srcSize * y.s1;
                    s.u0 = sx0 / sb.texW; s.u1 = sx1 / sb.texW;
                    s.v0 = sy0 / sb.texH; s.v1 = sy1 / sb.texH;
                    out.push_back(s);
                }
            }
        }
    }
    return out;
}

void uiStyleBoxMesh(const UiStyleBox& sb, const UiRect& rect, UiMesh& out) {
    switch (static_cast<UiStyleBoxKind>(sb.kind)) {
        case UiStyleBoxKind::Flat:
            out.texture = 0;
            flatMesh(sb, rect, out);
            break;
        case UiStyleBoxKind::Texture: {
            if (sb.texture == 0) return;
            out.texture = sb.texture;
            for (const UiPatchSlice& s : uiNinePatchSlices(sb, uiStyleBoxDrawRect(sb, rect))) {
                const u32 base = static_cast<u32>(out.verts.size());
                out.verts.push_back(UiVertex{s.dst.x, s.dst.y, s.u0, s.v0, sb.modulate});
                out.verts.push_back(UiVertex{s.dst.x + s.dst.w, s.dst.y, s.u1, s.v0, sb.modulate});
                out.verts.push_back(UiVertex{s.dst.x + s.dst.w, s.dst.y + s.dst.h, s.u1, s.v1, sb.modulate});
                out.verts.push_back(UiVertex{s.dst.x, s.dst.y + s.dst.h, s.u0, s.v1, sb.modulate});
                tri(out, base, base + 1, base + 2);
                tri(out, base, base + 2, base + 3);
            }
            break;
        }
        case UiStyleBoxKind::Line:
            out.texture = 0;
            lineMesh(sb, rect, out);
            break;
        default: break;
    }
}

void uiStyleBoxDraw(UiDrawList& dl, const UiStyleBox& sb, const UiRect& rect) {
    UiMesh m;
    uiStyleBoxMesh(sb, rect, m);
    if (!m.idx.empty()) dl.addMesh(m.verts.data(), m.verts.size(), m.idx.data(), m.idx.size(), m.texture);
}

} // namespace aver::ui
