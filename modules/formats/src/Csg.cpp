#include "aver/formats/Csg.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

namespace aver::fmt::csg {

namespace {

constexpr double kEps = 1e-5;
constexpr double kPi = 3.14159265358979323846;

struct Plane {
    double n[3] = {0, 0, 1};
    double w = 0;
};

struct PolyP {
    std::vector<Vertex> v;
    int material = 0;
    Plane plane;
};

double dot3(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
void cross3(const double a[3], const double b[3], double o[3]) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}
double len3(const double a[3]) { return std::sqrt(dot3(a, a)); }
void norm3(double a[3]) {
    const double l = len3(a);
    if (l > 1e-300) { a[0] /= l; a[1] /= l; a[2] /= l; }
}

// Newell's method: robust for a slightly non-planar or nearly collinear start. False for a sliver with no usable normal.
bool planeOf(const std::vector<Vertex>& v, Plane& pl) {
    double n[3] = {0, 0, 0};
    for (size_t i = 0; i < v.size(); ++i) {
        const double* a = v[i].p;
        const double* b = v[(i + 1) % v.size()].p;
        n[0] += (a[1] - b[1]) * (a[2] + b[2]);
        n[1] += (a[2] - b[2]) * (a[0] + b[0]);
        n[2] += (a[0] - b[0]) * (a[1] + b[1]);
    }
    if (len3(n) < 1e-14) return false;
    norm3(n);
    double c[3] = {0, 0, 0};
    for (const Vertex& x : v) { c[0] += x.p[0]; c[1] += x.p[1]; c[2] += x.p[2]; }
    c[0] /= double(v.size()); c[1] /= double(v.size()); c[2] /= double(v.size());
    pl.n[0] = n[0]; pl.n[1] = n[1]; pl.n[2] = n[2];
    pl.w = dot3(n, c);
    return true;
}

Vertex lerpV(const Vertex& a, const Vertex& b, double t) {
    Vertex r;
    for (int i = 0; i < 3; ++i) { r.p[i] = a.p[i] + (b.p[i] - a.p[i]) * t; r.n[i] = a.n[i] + (b.n[i] - a.n[i]) * t; }
    for (int i = 0; i < 2; ++i) r.uv[i] = a.uv[i] + (b.uv[i] - a.uv[i]) * t;
    if (len3(r.n) > 1e-12) norm3(r.n);
    return r;
}

void flipPoly(PolyP& p) {
    std::reverse(p.v.begin(), p.v.end());
    for (Vertex& x : p.v) { x.n[0] = -x.n[0]; x.n[1] = -x.n[1]; x.n[2] = -x.n[2]; }
    p.plane.n[0] = -p.plane.n[0]; p.plane.n[1] = -p.plane.n[1]; p.plane.n[2] = -p.plane.n[2];
    p.plane.w = -p.plane.w;
}

// 0 coplanar, 1 front, 2 back, 3 spanning.
void splitPolygon(const Plane& pl, const PolyP& poly, std::vector<PolyP>& cpf, std::vector<PolyP>& cpb, std::vector<PolyP>& front, std::vector<PolyP>& back) {
    int polyType = 0;
    std::vector<int> types(poly.v.size());
    for (size_t i = 0; i < poly.v.size(); ++i) {
        const double t = dot3(pl.n, poly.v[i].p) - pl.w;
        const int type = t < -kEps ? 2 : (t > kEps ? 1 : 0);
        polyType |= type;
        types[i] = type;
    }
    switch (polyType) {
        case 0: (dot3(pl.n, poly.plane.n) > 0 ? cpf : cpb).push_back(poly); break;
        case 1: front.push_back(poly); break;
        case 2: back.push_back(poly); break;
        default: {
            std::vector<Vertex> f, b;
            for (size_t i = 0; i < poly.v.size(); ++i) {
                const size_t j = (i + 1) % poly.v.size();
                const int ti = types[i], tj = types[j];
                const Vertex& vi = poly.v[i];
                const Vertex& vj = poly.v[j];
                if (ti != 2) f.push_back(vi);
                if (ti != 1) b.push_back(vi);
                if ((ti | tj) == 3) {
                    double d[3] = {vj.p[0] - vi.p[0], vj.p[1] - vi.p[1], vj.p[2] - vi.p[2]};
                    const double t = (pl.w - dot3(pl.n, vi.p)) / dot3(pl.n, d);
                    const Vertex v = lerpV(vi, vj, t);
                    f.push_back(v);
                    b.push_back(v);
                }
            }
            if (f.size() >= 3) { PolyP q; q.v = std::move(f); q.material = poly.material; q.plane = poly.plane; front.push_back(std::move(q)); }
            if (b.size() >= 3) { PolyP q; q.v = std::move(b); q.material = poly.material; q.plane = poly.plane; back.push_back(std::move(q)); }
        }
    }
}

struct Node {
    bool hasPlane = false;
    Plane plane;
    int front = -1, back = -1;
    std::vector<PolyP> polys;
};

class Bsp {
public:
    explicit Bsp(std::vector<PolyP> polys) {
        nodes_.emplace_back();
        build(0, std::move(polys));
    }

