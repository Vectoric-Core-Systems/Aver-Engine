// Voronoi fracture: see aver/formats/Fracture.hpp.
#include "aver/formats/Fracture.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_map>

namespace aver::fmt::fracture {
namespace {

// splitmix64: the same stream on every platform.
struct Rng {
    u64 s;
    explicit Rng(u64 seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}
    u64 next() {
        u64 z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double u() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }   // [0,1)
    double range(double a, double b) { return a + (b - a) * u(); }
};

double dot(const Vec3d& a, const Vec3d& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3d sub(const Vec3d& a, const Vec3d& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3d cross(const Vec3d& a, const Vec3d& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double len(const Vec3d& a) { return std::sqrt(dot(a, a)); }
Vec3d vec(const double* p) { return {p[0], p[1], p[2]}; }

// Ray parity against the triangulated mesh. The ray leans off every axis so it rarely grazes an edge.
class Inside {
public:
    explicit Inside(const csg::Mesh& m) {
        for (const csg::Polygon& p : m.polys)
            for (size_t i = 2; i < p.v.size(); ++i) tris_.push_back({vec(p.v[0].p), vec(p.v[i - 1].p), vec(p.v[i].p)});
    }
    bool test(const Vec3d& o) const {
        const Vec3d d = {0.5773502691896258 + 0.0013, 0.5773502691896258 - 0.0007, 0.5773502691896258 + 0.0003};
        int hits = 0;
        for (const auto& t : tris_) {
            const Vec3d e1 = sub(t[1], t[0]), e2 = sub(t[2], t[0]);
            const Vec3d pv = cross(d, e2);
            const double det = dot(e1, pv);
            if (std::fabs(det) < 1e-18) continue;
            const double inv = 1.0 / det;
            const Vec3d tv = sub(o, t[0]);
            const double u = dot(tv, pv) * inv;
            if (u < 0 || u > 1) continue;
            const Vec3d qv = cross(tv, e1);
            const double v = dot(d, qv) * inv;
            if (v < 0 || u + v > 1) continue;
            if (dot(e2, qv) * inv > 0) ++hits;
        }
        return (hits & 1) != 0;
    }
private:
    std::vector<std::array<Vec3d, 3>> tris_;
};

constexpr int kTagBase = 1 << 24;   // cell faces carry kTagBase + neighbour seed until the contact areas are read off

struct CellPoly {
    std::vector<Vec3d> v;
    int tag = -1;                   // neighbour seed index, -1 = the bounding box
};

struct Cell {
    std::vector<CellPoly> polys;
    double maxDist(const Vec3d& c) const {
        double m = 0;
        for (const CellPoly& p : polys) for (const Vec3d& v : p.v) m = std::max(m, len(sub(v, c)));
        return m;
    }
};

Cell boxCell(const double lo[3], const double hi[3]) {
    Cell c;
    auto P = [&](int x, int y, int z) { return Vec3d{x ? hi[0] : lo[0], y ? hi[1] : lo[1], z ? hi[2] : lo[2]}; };
    const int f[6][4][3] = {   // outward, counter-clockwise from outside
        {{0,0,0},{0,0,1},{0,1,1},{0,1,0}}, {{1,0,0},{1,1,0},{1,1,1},{1,0,1}},
        {{0,0,0},{1,0,0},{1,0,1},{0,0,1}}, {{0,1,0},{0,1,1},{1,1,1},{1,1,0}},
        {{0,0,0},{0,1,0},{1,1,0},{1,0,0}}, {{0,0,1},{1,0,1},{1,1,1},{0,1,1}}};
    for (auto& q : f) {
        CellPoly p;
        for (auto& c3 : q) p.v.push_back(P(c3[0], c3[1], c3[2]));
        c.polys.push_back(std::move(p));
    }
    return c;
}

// Keeps the half-space n . x <= d. Returns false when nothing of the cell is left.
bool clipCell(Cell& cell, const Vec3d& n, double d, int tag, double eps, double dedupe) {
    double maxD = -1e300, minD = 1e300;
    for (const CellPoly& p : cell.polys)
        for (const Vec3d& v : p.v) { const double s = dot(n, v) - d; maxD = std::max(maxD, s); minD = std::min(minD, s); }
    if (maxD <= eps) return true;
    if (minD >= -eps) { cell.polys.clear(); return false; }

    Cell out;
    std::vector<Vec3d> cap;
    for (const CellPoly& p : cell.polys) {
        CellPoly q;
        q.tag = p.tag;
        const size_t N = p.v.size();
        for (size_t i = 0; i < N; ++i) {
            const Vec3d& a = p.v[i];
            const Vec3d& b = p.v[(i + 1) % N];
            const double da = dot(n, a) - d, db = dot(n, b) - d;
            if (da <= eps) { q.v.push_back(a); if (da >= -eps) cap.push_back(a); }
            if ((da < -eps && db > eps) || (da > eps && db < -eps)) {
                const double t = da / (da - db);
                const Vec3d x = {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
                q.v.push_back(x);
                cap.push_back(x);
            }
        }
        if (q.v.size() >= 3) out.polys.push_back(std::move(q));
    }
    // dedupe the cap points, then order them counter-clockwise about +n
    std::vector<Vec3d> pts;
    for (const Vec3d& c : cap) {
        bool dup = false;
        for (const Vec3d& e : pts) if (len(sub(c, e)) <= dedupe) { dup = true; break; }
        if (!dup) pts.push_back(c);
    }
    if (pts.size() >= 3) {
        Vec3d ctr = {0, 0, 0};
        for (const Vec3d& c : pts) for (int k = 0; k < 3; ++k) ctr[k] += c[k] / static_cast<double>(pts.size());
        Vec3d u = std::fabs(n[0]) < 0.9 ? Vec3d{1, 0, 0} : Vec3d{0, 1, 0};
        u = cross(n, u);
        const double ul = len(u);
        u = {u[0] / ul, u[1] / ul, u[2] / ul};
        const Vec3d w = cross(n, u);
        std::sort(pts.begin(), pts.end(), [&](const Vec3d& a, const Vec3d& b) {
            const Vec3d ra = sub(a, ctr), rb = sub(b, ctr);
            return std::atan2(dot(ra, w), dot(ra, u)) < std::atan2(dot(rb, w), dot(rb, u));
        });
        CellPoly capPoly;
        capPoly.v = std::move(pts);
        capPoly.tag = tag;
        out.polys.push_back(std::move(capPoly));
    }
    cell = std::move(out);
    return cell.polys.size() >= 4;
}

csg::Mesh cellMesh(const Cell& cell, int insideMaterial, double uvScale) {
    csg::Mesh m;
    for (const CellPoly& p : cell.polys) {
        csg::Polygon poly;
        poly.material = p.tag >= 0 ? kTagBase + p.tag : insideMaterial;
        Vec3d n = cross(sub(p.v[1], p.v[0]), sub(p.v[2], p.v[0]));
        const double nl = len(n);
        if (nl <= 0) continue;
        n = {n[0] / nl, n[1] / nl, n[2] / nl};
        const int drop = (std::fabs(n[0]) >= std::fabs(n[1]) && std::fabs(n[0]) >= std::fabs(n[2])) ? 0 : (std::fabs(n[1]) >= std::fabs(n[2]) ? 1 : 2);
        const int ax = drop == 0 ? 1 : 0, ay = drop == 2 ? 1 : 2;
        for (const Vec3d& v : p.v) {
            csg::Vertex cv;
            for (int k = 0; k < 3; ++k) cv.p[k] = v[k];
            cv.uv[0] = v[ax] * uvScale;
            cv.uv[1] = v[ay] * uvScale;
            poly.v.push_back(cv);
        }
        m.polys.push_back(std::move(poly));
    }
    return m;
}

double polyArea(const csg::Polygon& p) {
    double a[3] = {0, 0, 0};
    for (size_t i = 1; i + 1 < p.v.size(); ++i) {
        const Vec3d e1 = sub(vec(p.v[i].p), vec(p.v[0].p)), e2 = sub(vec(p.v[i + 1].p), vec(p.v[0].p));
        const Vec3d c = cross(e1, e2);
        for (int k = 0; k < 3; ++k) a[k] += c[k];
    }
    return 0.5 * std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
}

// ---- spatial hash of the seeds for the bounded neighbour search ----
class SeedGrid {
public:
    explicit SeedGrid(const std::vector<Vec3d>& seeds) : seeds_(seeds) {
        double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
        for (const Vec3d& s : seeds) for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], s[k]); hi[k] = std::max(hi[k], s[k]); }
        double maxExt = 0;
        for (int k = 0; k < 3; ++k) { origin_[k] = lo[k]; maxExt = std::max(maxExt, hi[k] - lo[k]); }
        if (maxExt <= 0) maxExt = 1;
        double prod = 1;
        for (int k = 0; k < 3; ++k) prod *= std::max(hi[k] - lo[k], maxExt * 1e-3);
        cs_ = std::max(std::cbrt(prod / static_cast<double>(std::max<size_t>(seeds.size(), 1))), maxExt / 256.0);
        for (int k = 0; k < 3; ++k) dim_[k] = static_cast<int>((hi[k] - lo[k]) / cs_) + 1;
        for (u32 i = 0; i < seeds.size(); ++i) cells_[key(coord(seeds[i], 0), coord(seeds[i], 1), coord(seeds[i], 2))].push_back(i);
    }
    // Other seeds in order of increasing distance within each ring of grid cells, rings walked outward until nothing closer than reach() can remain.
    void forNeighbours(u32 i, const std::function<double()>& reach, const std::function<void(u32, double)>& f) const {
        const int c[3] = {coord(seeds_[i], 0), coord(seeds_[i], 1), coord(seeds_[i], 2)};
        const int maxR = std::max({dim_[0], dim_[1], dim_[2]});
        std::vector<std::pair<double, u32>> ring;
        for (int r = 0; r <= maxR; ++r) {
            if (r >= 1 && (r - 1) * cs_ > reach()) break;
            ring.clear();
            for (int dx = -r; dx <= r; ++dx)
                for (int dy = -r; dy <= r; ++dy) {
                    const bool edge = std::abs(dx) == r || std::abs(dy) == r;
                    for (int dz = -r; dz <= r; dz += (edge || r == 0) ? 1 : 2 * r) {
                        auto it = cells_.find(key(c[0] + dx, c[1] + dy, c[2] + dz));
                        if (it == cells_.end()) continue;
                        for (u32 j : it->second) if (j != i) ring.push_back({len(sub(seeds_[j], seeds_[i])), j});
                    }
                }
            std::sort(ring.begin(), ring.end());
            for (auto& [d, j] : ring) { if (d > reach()) break; f(j, d); }
        }
    }
private:
    int coord(const Vec3d& p, int k) const { return static_cast<int>(std::floor((p[k] - origin_[k]) / cs_)); }
    static i64 key(int x, int y, int z) { return (static_cast<i64>(x + 1048576) << 42) ^ (static_cast<i64>(y + 1048576) << 21) ^ static_cast<i64>(z + 1048576); }
    const std::vector<Vec3d>& seeds_;
    double origin_[3] = {0, 0, 0};
    double cs_ = 1;
    int dim_[3] = {1, 1, 1};
    std::unordered_map<i64, std::vector<u32>> cells_;
};

// True when no polygon of the source can touch the cell: a conservative separating-plane test of every polygon against the cell's face planes.
// Then the cell lies wholly inside or wholly outside the solid and needs no boolean.
bool cellMissesSurface(const csg::Mesh& src, const std::vector<std::array<double, 6>>& boxes, const Cell& cell) {
    double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    struct Pl { Vec3d n; double d; };
    std::vector<Pl> planes;
    for (const CellPoly& p : cell.polys) {
        for (const Vec3d& v : p.v) for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], v[k]); hi[k] = std::max(hi[k], v[k]); }
        Vec3d n = cross(sub(p.v[1], p.v[0]), sub(p.v[2], p.v[0]));
        const double nl = len(n);
        if (nl <= 0) continue;
        n = {n[0] / nl, n[1] / nl, n[2] / nl};
        planes.push_back({n, dot(n, p.v[0])});
    }
    for (size_t i = 0; i < src.polys.size(); ++i) {
        const auto& b = boxes[i];
        if (b[0] > hi[0] || b[3] < lo[0] || b[1] > hi[1] || b[4] < lo[1] || b[2] > hi[2] || b[5] < lo[2]) continue;
        bool separated = false;
        for (const Pl& pl : planes) {
            bool allOut = true;
            for (const csg::Vertex& v : src.polys[i].v) if (dot(pl.n, vec(v.p)) - pl.d < 1e-9) { allOut = false; break; }
            if (allOut) { separated = true; break; }
        }
        if (!separated) return false;
    }
    return true;
}

}  // namespace

