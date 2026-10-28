#include "FabricMeshGen.hpp"

#include "aver/formats/AxisMap.hpp"

#include <cmath>

namespace aver::fabric {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Builder {
    SoupMesh& m;

    unsigned vert(const V3& p, const V3& n, double u, double v) {
        const unsigned i = static_cast<unsigned>(m.pos.size() / 3);
        m.pos.insert(m.pos.end(), {static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)});
        const V3 nn = n.normalized();
        m.nrm.insert(m.nrm.end(), {static_cast<float>(nn.x), static_cast<float>(nn.y), static_cast<float>(nn.z)});
        m.uv.insert(m.uv.end(), {static_cast<float>(u), static_cast<float>(v)});
        return i;
    }
    V3 p(unsigned i) const { return {m.pos[i * 3], m.pos[i * 3 + 1], m.pos[i * 3 + 2]}; }
    V3 n(unsigned i) const { return {m.nrm[i * 3], m.nrm[i * 3 + 1], m.nrm[i * 3 + 2]}; }

    // The triangle faces the way its vertex normals do; a sliver is dropped.
    void tri(unsigned a, unsigned b, unsigned c) {
        const V3 g = (p(b) - p(a)).cross(p(c) - p(a));
        if (g.length() < 1e-12) return;
        if (g.dot(n(a) + n(b) + n(c)) < 0) std::swap(b, c);
        m.idx.insert(m.idx.end(), {a, b, c});
    }
    void quad(unsigned a, unsigned b, unsigned c, unsigned d) { tri(a, b, c); tri(a, c, d); }

    // A flat rectangle: centre c, half-extent vectors u and v, outward normal. uvU and uvV are the (axis-aligned, unit) directions in which
    // the texture's U and V grow; V grows downwards in the image, as in Godot and glTF.
    void grid(const V3& c, const V3& u, const V3& v, const V3& normal, int nu, int nv, const V3& uvU, const V3& uvV) {
        nu = std::max(nu, 1);
        nv = std::max(nv, 1);
        const auto extent = [&](const V3& dir) { return 2 * std::max(std::fabs(u.dot(dir)), std::fabs(v.dot(dir))); };
        const double eu = std::max(extent(uvU), 1e-12), ev = std::max(extent(uvV), 1e-12);
        std::vector<unsigned> ids;
        for (int j = 0; j <= nv; ++j)
            for (int i = 0; i <= nu; ++i) {
                const double s = static_cast<double>(i) / nu, t = static_cast<double>(j) / nv;
                const V3 rel = u * (2 * s - 1) + v * (2 * t - 1);
                ids.push_back(vert(c + rel, normal, 0.5 + rel.dot(uvU) / eu, 0.5 + rel.dot(uvV) / ev));
            }
        for (int j = 0; j < nv; ++j)
            for (int i = 0; i < nu; ++i) {
                const unsigned a = ids[j * (nu + 1) + i], b = ids[j * (nu + 1) + i + 1];
                const unsigned cc = ids[(j + 1) * (nu + 1) + i + 1], d = ids[(j + 1) * (nu + 1) + i];
                quad(a, b, cc, d);
            }
    }
};

struct ProfilePoint {
    double r, y, nr, ny, v;
};

// A surface of revolution about +Y from a profile, with a duplicated seam so U reaches 1.
void revolve(Builder& b, const std::vector<ProfilePoint>& prof, int segs) {
    segs = std::max(segs, 3);
    std::vector<std::vector<unsigned>> rows;
    for (const ProfilePoint& pp : prof) {
        std::vector<unsigned> row;
        for (int i = 0; i <= segs; ++i) {
            const double u = static_cast<double>(i) / segs, a = u * 2 * kPi;
            row.push_back(b.vert({std::sin(a) * pp.r, pp.y, std::cos(a) * pp.r}, {std::sin(a) * pp.nr, pp.ny, std::cos(a) * pp.nr}, u, pp.v));
        }
        rows.push_back(std::move(row));
    }
    for (size_t k = 0; k + 1 < rows.size(); ++k)
        for (int i = 0; i < segs; ++i) b.quad(rows[k][i], rows[k][i + 1], rows[k + 1][i + 1], rows[k + 1][i]);
}