    void invert() {
        for (Node& n : nodes_) {
            for (PolyP& p : n.polys) flipPoly(p);
            if (n.hasPlane) { for (double& c : n.plane.n) c = -c; n.plane.w = -n.plane.w; }
            std::swap(n.front, n.back);
        }
    }

    // The part of `in` outside this tree's solid (its front side everywhere).
    std::vector<PolyP> clipPolygons(std::vector<PolyP> in) const {
        std::vector<PolyP> result;
        std::vector<std::pair<int, std::vector<PolyP>>> stack;
        stack.emplace_back(0, std::move(in));
        while (!stack.empty()) {
            auto [ni, polys] = std::move(stack.back());
            stack.pop_back();
            const Node& n = nodes_[static_cast<size_t>(ni)];
            if (!n.hasPlane) { for (PolyP& p : polys) result.push_back(std::move(p)); continue; }
            std::vector<PolyP> front, back;
            for (const PolyP& p : polys) splitPolygon(n.plane, p, front, back, front, back);
            if (n.front >= 0) { if (!front.empty()) stack.emplace_back(n.front, std::move(front)); }
            else for (PolyP& p : front) result.push_back(std::move(p));
            if (n.back >= 0 && !back.empty()) stack.emplace_back(n.back, std::move(back));
            // no back child: the polygons are inside the solid and are dropped
        }
        return result;
    }

    void clipTo(const Bsp& other) {
        for (Node& n : nodes_)
            if (!n.polys.empty()) n.polys = other.clipPolygons(std::move(n.polys));
    }

    std::vector<PolyP> allPolygons() const {
        std::vector<PolyP> r;
        for (const Node& n : nodes_) r.insert(r.end(), n.polys.begin(), n.polys.end());
        return r;
    }