void measure(const csg::Mesh& m, double& volume, double& area, double centroid[3]) {
    volume = 0;
    area = 0;
    double c[3] = {0, 0, 0};
    for (const csg::Polygon& p : m.polys) {
        area += polyArea(p);
        for (size_t i = 1; i + 1 < p.v.size(); ++i) {
            const Vec3d a = vec(p.v[0].p), b = vec(p.v[i].p), d = vec(p.v[i + 1].p);
            const double v6 = dot(a, cross(b, d)) / 6.0;
            volume += v6;
            for (int k = 0; k < 3; ++k) c[k] += v6 * (a[k] + b[k] + d[k]) * 0.25;
        }
    }
    for (int k = 0; k < 3; ++k) centroid[k] = std::fabs(volume) > 1e-300 ? c[k] / volume : 0;
}

bool isClosed(const csg::Mesh& m, double tolerance) {
    double s[3] = {0, 0, 0}, area = 0;
    for (const csg::Polygon& p : m.polys) {
        area += polyArea(p);
        for (size_t i = 1; i + 1 < p.v.size(); ++i) {
            const Vec3d c = cross(sub(vec(p.v[i].p), vec(p.v[0].p)), sub(vec(p.v[i + 1].p), vec(p.v[0].p)));
            for (int k = 0; k < 3; ++k) s[k] += 0.5 * c[k];
        }
    }
    return std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]) <= tolerance * std::max(area, 1e-300);
}