// A flat disc cap about +Y at height y facing up (+1) or down (-1).
void cap(Builder& b, double radius, double y, double facing, int segs) {
    segs = std::max(segs, 3);
    const V3 n{0, facing, 0};
    const unsigned centre = b.vert({0, y, 0}, n, 0.5, 0.5);
    std::vector<unsigned> ring;
    for (int i = 0; i <= segs; ++i) {
        const double a = static_cast<double>(i) / segs * 2 * kPi;
        ring.push_back(b.vert({std::sin(a) * radius, y, std::cos(a) * radius}, n, 0.5 + 0.5 * std::sin(a), 0.5 + 0.5 * std::cos(a)));
    }
    for (int i = 0; i < segs; ++i) b.tri(centre, ring[i], ring[i + 1]);
}

double num(const PropGetter& get, const char* name, double def) { return numOf(get(name), def); }
int inum(const PropGetter& get, const char* name, int def) { return static_cast<int>(std::lround(numOf(get(name), def))); }

V3 vec2As3(const Var* v, double dx, double dy) {
    if (v && (v->type == VType::Vector2 || v->type == VType::Vector2i)) return {v->num(0), v->num(1), 0};
    return {dx, dy, 0};
}

void planeFaces(Builder& b, const V3& size2, int orientation, const V3& offset, int sub0, int sub1) {
    const double hx = size2.x / 2, hy = size2.y / 2;
    if (orientation == 0)      b.grid(offset, {0, 0, hx}, {0, hy, 0}, {1, 0, 0}, sub0 + 1, sub1 + 1, {0, 0, -1}, {0, -1, 0});   // FACE_X: plane YZ
    else if (orientation == 2) b.grid(offset, {hx, 0, 0}, {0, hy, 0}, {0, 0, 1}, sub0 + 1, sub1 + 1, {1, 0, 0}, {0, -1, 0});    // FACE_Z: plane XY, upright
    else                       b.grid(offset, {hx, 0, 0}, {0, 0, hy}, {0, 1, 0}, sub0 + 1, sub1 + 1, {1, 0, 0}, {0, 0, 1});     // FACE_Y: plane XZ
}

}  // namespace