    void build(int root, std::vector<PolyP> polys) {
        std::vector<std::pair<int, std::vector<PolyP>>> stack;
        stack.emplace_back(root, std::move(polys));
        while (!stack.empty()) {
            auto [ni, list] = std::move(stack.back());
            stack.pop_back();
            if (list.empty()) continue;
            if (!nodes_[static_cast<size_t>(ni)].hasPlane) {
                nodes_[static_cast<size_t>(ni)].hasPlane = true;
                nodes_[static_cast<size_t>(ni)].plane = list[0].plane;
            }
            const Plane pl = nodes_[static_cast<size_t>(ni)].plane;
            std::vector<PolyP> front, back, own;
            for (const PolyP& p : list) splitPolygon(pl, p, own, own, front, back);
            auto& polysOf = nodes_[static_cast<size_t>(ni)].polys;
            for (PolyP& p : own) polysOf.push_back(std::move(p));
            if (!front.empty()) {
                if (nodes_[static_cast<size_t>(ni)].front < 0) { nodes_[static_cast<size_t>(ni)].front = static_cast<int>(nodes_.size()); nodes_.emplace_back(); }
                stack.emplace_back(nodes_[static_cast<size_t>(ni)].front, std::move(front));
            }
            if (!back.empty()) {
                if (nodes_[static_cast<size_t>(ni)].back < 0) { nodes_[static_cast<size_t>(ni)].back = static_cast<int>(nodes_.size()); nodes_.emplace_back(); }
                stack.emplace_back(nodes_[static_cast<size_t>(ni)].back, std::move(back));
            }
        }
    }

private:
    std::vector<Node> nodes_;
};

std::vector<PolyP> toInternal(const Mesh& m) {
    std::vector<PolyP> r;
    r.reserve(m.polys.size());
    for (const Polygon& p : m.polys) {
        if (p.v.size() < 3) continue;
        PolyP q;
        q.v = p.v;
        q.material = p.material;
        if (!planeOf(q.v, q.plane)) continue;
        r.push_back(std::move(q));
    }
    return r;
}

Mesh fromInternal(const std::vector<PolyP>& in) {
    Mesh m;
    for (const PolyP& p : in) {
        if (p.v.size() < 3) continue;
        Plane pl;
        if (!planeOf(p.v, pl)) continue;   // a sliver left by clipping
        Polygon q;
        q.v = p.v;
        q.material = p.material;
        m.polys.push_back(std::move(q));
    }
    return m;
}

// Makes the polygon's normal point away from `ref` (reverses it when it points toward).
void orientAway(Polygon& p, const double ref[3]) {
    Plane pl;
    if (!planeOf(p.v, pl)) return;
    double c[3] = {0, 0, 0};
    for (const Vertex& v : p.v) for (int i = 0; i < 3; ++i) c[i] += v.p[i] / double(p.v.size());
    const double d[3] = {c[0] - ref[0], c[1] - ref[1], c[2] - ref[2]};
    if (dot3(pl.n, d) < 0) {
        std::reverse(p.v.begin(), p.v.end());
        for (Vertex& v : p.v) { v.n[0] = -v.n[0]; v.n[1] = -v.n[1]; v.n[2] = -v.n[2]; }
    }
}

// Orients along a given outward direction instead of a point.
void orientAlong(Polygon& p, const double dir[3]) {
    Plane pl;
    if (!planeOf(p.v, pl)) return;
    if (dot3(pl.n, dir) < 0) {
        std::reverse(p.v.begin(), p.v.end());
        for (Vertex& v : p.v) { v.n[0] = -v.n[0]; v.n[1] = -v.n[1]; v.n[2] = -v.n[2]; }
    }
}

Vertex mk(double x, double y, double z, double u = 0, double v = 0) {
    Vertex r;
    r.p[0] = x; r.p[1] = y; r.p[2] = z; r.uv[0] = u; r.uv[1] = v;
    return r;
}

// Ear clipping of a simple polygon (any winding); returns index triples counter-clockwise.
std::vector<unsigned> earClip(const std::vector<double>& xy) {
    std::vector<unsigned> out;
    const size_t n = xy.size() / 2;
    if (n < 3) return out;
    double area = 0;
    for (size_t i = 0; i < n; ++i) {
        const size_t j = (i + 1) % n;
        area += xy[i * 2] * xy[j * 2 + 1] - xy[j * 2] * xy[i * 2 + 1];
    }
    std::vector<unsigned> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<unsigned>(i);
    if (area < 0) std::reverse(v.begin(), v.end());
    const auto cr = [&](unsigned a, unsigned b, unsigned c) {
        return (xy[b * 2] - xy[a * 2]) * (xy[c * 2 + 1] - xy[a * 2 + 1]) - (xy[b * 2 + 1] - xy[a * 2 + 1]) * (xy[c * 2] - xy[a * 2]);
    };
    size_t guard = 0;
    while (v.size() > 3 && guard++ < n * n + 8) {
        bool clipped = false;
        for (size_t i = 0; i < v.size(); ++i) {
            const unsigned a = v[(i + v.size() - 1) % v.size()], b = v[i], c = v[(i + 1) % v.size()];
            if (cr(a, b, c) <= 1e-12) continue;
            bool ear = true;
            for (const unsigned q : v)
                if (q != a && q != b && q != c && cr(a, b, q) >= 0 && cr(b, c, q) >= 0 && cr(c, a, q) >= 0) { ear = false; break; }
            if (!ear) continue;
            out.insert(out.end(), {a, b, c});
            v.erase(v.begin() + static_cast<long>(i));
            clipped = true;
            break;
        }
        if (!clipped) break;
    }
    if (v.size() == 3) out.insert(out.end(), {v[0], v[1], v[2]});
    return out;
}

}  // namespace

Mesh combine(const Mesh& a, const Mesh& b, Op op) {
    if (a.empty()) return op == Op::Union ? b : Mesh{};
    if (b.empty()) return op == Op::Intersection ? Mesh{} : a;
    Bsp A(toInternal(a)), B(toInternal(b));
    switch (op) {
        case Op::Union:
            A.clipTo(B);
            B.clipTo(A);
            B.invert();
            B.clipTo(A);
            B.invert();
            A.build(0, B.allPolygons());
            break;
        case Op::Subtraction:
            A.invert();
            A.clipTo(B);
            B.clipTo(A);
            B.invert();
            B.clipTo(A);
            B.invert();
            A.build(0, B.allPolygons());
            A.invert();
            break;
        case Op::Intersection:
            A.invert();
            B.clipTo(A);
            B.invert();
            A.clipTo(B);
            B.clipTo(A);
            A.build(0, B.allPolygons());
            A.invert();
            break;
    }
    return fromInternal(A.allPolygons());
}

void transform(Mesh& m, const double t[12]) {
    // Inverse transpose of the 3x3 part: the cofactor matrix over the determinant.
    const double a = t[0], b = t[1], c = t[2], d = t[4], e = t[5], f = t[6], g = t[8], h = t[9], i = t[10];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    double cof[9] = {e * i - f * h, -(d * i - f * g), d * h - e * g, -(b * i - c * h), a * i - c * g, -(a * h - b * g), b * f - c * e, -(a * f - c * d), a * e - b * d};
    const double inv = std::fabs(det) > 1e-300 ? 1.0 / det : 1.0;
    for (double& x : cof) x *= inv;
    for (Polygon& p : m.polys) {
        for (Vertex& v : p.v) {
            const double x = v.p[0], y = v.p[1], z = v.p[2];
            v.p[0] = t[0] * x + t[1] * y + t[2] * z + t[3];
            v.p[1] = t[4] * x + t[5] * y + t[6] * z + t[7];
            v.p[2] = t[8] * x + t[9] * y + t[10] * z + t[11];
            const double nx = v.n[0], ny = v.n[1], nz = v.n[2];
            v.n[0] = cof[0] * nx + cof[1] * ny + cof[2] * nz;
            v.n[1] = cof[3] * nx + cof[4] * ny + cof[5] * nz;
            v.n[2] = cof[6] * nx + cof[7] * ny + cof[8] * nz;
            if (len3(v.n) > 1e-12) norm3(v.n);
        }
        if (det < 0) std::reverse(p.v.begin(), p.v.end());
    }
}

void invertFaces(Mesh& m) {
    for (Polygon& p : m.polys) {
        std::reverse(p.v.begin(), p.v.end());
        for (Vertex& v : p.v) { v.n[0] = -v.n[0]; v.n[1] = -v.n[1]; v.n[2] = -v.n[2]; }
    }
}

Bounds bounds(const Mesh& m) {
    Bounds b;
    for (int i = 0; i < 3; ++i) { b.lo[i] = 1e300; b.hi[i] = -1e300; }
    for (const Polygon& p : m.polys)
        for (const Vertex& v : p.v) {
            b.valid = true;
            for (int i = 0; i < 3; ++i) { b.lo[i] = std::min(b.lo[i], v.p[i]); b.hi[i] = std::max(b.hi[i], v.p[i]); }
        }
    if (!b.valid) for (int i = 0; i < 3; ++i) b.lo[i] = b.hi[i] = 0;
    return b;
}

double volume(const Mesh& m) {
    double vol = 0;
    for (const Polygon& p : m.polys)
        for (size_t k = 1; k + 1 < p.v.size(); ++k) {
            double cx[3];
            cross3(p.v[k].p, p.v[k + 1].p, cx);
            vol += dot3(p.v[0].p, cx) / 6.0;
        }
    return vol;
}

double surfaceArea(const Mesh& m) {
    double area = 0;
    for (const Polygon& p : m.polys)
        for (size_t k = 1; k + 1 < p.v.size(); ++k) {
            const double e1[3] = {p.v[k].p[0] - p.v[0].p[0], p.v[k].p[1] - p.v[0].p[1], p.v[k].p[2] - p.v[0].p[2]};
            const double e2[3] = {p.v[k + 1].p[0] - p.v[0].p[0], p.v[k + 1].p[1] - p.v[0].p[1], p.v[k + 1].p[2] - p.v[0].p[2]};
            double cx[3];
            cross3(e1, e2, cx);
            area += 0.5 * len3(cx);
        }
    return area;
}

// ---- solids ----

Mesh makeBox(const double size[3], int material) {
    Mesh m;
    const double h[3] = {size[0] / 2, size[1] / 2, size[2] / 2};
    // axis a is the face normal; u = axis (a+1)%3 and v = axis (a+2)%3 satisfy u x v = +normal for a positive face.
    for (int a = 0; a < 3; ++a)
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            const int ua = (a + 1) % 3, va = (a + 2) % 3;
            Polygon p;
            p.material = material;
            const double su[4] = {-1, 1, 1, -1}, sv[4] = {-1, -1, 1, 1};
            for (int k = 0; k < 4; ++k) {
                Vertex v;
                v.p[a] = sgn * h[a];
                v.p[ua] = su[k] * h[ua];
                v.p[va] = sv[k] * h[va];
                v.uv[0] = (su[k] + 1) / 2; v.uv[1] = (sv[k] + 1) / 2;
                v.n[a] = sgn;
                p.v.push_back(v);
            }
            if (sgn < 0) std::reverse(p.v.begin(), p.v.end());
            m.polys.push_back(std::move(p));
        }
    return m;
}