std::vector<Vec3d> seedsUniform(const csg::Mesh& src, int count, u64 seed) {
    std::vector<Vec3d> out;
    const csg::Bounds b = csg::bounds(src);
    if (!b.valid || count <= 0) return out;
    Inside inside(src);
    Rng rng(seed);
    for (long attempts = 0; static_cast<int>(out.size()) < count && attempts < static_cast<long>(count) * 200; ++attempts) {
        const Vec3d p = {rng.range(b.lo[0], b.hi[0]), rng.range(b.lo[1], b.hi[1]), rng.range(b.lo[2], b.hi[2])};
        if (inside.test(p)) out.push_back(p);
    }
    return out;
}

std::vector<Vec3d> seedsRadial(const Vec3d& center, double maxRadius, int rings, int perShell, double jitter, u64 seed) {
    std::vector<Vec3d> out;
    if (rings <= 0 || perShell <= 0 || maxRadius <= 0) return out;
    Rng rng(seed);
    out.push_back(center);
    const double golden = 3.14159265358979323846 * (3.0 - std::sqrt(5.0));
    const double step = maxRadius / rings;
    for (int k = 1; k <= rings; ++k) {
        const int n = perShell * k;
        const double rad = step * k;
        const double phase = rng.range(0, 6.283185307179586);
        for (int i = 0; i < n; ++i) {
            const double z = 1.0 - (2.0 * i + 1.0) / n;
            const double rr = std::sqrt(std::max(0.0, 1.0 - z * z));
            const double a = golden * i + phase;
            const double r = rad + rng.range(-0.5, 0.5) * jitter * step;
            out.push_back({center[0] + r * rr * std::cos(a), center[1] + r * rr * std::sin(a), center[2] + r * z});
        }
    }
    return out;
}