bool buildPrimitiveMesh(const std::string& type, const PropGetter& get, SoupMesh& out, std::string* why) {
    Builder b{out};
    const bool flip = boolOf(get("flip_faces"), false);

    if (type == "BoxMesh") {
        const V3 s = vec3Of(get("size"), {1, 1, 1});
        const int w = inum(get, "subdivide_width", 0), h = inum(get, "subdivide_height", 0), d = inum(get, "subdivide_depth", 0);
        const double hx = s.x / 2, hy = s.y / 2, hz = s.z / 2;
        // Each face has its texture upright as seen from outside (U to the viewer's right, V down).
        b.grid({hx, 0, 0}, {0, hy, 0}, {0, 0, hz}, {1, 0, 0}, h + 1, d + 1, {0, 0, -1}, {0, -1, 0});
        b.grid({-hx, 0, 0}, {0, hy, 0}, {0, 0, hz}, {-1, 0, 0}, h + 1, d + 1, {0, 0, 1}, {0, -1, 0});
        b.grid({0, hy, 0}, {hx, 0, 0}, {0, 0, hz}, {0, 1, 0}, w + 1, d + 1, {1, 0, 0}, {0, 0, 1});
        b.grid({0, -hy, 0}, {hx, 0, 0}, {0, 0, hz}, {0, -1, 0}, w + 1, d + 1, {1, 0, 0}, {0, 0, -1});
        b.grid({0, 0, hz}, {hx, 0, 0}, {0, hy, 0}, {0, 0, 1}, w + 1, h + 1, {1, 0, 0}, {0, -1, 0});
        b.grid({0, 0, -hz}, {hx, 0, 0}, {0, hy, 0}, {0, 0, -1}, w + 1, h + 1, {-1, 0, 0}, {0, -1, 0});
    } else if (type == "SphereMesh") {
        const double radius = num(get, "radius", 0.5), height = num(get, "height", 1.0);
        const int segs = inum(get, "radial_segments", 64), rings = inum(get, "rings", 32);
        const bool hemi = boolOf(get("is_hemisphere"), false);
        const double scale = height * (hemi ? 1.0 : 0.5);
        std::vector<ProfilePoint> prof;
        const int bands = std::max(rings, 2) + 1;
        for (int j = 0; j <= bands; ++j) {
            const double v = static_cast<double>(j) / bands, w = std::sin(kPi * v), y = scale * std::cos(kPi * v);
            if (hemi && y < 0.0) prof.push_back({radius * w, 0.0, 0.0, -1.0, v});
            else                 prof.push_back({radius * w, y, w * scale, radius * (y / scale), v});
        }
        revolve(b, prof, segs);
    } else if (type == "CapsuleMesh") {
        const double radius = num(get, "radius", 0.5), height = num(get, "height", 2.0);
        const int segs = inum(get, "radial_segments", 64), rings = std::max(inum(get, "rings", 8), 2);
        const double mid = std::max(height - 2 * radius, 0.0);
        std::vector<ProfilePoint> prof;
        for (int k = 0; k <= rings; ++k) {   // top pole down to the equator
            const double th = (kPi / 2) * k / rings;
            prof.push_back({radius * std::sin(th), mid / 2 + radius * std::cos(th), std::sin(th), std::cos(th), 0.5 * k / rings});
        }
        for (int k = 0; k <= rings; ++k) {   // equator down to the bottom pole
            const double th = kPi / 2 + (kPi / 2) * k / rings;
            prof.push_back({radius * std::sin(th), -mid / 2 + radius * std::cos(th), std::sin(th), std::cos(th), 0.5 + 0.5 * k / rings});
        }
        revolve(b, prof, segs);
    } else if (type == "CylinderMesh") {
        const double top = num(get, "top_radius", 0.5), bottom = num(get, "bottom_radius", 0.5), height = num(get, "height", 2.0);
        const int segs = inum(get, "radial_segments", 64), rings = std::max(inum(get, "rings", 4), 1);
        const double len = std::sqrt(height * height + (bottom - top) * (bottom - top));
        const double nr = len > 0 ? height / len : 1.0, ny = len > 0 ? (bottom - top) / len : 0.0;
        std::vector<ProfilePoint> prof;
        for (int k = 0; k <= rings; ++k) {
            const double t = static_cast<double>(k) / rings;
            prof.push_back({top + (bottom - top) * t, height / 2 - t * height, nr, ny, t});
        }
        revolve(b, prof, segs);
        if (boolOf(get("cap_top"), true) && top > 0) cap(b, top, height / 2, 1.0, segs);
        if (boolOf(get("cap_bottom"), true) && bottom > 0) cap(b, bottom, -height / 2, -1.0, segs);
    } else if (type == "PlaneMesh") {
        planeFaces(b, vec2As3(get("size"), 2, 2), inum(get, "orientation", 1), vec3Of(get("center_offset"), {0, 0, 0}),
                   inum(get, "subdivide_width", 0), inum(get, "subdivide_depth", 0));
    } else if (type == "QuadMesh") {
        planeFaces(b, vec2As3(get("size"), 1, 1), inum(get, "orientation", 2), vec3Of(get("center_offset"), {0, 0, 0}), 0, 0);
    } else if (type == "PrismMesh") {
        const double l2r = num(get, "left_to_right", 0.5);
        const V3 s = vec3Of(get("size"), {1, 1, 1});
        const double ax = -s.x / 2 + l2r * s.x, ay = s.y / 2, hz = s.z / 2;
        const V3 bl{-s.x / 2, -s.y / 2, 0}, br{s.x / 2, -s.y / 2, 0}, ap{ax, ay, 0};
        for (const double z : {hz, -hz}) {
            const V3 n{0, 0, z > 0 ? 1.0 : -1.0};
            b.tri(b.vert({bl.x, bl.y, z}, n, 0, 0), b.vert({br.x, br.y, z}, n, 1, 0), b.vert({ap.x, ap.y, z}, n, l2r, 1));
        }
        const V3 nb{0, -1, 0};
        b.quad(b.vert({bl.x, bl.y, hz}, nb, 0, 0), b.vert({br.x, br.y, hz}, nb, 1, 0), b.vert({br.x, br.y, -hz}, nb, 1, 1),
               b.vert({bl.x, bl.y, -hz}, nb, 0, 1));
        const V3 nl = V3{-s.y, ax + s.x / 2, 0}.normalized(), nrr = V3{s.y, s.x / 2 - ax, 0}.normalized();
        b.quad(b.vert({bl.x, bl.y, hz}, nl, 0, 0), b.vert({ap.x, ap.y, hz}, nl, 1, 0), b.vert({ap.x, ap.y, -hz}, nl, 1, 1),
               b.vert({bl.x, bl.y, -hz}, nl, 0, 1));
        b.quad(b.vert({br.x, br.y, hz}, nrr, 0, 0), b.vert({ap.x, ap.y, hz}, nrr, 1, 0), b.vert({ap.x, ap.y, -hz}, nrr, 1, 1),
               b.vert({br.x, br.y, -hz}, nrr, 0, 1));
    } else if (type == "TorusMesh") {
        const double inner = num(get, "inner_radius", 0.5), outer = num(get, "outer_radius", 1.0);
        const int rings = std::max(inum(get, "rings", 64), 3), segs = std::max(inum(get, "ring_segments", 32), 3);
        const double R = (inner + outer) / 2, r = std::fabs(outer - inner) / 2;
        std::vector<unsigned> ids;
        for (int i = 0; i <= rings; ++i)
            for (int j = 0; j <= segs; ++j) {
                const double a = static_cast<double>(i) / rings * 2 * kPi, c = static_cast<double>(j) / segs * 2 * kPi;
                const V3 d{std::sin(a), 0, std::cos(a)};
                const V3 nrm = d * std::cos(c) + V3{0, std::sin(c), 0};
                ids.push_back(b.vert(d * R + nrm * r, nrm, static_cast<double>(i) / rings, static_cast<double>(j) / segs));
            }
        for (int i = 0; i < rings; ++i)
            for (int j = 0; j < segs; ++j)
                b.quad(ids[i * (segs + 1) + j], ids[(i + 1) * (segs + 1) + j], ids[(i + 1) * (segs + 1) + j + 1], ids[i * (segs + 1) + j + 1]);
    } else if (type == "TubeTrailMesh") {
        // G4 R7: the static shape of a trail mesh: a straight tube along Y, centred. The particle system's bend and the width curve are not applied.
        const double radius = num(get, "radius", 0.5), len = num(get, "section_length", 0.2);
        const int radial = std::max(inum(get, "radial_steps", 8), 3), sections = std::max(inum(get, "sections", 5), 1), rings = std::max(inum(get, "section_rings", 3), 1);
        const double total = sections * len;
        const int rows = sections * rings;
        std::vector<ProfilePoint> prof;
        for (int k = 0; k <= rows; ++k) {
            const double t = static_cast<double>(k) / rows;
            prof.push_back({radius, total / 2 - t * total, 1.0, 0.0, t});
        }
        revolve(b, prof, radial);
        if (boolOf(get("cap_top"), true)) cap(b, radius, total / 2, 1.0, radial);
        if (boolOf(get("cap_bottom"), true)) cap(b, radius, -total / 2, -1.0, radial);
    } else if (type == "RibbonTrailMesh") {
        const double size = num(get, "size", 1.0), len = num(get, "section_length", 0.2);
        const int sections = std::max(inum(get, "sections", 5), 1), segs = std::max(inum(get, "section_segments", 3), 1);
        const double total = sections * len;
        b.grid({0, 0, 0}, {size / 2, 0, 0}, {0, total / 2, 0}, {0, 0, 1}, 1, sections * segs, {1, 0, 0}, {0, -1, 0});
        if (inum(get, "shape", 1) == 1) b.grid({0, 0, 0}, {0, 0, size / 2}, {0, total / 2, 0}, {1, 0, 0}, 1, sections * segs, {0, 0, -1}, {0, -1, 0});
    } else {
        if (why) *why = type + " is not a primitive mesh the converter generates";
        return false;
    }

    if (flip) {
        for (size_t i = 0; i + 2 < out.idx.size(); i += 3) std::swap(out.idx[i + 1], out.idx[i + 2]);
        for (float& f : out.nrm) f = -f;
    }
    return !out.idx.empty();
}