Mesh makeSphere(double radius, int radialSegments, int rings, bool smooth, int material) {
    Mesh m;
    const int R = std::max(radialSegments, 3), B = std::max(rings, 1) + 1;
    const auto pt = [&](int i, int j) {
        const double th = kPi * i / B, ph = 2 * kPi * (j % R) / R;
        Vertex v = mk(radius * std::sin(th) * std::sin(ph), radius * std::cos(th), radius * std::sin(th) * std::cos(ph), double(j) / R, double(i) / B);
        if (smooth) { v.n[0] = v.p[0]; v.n[1] = v.p[1]; v.n[2] = v.p[2]; norm3(v.n); }
        return v;
    };
    const double origin[3] = {0, 0, 0};
    for (int i = 0; i < B; ++i)
        for (int j = 0; j < R; ++j) {
            Polygon p;
            p.material = material;
            if (i == 0) p.v = {pt(0, j), pt(1, j + 1), pt(1, j)};
            else if (i == B - 1) p.v = {pt(i, j), pt(i, j + 1), pt(B, j)};
            else p.v = {pt(i, j), pt(i, j + 1), pt(i + 1, j + 1), pt(i + 1, j)};
            orientAway(p, origin);
            m.polys.push_back(std::move(p));
        }
    return m;
}