std::vector<Vec3d> seedsBrick(const csg::Bounds& b, const Vec3d& cell, double stagger, double jitter, u64 seed) {
    std::vector<Vec3d> out;
    if (!b.valid || cell[0] <= 0 || cell[1] <= 0 || cell[2] <= 0) return out;
    Rng rng(seed);
    const int nz = std::max(1, static_cast<int>(std::ceil((b.hi[2] - b.lo[2]) / cell[2])));
    const int ny = std::max(1, static_cast<int>(std::ceil((b.hi[1] - b.lo[1]) / cell[1])));
    const int nx = std::max(1, static_cast<int>(std::ceil((b.hi[0] - b.lo[0]) / cell[0])));
    for (int z = 0; z < nz; ++z)
        for (int y = 0; y < ny; ++y)
            for (int x = 0; x < nx; ++x) {
                double px = b.lo[0] + (x + 0.5) * cell[0], py = b.lo[1] + (y + 0.5) * cell[1], pz = b.lo[2] + (z + 0.5) * cell[2];
                if (y & 1) px += stagger * cell[0];
                if (z & 1) py += stagger * cell[1];
                px += rng.range(-0.5, 0.5) * jitter * cell[0];
                py += rng.range(-0.5, 0.5) * jitter * cell[1];
                pz += rng.range(-0.5, 0.5) * jitter * cell[2];
                out.push_back({px, py, pz});
            }
    return out;
}