fmt::OcMeshData toOcMesh(const SoupMesh& soup, const std::string& materialSlot) {
    fmt::OcMeshData m;
    const size_t n = soup.pos.size() / 3;
    m.positions.resize(n * 3);
    m.normals.resize(n * 3);
    m.uvs = soup.uv;
    for (size_t i = 0; i < n; ++i) {
        const Vec3 p = fmt::axis::position(soup.pos[i * 3], soup.pos[i * 3 + 1], soup.pos[i * 3 + 2]);
        Vec3 nn = fmt::axis::direction(soup.nrm[i * 3], soup.nrm[i * 3 + 1], soup.nrm[i * 3 + 2]);
        m.positions[i * 3] = p.x; m.positions[i * 3 + 1] = p.y; m.positions[i * 3 + 2] = p.z;
        m.normals[i * 3] = nn.x; m.normals[i * 3 + 1] = nn.y; m.normals[i * 3 + 2] = nn.z;
    }
    // The axis map has determinant -1: every triangle's winding reverses.
    m.indices.reserve(soup.idx.size());
    for (size_t i = 0; i + 2 < soup.idx.size(); i += 3) {
        m.indices.push_back(soup.idx[i]);
        m.indices.push_back(soup.idx[i + 2]);
        m.indices.push_back(soup.idx[i + 1]);
    }
    m.materialSlots.push_back(materialSlot);
    fmt::OcMeshSubmesh sm;
    sm.name = materialSlot;
    sm.materialSlot = 0;
    sm.indexStart = 0;
    sm.indexCount = static_cast<u32>(m.indices.size());
    sm.baseVertex = 0;
    sm.vertexCount = static_cast<u32>(n);
    m.submeshes.push_back(std::move(sm));
    m.computeBounds();
    // Godot's PrimitiveMesh arrays carry tangents; so do these (R1). UV0 only: Godot adds UV2 only for add_uv2.
    fmt::generateTangents(m);
    return m;
}