Mesh makeCylinder(double radius, double height, int sides, bool cone, bool smooth, int material) {
    Mesh m;
    const int S = std::max(sides, 3);
    const double hy = height / 2;
    const double rTop = cone ? 0.0 : radius;
    const auto ring = [&](double r, double y, int j) {
        const double a = 2 * kPi * (j % S) / S;
        Vertex v = mk(r * std::sin(a), y, r * std::cos(a), double(j) / S, y > 0 ? 0.0 : 1.0);
        if (smooth) { v.n[0] = std::sin(a); v.n[1] = 0; v.n[2] = std::cos(a); }
        return v;
    };
    const double axis[3] = {0, 0, 0};
    for (int j = 0; j < S; ++j) {
        Polygon p;
        p.material = material;
        if (cone) p.v = {ring(radius, -hy, j), ring(0, hy, j), ring(radius, -hy, j + 1)};
        else p.v = {ring(radius, -hy, j), ring(rTop, hy, j), ring(rTop, hy, j + 1), ring(radius, -hy, j + 1)};
        if (cone && smooth) {   // slanted normals: the radial direction tilted up by the cone's slope
            for (Vertex& v : p.v) {
                v.n[0] *= height; v.n[2] *= height; v.n[1] = radius;
                norm3(v.n);
            }
        }
        orientAway(p, axis);
        m.polys.push_back(std::move(p));
    }
    Polygon bottom;
    bottom.material = material;
    for (int j = 0; j < S; ++j) { Vertex v = ring(radius, -hy, j); v.n[0] = v.n[2] = 0; v.n[1] = -1; bottom.v.push_back(v); }
    const double down[3] = {0, -1, 0};
    orientAlong(bottom, down);
    m.polys.push_back(std::move(bottom));
    if (!cone) {
        Polygon top;
        top.material = material;
        for (int j = 0; j < S; ++j) { Vertex v = ring(rTop, hy, j); v.n[0] = v.n[2] = 0; v.n[1] = 1; top.v.push_back(v); }
        const double up[3] = {0, 1, 0};
        orientAlong(top, up);
        m.polys.push_back(std::move(top));
    }
    return m;
}