std::vector<Vec3d> seedsClustered(const csg::Mesh& src, const Vec3d& center, int count, u64 seed) {
    std::vector<Vec3d> out;
    const csg::Bounds b = csg::bounds(src);
    if (!b.valid || count <= 0) return out;
    Inside inside(src);
    Rng rng(seed);
    const double dx = b.hi[0] - b.lo[0], dy = b.hi[1] - b.lo[1], dz = b.hi[2] - b.lo[2];
    const double reach = 0.5 * std::sqrt(dx * dx + dy * dy + dz * dz);
    for (long attempts = 0; static_cast<int>(out.size()) < count && attempts < static_cast<long>(count) * 400; ++attempts) {
        const double u = rng.u();
        const double r = reach * 2.0 * u * u;
        const double z = rng.range(-1, 1), a = rng.range(0, 6.283185307179586), rr = std::sqrt(1 - z * z);
        const Vec3d p = {center[0] + r * rr * std::cos(a), center[1] + r * rr * std::sin(a), center[2] + r * z};
        if (inside.test(p)) out.push_back(p);
    }
    return out;
}

Result voronoi(const csg::Mesh& src, const std::vector<Vec3d>& seeds, const Options& opt) {
    Result res;
    const csg::Bounds bb = csg::bounds(src);
    if (!bb.valid || seeds.empty()) return res;

    int maxMat = 0;
    for (const csg::Polygon& p : src.polys) maxMat = std::max(maxMat, p.material);
    res.insideMaterial = opt.insideMaterial >= 0 ? opt.insideMaterial : maxMat + 1;

    double ext = 0;
    for (int k = 0; k < 3; ++k) ext = std::max(ext, bb.hi[k] - bb.lo[k]);
    const double pad = ext * 0.05 + 1e-9;
    double lo[3], hi[3];
    for (int k = 0; k < 3; ++k) { lo[k] = bb.lo[k] - pad; hi[k] = bb.hi[k] + pad; }
    const double eps = ext * 1e-10, dedupe = ext * 1e-8;
    double srcVol, srcArea, srcC[3];
    measure(src, srcVol, srcArea, srcC);
    const double minVol = std::fabs(srcVol) * opt.minVolumeFraction;
    const double minFace = ext * ext * 1e-12;

    const csg::Prepared prepared(src);
    const Inside insideTest(src);
    std::vector<std::array<double, 6>> boxes(src.polys.size());
    for (size_t i = 0; i < src.polys.size(); ++i) {
        auto& b = boxes[i];
        b = {1e300, 1e300, 1e300, -1e300, -1e300, -1e300};
        for (const csg::Vertex& v : src.polys[i].v) for (int k = 0; k < 3; ++k) { b[k] = std::min(b[k], v.p[k]); b[3 + k] = std::max(b[3 + k], v.p[k]); }
    }
    const SeedGrid grid(seeds);
    std::vector<u32> pieceOfSeed(seeds.size(), 0xFFFFFFFFu);
    std::vector<std::vector<std::pair<u32, double>>> contacts;   // per piece: (neighbour seed, area)

    for (u32 i = 0; i < seeds.size(); ++i) {
        Cell cell = boxCell(lo, hi);
        double R = cell.maxDist(seeds[i]);
        bool alive = true;
        grid.forNeighbours(i, [&] { return 2.0 * R; }, [&](u32 j, double) {
            if (!alive) return;
            const Vec3d n = sub(seeds[j], seeds[i]);
            if (len(n) <= dedupe) return;   // coincident seeds: the first keeps the cell
            const double d = 0.5 * (dot(seeds[j], seeds[j]) - dot(seeds[i], seeds[i]));
            if (!clipCell(cell, n, d, static_cast<int>(j), eps, dedupe)) { alive = false; return; }
            R = cell.maxDist(seeds[i]);
        });
        if (!alive || cell.polys.size() < 4) continue;

        csg::Mesh cm = cellMesh(cell, res.insideMaterial, opt.uvScale);
        csg::Mesh piece;
        if (cellMissesSurface(src, boxes, cell)) {
            // no surface inside the cell: it is all solid or all air, decided by one point
            Vec3d probe = {0, 0, 0};
            size_t cnt = 0;
            for (const CellPoly& p : cell.polys) for (const Vec3d& v : p.v) { for (int k = 0; k < 3; ++k) probe[k] += v[k]; ++cnt; }
            for (int k = 0; k < 3; ++k) probe[k] /= static_cast<double>(cnt);
            if (insideTest.test(probe)) piece = std::move(cm);
        } else {
            piece = prepared.combine(cm, csg::Op::Intersection);
        }
        if (piece.empty()) continue;
        std::vector<std::pair<u32, double>> touch;
        for (csg::Polygon& p : piece.polys) {
            if (p.material >= kTagBase) {
                touch.push_back({static_cast<u32>(p.material - kTagBase), polyArea(p)});
                p.material = res.insideMaterial;
            }
        }
        Piece pc;
        measure(piece, pc.volume, pc.area, pc.centroid);
        if (pc.volume <= minVol) continue;
        pc.mesh = std::move(piece);
        pc.seedIndex = i;
        pieceOfSeed[i] = static_cast<u32>(res.pieces.size());
        res.pieces.push_back(std::move(pc));
        contacts.push_back(std::move(touch));
    }

    // a shared face is seen from both cells: keep the larger reading so a numerically lost sliver on one side does not drop the link
    std::unordered_map<u64, double> shared;
    for (u32 a = 0; a < res.pieces.size(); ++a)
        for (auto& [seedJ, area] : contacts[a]) {
            const u32 b = pieceOfSeed[seedJ];
            if (b == 0xFFFFFFFFu || b == a) continue;
            const u64 k = (static_cast<u64>(std::min(a, b)) << 32) | std::max(a, b);
            double& cur = shared[k];
            cur = std::max(cur, area);
        }
    for (auto& [k, area] : shared)
        if (area > minFace) res.links.push_back({static_cast<u32>(k >> 32), static_cast<u32>(k & 0xFFFFFFFFu), area});
    std::sort(res.links.begin(), res.links.end(), [](const Link& x, const Link& y) { return x.a != y.a ? x.a < y.a : x.b < y.b; });
    return res;
}