bool polygonIsConvex(const std::vector<std::pair<double, double>>& p) {
    const size_t n = p.size();
    if (n < 3) return false;
    double sign = 0;
    for (size_t i = 0; i < n; ++i) {
        const auto& a = p[i];
        const auto& b = p[(i + 1) % n];
        const auto& c = p[(i + 2) % n];
        const double cr = (b.first - a.first) * (c.second - b.second) - (b.second - a.second) * (c.first - b.first);
        if (std::fabs(cr) < 1e-12) continue;
        if (sign == 0) sign = cr > 0 ? 1 : -1;
        else if ((cr > 0 ? 1 : -1) != sign) return false;
    }
    return true;
}

std::vector<unsigned> polygonTriangulate(const std::vector<std::pair<double, double>>& p) {
    std::vector<unsigned> out;
    const size_t n = p.size();
    if (n < 3) return out;
    double area = 0;
    for (size_t i = 0; i < n; ++i) {
        const auto& a = p[i];
        const auto& b = p[(i + 1) % n];
        area += a.first * b.second - b.first * a.second;
    }
    std::vector<unsigned> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<unsigned>(i);
    if (area < 0) std::reverse(v.begin(), v.end());   // counter-clockwise
    const auto cross = [&](unsigned a, unsigned b, unsigned c) {
        return (p[b].first - p[a].first) * (p[c].second - p[a].second) - (p[b].second - p[a].second) * (p[c].first - p[a].first);
    };
    const auto inside = [&](unsigned a, unsigned b, unsigned c, unsigned q) {
        return cross(a, b, q) >= 0 && cross(b, c, q) >= 0 && cross(c, a, q) >= 0;
    };
    size_t guard = 0;
    while (v.size() > 3 && guard++ < n * n + 8) {
        bool clipped = false;
        for (size_t i = 0; i < v.size(); ++i) {
            const unsigned a = v[(i + v.size() - 1) % v.size()], b = v[i], c = v[(i + 1) % v.size()];
            if (cross(a, b, c) <= 1e-12) continue;
            bool ear = true;
            for (const unsigned q : v)
                if (q != a && q != b && q != c && inside(a, b, c, q)) { ear = false; break; }
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

}  // namespace aver::fabric