Mesh makeTorus(double innerRadius, double outerRadius, int sides, int ringSides, bool smooth, int material) {
    Mesh m;
    const int S = std::max(sides, 3), RS = std::max(ringSides, 3);
    const double R = (innerRadius + outerRadius) / 2, r = std::fabs(outerRadius - innerRadius) / 2;
    const auto pt = [&](int i, int j) {
        const double a = 2 * kPi * (i % RS) / RS, c = 2 * kPi * (j % S) / S;
        const double dx = std::sin(a), dz = std::cos(a);
        Vertex v = mk((R + r * std::cos(c)) * dx, r * std::sin(c), (R + r * std::cos(c)) * dz, double(i) / RS, double(j) / S);
        if (smooth) { v.n[0] = std::cos(c) * dx; v.n[1] = std::sin(c); v.n[2] = std::cos(c) * dz; }
        return v;
    };
    for (int i = 0; i < RS; ++i)
        for (int j = 0; j < S; ++j) {
            Polygon p;
            p.material = material;
            p.v = {pt(i, j), pt(i + 1, j), pt(i + 1, j + 1), pt(i, j + 1)};
            const double a = 2 * kPi * (i + 0.5) / RS;
            const double ref[3] = {R * std::sin(a), 0, R * std::cos(a)};
            orientAway(p, ref);
            m.polys.push_back(std::move(p));
        }
    return m;
}

Mesh extrudePolygon(const std::vector<double>& xyIn, double depth, int material) {
    Mesh m;
    const size_t n = xyIn.size() / 2;
    if (n < 3) return m;
    std::vector<double> xy = xyIn;
    double area = 0;
    for (size_t i = 0; i < n; ++i) { const size_t j = (i + 1) % n; area += xy[i * 2] * xy[j * 2 + 1] - xy[j * 2] * xy[i * 2 + 1]; }
    if (area < 0) {   // counter-clockwise
        for (size_t i = 0; i < n / 2; ++i) { std::swap(xy[i * 2], xy[(n - 1 - i) * 2]); std::swap(xy[i * 2 + 1], xy[(n - 1 - i) * 2 + 1]); }
    }
    const std::vector<unsigned> tris = earClip(xy);
    const double up[3] = {0, 0, 1}, dn[3] = {0, 0, -1};
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
        Polygon top, bot;
        top.material = bot.material = material;
        for (int k = 0; k < 3; ++k) {
            const unsigned i = tris[t + static_cast<size_t>(k)];
            top.v.push_back(mk(xy[i * 2], xy[i * 2 + 1], 0, xy[i * 2], xy[i * 2 + 1]));
            bot.v.push_back(mk(xy[i * 2], xy[i * 2 + 1], -depth, xy[i * 2], xy[i * 2 + 1]));
        }
        orientAlong(top, up);
        orientAlong(bot, dn);
        m.polys.push_back(std::move(top));
        m.polys.push_back(std::move(bot));
    }
    for (size_t i = 0; i < n; ++i) {
        const size_t j = (i + 1) % n;
        const double ax = xy[i * 2], ay = xy[i * 2 + 1], bx = xy[j * 2], by = xy[j * 2 + 1];
        if (std::fabs(ax - bx) < 1e-12 && std::fabs(ay - by) < 1e-12) continue;
        Polygon w;
        w.material = material;
        w.v = {mk(ax, ay, 0, 0, 0), mk(bx, by, 0, 1, 0), mk(bx, by, -depth, 1, 1), mk(ax, ay, -depth, 0, 1)};
        const double out[3] = {by - ay, -(bx - ax), 0};
        orientAlong(w, out);
        m.polys.push_back(std::move(w));
    }
    return m;
}