Graph makeGraph(const Result& r) {
    Graph g;
    g.count = static_cast<u32>(r.pieces.size());
    g.links = r.links;
    g.anchor.assign(g.count, 0);
    return g;
}

void anchorByPlane(Graph& g, const Result& r, const Vec3d& normal, double offset, double tolerance) {
    const double nl = len(normal);
    if (nl <= 0) return;
    for (u32 i = 0; i < r.pieces.size() && i < g.count; ++i) {
        for (const csg::Polygon& p : r.pieces[i].mesh.polys) {
            bool hit = false;
            for (const csg::Vertex& v : p.v) if ((dot(normal, vec(v.p)) - offset) / nl <= tolerance) { hit = true; break; }
            if (hit) { g.anchor[i] = 1; break; }
        }
    }
}

std::vector<u32> components(u32 pieceCount, const std::vector<Link>& links, const std::vector<u8>& broken, u32* outCount) {
    std::vector<u32> parent(pieceCount);
    for (u32 i = 0; i < pieceCount; ++i) parent[i] = i;
    auto find = [&](u32 x) { while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; } return x; };
    for (size_t i = 0; i < links.size(); ++i) {
        if (i < broken.size() && broken[i]) continue;
        const u32 a = find(links[i].a), b = find(links[i].b);
        if (a != b) parent[std::max(a, b)] = std::min(a, b);
    }
    std::vector<u32> id(pieceCount, 0xFFFFFFFFu), out(pieceCount);
    u32 n = 0;
    for (u32 i = 0; i < pieceCount; ++i) {
        const u32 r = find(i);
        if (id[r] == 0xFFFFFFFFu) id[r] = n++;
        out[i] = id[r];
    }
    if (outCount) *outCount = n;
    return out;
}

}  // namespace aver::fmt::fracture