Mesh spinPolygon(const std::vector<double>& xyIn, double degrees, int steps, int material) {
    Mesh m;
    const size_t n = xyIn.size() / 2;
    if (n < 3 || steps < 1) return m;
    std::vector<double> xy = xyIn;
    double area = 0;
    for (size_t i = 0; i < n; ++i) { const size_t j = (i + 1) % n; area += xy[i * 2] * xy[j * 2 + 1] - xy[j * 2] * xy[i * 2 + 1]; }
    if (area < 0)
        for (size_t i = 0; i < n / 2; ++i) { std::swap(xy[i * 2], xy[(n - 1 - i) * 2]); std::swap(xy[i * 2 + 1], xy[(n - 1 - i) * 2 + 1]); }
    const double total = std::min(std::max(degrees, 0.0), 360.0) * kPi / 180.0;
    const bool full = total >= 2 * kPi - 1e-9;
    const auto at = [&](size_t i, int s) {
        const double a = total * s / steps;
        return mk(xy[i * 2] * std::cos(a), xy[i * 2 + 1], -xy[i * 2] * std::sin(a), double(s) / steps, double(i) / double(n));
    };
    for (int s = 0; s < steps; ++s)
        for (size_t i = 0; i < n; ++i) {
            const size_t j = (i + 1) % n;
            const double am = total * (s + 0.5) / steps;
            const double ex = xy[j * 2 + 1] - xy[i * 2 + 1], ey = -(xy[j * 2] - xy[i * 2]);   // 2D outward (dy, -dx)
            const double out[3] = {ex * std::cos(am), ey, -ex * std::sin(am)};
            Polygon p;
            p.material = material;
            const Vertex q[4] = {at(i, s), at(j, s), at(j, s + 1), at(i, s + 1)};
            for (const Vertex& v : q) {
                if (!p.v.empty() && std::fabs(p.v.back().p[0] - v.p[0]) < 1e-9 && std::fabs(p.v.back().p[1] - v.p[1]) < 1e-9 && std::fabs(p.v.back().p[2] - v.p[2]) < 1e-9) continue;
                p.v.push_back(v);
            }
            if (p.v.size() > 1 && std::fabs(p.v.front().p[0] - p.v.back().p[0]) < 1e-9 && std::fabs(p.v.front().p[1] - p.v.back().p[1]) < 1e-9 && std::fabs(p.v.front().p[2] - p.v.back().p[2]) < 1e-9) p.v.pop_back();
            if (p.v.size() < 3) continue;
            orientAlong(p, out);
            m.polys.push_back(std::move(p));
        }
    if (!full) {
        const std::vector<unsigned> tris = earClip(xy);
        for (size_t t = 0; t + 2 < tris.size(); t += 3) {
            Polygon a, b;
            a.material = b.material = material;
            for (int k = 0; k < 3; ++k) { a.v.push_back(at(tris[t + static_cast<size_t>(k)], 0)); b.v.push_back(at(tris[t + static_cast<size_t>(k)], steps)); }
            const double start[3] = {0, 0, 1}, end[3] = {-std::sin(total), 0, -std::cos(total)};
            orientAlong(a, start);
            orientAlong(b, end);
            m.polys.push_back(std::move(a));
            m.polys.push_back(std::move(b));
        }
    }
    return m;
}

Mesh fromTriangles(const std::vector<float>& pos, const std::vector<float>& nrm, const std::vector<float>& uv, const std::vector<uint32_t>& idx,
                   const std::vector<int>& groups) {
    Mesh m;
    const size_t vc = pos.size() / 3;
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        Polygon p;
        p.material = groups.empty() ? 0 : groups[std::min(t / 3, groups.size() - 1)];
        for (size_t k = 0; k < 3; ++k) {
            const uint32_t i = idx[t + k];
            if (i >= vc) { p.v.clear(); break; }
            Vertex v = mk(pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]);
            if (nrm.size() >= vc * 3) { v.n[0] = nrm[i * 3]; v.n[1] = nrm[i * 3 + 1]; v.n[2] = nrm[i * 3 + 2]; }
            if (uv.size() >= vc * 2) { v.uv[0] = uv[i * 2]; v.uv[1] = uv[i * 2 + 1]; }
            p.v.push_back(v);
        }
        if (p.v.size() == 3) m.polys.push_back(std::move(p));
    }
    return m;
}

Soup toSoup(const Mesh& m, double snap) {
    Soup s;
    std::map<int, std::vector<const Polygon*>> byMat;
    for (const Polygon& p : m.polys) byMat[p.material].push_back(&p);
    std::map<std::array<long long, 8>, uint32_t> weld;
    const auto q = [](double v, double scale) { return static_cast<long long>(std::llround(v * scale)); };
    for (const auto& [mat, polys] : byMat) {
        const uint32_t start = static_cast<uint32_t>(s.idx.size());
        for (const Polygon* p : polys) {
            Plane pl;
            if (!planeOf(p->v, pl)) continue;
            std::vector<uint32_t> ids;
            std::vector<std::array<double, 3>> snapped;
            for (const Vertex& v : p->v) {
                double pp[3] = {v.p[0], v.p[1], v.p[2]};
                if (snap > 0) for (double& c : pp) c = std::round(c / snap) * snap;
                double nn[3] = {v.n[0], v.n[1], v.n[2]};
                if (len3(nn) < 1e-9) { nn[0] = pl.n[0]; nn[1] = pl.n[1]; nn[2] = pl.n[2]; }
                norm3(nn);
                const std::array<long long, 8> key = {q(pp[0], 1e6), q(pp[1], 1e6), q(pp[2], 1e6), q(nn[0], 1e4), q(nn[1], 1e4), q(nn[2], 1e4), q(v.uv[0], 1e5), q(v.uv[1], 1e5)};
                auto it = weld.find(key);
                if (it == weld.end()) {
                    it = weld.emplace(key, static_cast<uint32_t>(s.pos.size() / 3)).first;
                    s.pos.insert(s.pos.end(), {float(pp[0]), float(pp[1]), float(pp[2])});
                    s.nrm.insert(s.nrm.end(), {float(nn[0]), float(nn[1]), float(nn[2])});
                    s.uv.insert(s.uv.end(), {float(v.uv[0]), float(v.uv[1])});
                }
                ids.push_back(it->second);
                snapped.push_back({pp[0], pp[1], pp[2]});
            }
            for (size_t k = 1; k + 1 < ids.size(); ++k) {
                if (ids[0] == ids[k] || ids[k] == ids[k + 1] || ids[0] == ids[k + 1]) continue;
                const double e1[3] = {snapped[k][0] - snapped[0][0], snapped[k][1] - snapped[0][1], snapped[k][2] - snapped[0][2]};
                const double e2[3] = {snapped[k + 1][0] - snapped[0][0], snapped[k + 1][1] - snapped[0][1], snapped[k + 1][2] - snapped[0][2]};
                double cx[3];
                cross3(e1, e2, cx);
                if (len3(cx) < 1e-12) continue;
                s.idx.insert(s.idx.end(), {ids[0], ids[k], ids[k + 1]});
            }
        }
        const uint32_t count = static_cast<uint32_t>(s.idx.size()) - start;
        if (count == 0) continue;
        s.materials.push_back(mat);
        s.rangeStart.push_back(start);
        s.rangeCount.push_back(count);
    }
    return s;
}

}  // namespace aver::fmt::csg
