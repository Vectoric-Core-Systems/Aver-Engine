// `.ocfrac` build, validate, read and write: see aver/formats/OcFrac.hpp.
#include "aver/formats/OcFrac.hpp"

#include <cmath>
#include <cstring>
#include <map>

namespace aver::fmt {
namespace {

bool fail(std::string* why, const char* msg) {
    if (why) *why = msg;
    return false;
}

template <typename T> void put(std::vector<u8>& b, T v) {
    const usize at = b.size();
    b.resize(at + sizeof(T));
    std::memcpy(b.data() + at, &v, sizeof(T));
}

template <typename T> void putArray(std::vector<u8>& b, const std::vector<T>& v) {
    if (v.empty()) return;
    const usize at = b.size();
    b.resize(at + v.size() * sizeof(T));
    std::memcpy(b.data() + at, v.data(), v.size() * sizeof(T));
}

struct Cursor {
    const u8* p = nullptr;
    usize left = 0;
    bool ok = true;
    template <typename T> T take() {
        T v{};
        if (left < sizeof(T)) { ok = false; return v; }
        std::memcpy(&v, p, sizeof(T));
        p += sizeof(T);
        left -= sizeof(T);
        return v;
    }
    template <typename T> bool takeArray(std::vector<T>& v, usize count) {
        if (count > left / sizeof(T)) { ok = false; return false; }
        v.resize(count);
        if (count) std::memcpy(v.data(), p, count * sizeof(T));
        p += count * sizeof(T);
        left -= count * sizeof(T);
        return true;
    }
};

// FHDR: u32 version, u32 verts, u32 indices, u32 ranges, u32 pieces, u32 hullPoints, u32 links, i32 insideMaterial, f32 bounds[6], f32 sourceVolume
constexpr u32 kVersion = 1;

}  // namespace

OcFracData buildOcFrac(const fracture::Result& r, const fracture::Graph& graph, const OcFracBuild& opt) {
    OcFracData d;
    d.insideMaterial = r.insideMaterial;
    bool first = true;
    double vol = 0;
    for (const fracture::Piece& pc : r.pieces) {
        OcFracPiece out;
        out.volume = static_cast<f32>(pc.volume);
        out.area = static_cast<f32>(pc.area);
        for (int k = 0; k < 3; ++k) out.centroid[k] = static_cast<f32>(pc.centroid[k]);
        vol += pc.volume;

        const csg::Soup soup = csg::toSoup(pc.mesh, opt.snap);
        const u32 baseVert = d.vertexCount();
        for (usize v = 0; v < soup.pos.size() / 3; ++v) {
            for (int k = 0; k < 3; ++k) d.verts.push_back(soup.pos[v * 3 + k]);
            for (int k = 0; k < 3; ++k) d.verts.push_back(soup.nrm[v * 3 + k]);
            for (int k = 0; k < 2; ++k) d.verts.push_back(soup.uv[v * 2 + k]);
            for (int k = 0; k < 3; ++k) {
                const f32 c = soup.pos[v * 3 + k];
                if (first) { d.boundsMin[k] = d.boundsMax[k] = c; }
                else { if (c < d.boundsMin[k]) d.boundsMin[k] = c; if (c > d.boundsMax[k]) d.boundsMax[k] = c; }
            }
            first = false;
        }
        out.rangeStart = static_cast<u32>(d.ranges.size());
        for (usize m = 0; m < soup.materials.size(); ++m) {
            OcFracRange rg;
            rg.material = soup.materials[m];
            rg.indexStart = static_cast<u32>(d.indices.size());
            rg.indexCount = soup.rangeCount[m];
            for (u32 i = 0; i < soup.rangeCount[m]; ++i) d.indices.push_back(baseVert + soup.idx[soup.rangeStart[m] + i]);
            d.ranges.push_back(rg);
        }
        out.rangeCount = static_cast<u32>(d.ranges.size()) - out.rangeStart;

        // hull points: the piece's distinct positions (for a convex piece these are its hull vertices; the physics hull builder reduces the rest)
        out.hullStart = static_cast<u32>(d.hull.size() / 3);
        std::map<std::array<long long, 3>, bool> seen;
        for (const csg::Polygon& p : pc.mesh.polys)
            for (const csg::Vertex& v : p.v) {
                const std::array<long long, 3> key = {std::llround(v.p[0] * 1e6), std::llround(v.p[1] * 1e6), std::llround(v.p[2] * 1e6)};
                if (!seen.emplace(key, true).second) continue;
                for (int k = 0; k < 3; ++k) d.hull.push_back(static_cast<f32>(v.p[k]));
            }
        out.hullCount = static_cast<u32>(d.hull.size() / 3) - out.hullStart;
        d.pieces.push_back(out);
    }
    for (const fracture::Link& l : graph.links) {
        OcFracLink o;
        o.a = l.a;
        o.b = l.b;
        o.area = static_cast<f32>(l.area);
        o.threshold = static_cast<f32>(opt.toughness * l.area);
        d.links.push_back(o);
    }
    d.anchors = graph.anchor;
    d.anchors.resize(d.pieces.size(), 0);
    d.sourceVolume = static_cast<f32>(vol);
    return d;
}

bool validateOcFrac(const OcFracData& d, std::string* why) {
    if (d.pieces.empty()) return fail(why, "no pieces");
    if (d.pieces.size() > kOcFracMaxPieces) return fail(why, "too many pieces");
    if (d.verts.size() % 8 != 0) return fail(why, "vertex pool is not a whole number of vertices");
    if (d.indices.size() % 3 != 0) return fail(why, "index count is not a multiple of 3");
    if (d.hull.size() % 3 != 0) return fail(why, "hull pool is not a whole number of points");
    if (d.anchors.size() != d.pieces.size()) return fail(why, "anchor flags do not match the piece count");
    for (f32 v : d.verts) if (!std::isfinite(v)) return fail(why, "non-finite vertex data");
    for (f32 v : d.hull) if (!std::isfinite(v)) return fail(why, "non-finite hull point");
    const u32 vc = d.vertexCount();
    for (u32 i : d.indices) if (i >= vc) return fail(why, "index past the vertex pool");
    for (const OcFracRange& r : d.ranges) {
        if (r.indexStart > d.indices.size() || r.indexCount > d.indices.size() - r.indexStart) return fail(why, "range leaves the index pool");
        if (r.indexCount % 3 != 0) return fail(why, "range is not whole triangles");
    }
    const u32 hp = static_cast<u32>(d.hull.size() / 3);
    for (const OcFracPiece& p : d.pieces) {
        if (p.rangeStart > d.ranges.size() || p.rangeCount > d.ranges.size() - p.rangeStart) return fail(why, "piece range leaves the range table");
        if (p.hullStart > hp || p.hullCount > hp - p.hullStart) return fail(why, "piece hull leaves the hull pool");
        if (p.hullCount < 4) return fail(why, "piece hull has fewer than 4 points");
        if (!(p.volume > 0) || !std::isfinite(p.volume)) return fail(why, "piece volume must be positive");
        for (int k = 0; k < 3; ++k) if (!std::isfinite(p.centroid[k])) return fail(why, "non-finite centroid");
    }
    for (const OcFracLink& l : d.links) {
        if (l.a >= d.pieces.size() || l.b >= d.pieces.size() || l.a == l.b) return fail(why, "link names a bad piece");
        if (!(l.threshold >= 0) || !std::isfinite(l.threshold) || !std::isfinite(l.area)) return fail(why, "link threshold or area is not usable");
    }
    return true;
}

bool writeOcFrac(const OcFracData& d, std::vector<u8>& out, std::string* why) {
    if (!validateOcFrac(d, why)) return false;
    std::vector<u8> hdr;
    put<u32>(hdr, kVersion);
    put<u32>(hdr, d.vertexCount());
    put<u32>(hdr, static_cast<u32>(d.indices.size()));
    put<u32>(hdr, static_cast<u32>(d.ranges.size()));
    put<u32>(hdr, static_cast<u32>(d.pieces.size()));
    put<u32>(hdr, static_cast<u32>(d.hull.size() / 3));
    put<u32>(hdr, static_cast<u32>(d.links.size()));
    put<i32>(hdr, d.insideMaterial);
    for (int k = 0; k < 3; ++k) put<f32>(hdr, d.boundsMin[k]);
    for (int k = 0; k < 3; ++k) put<f32>(hdr, d.boundsMax[k]);
    put<f32>(hdr, d.sourceVolume);

    std::vector<u8> vtx, idx, rng, pcs, hul, lnk;
    putArray(vtx, d.verts);
    putArray(idx, d.indices);
    for (const OcFracRange& r : d.ranges) { put<i32>(rng, r.material); put<u32>(rng, r.indexStart); put<u32>(rng, r.indexCount); }
    for (const OcFracPiece& p : d.pieces) {
        put<f32>(pcs, p.volume); put<f32>(pcs, p.area);
        for (int k = 0; k < 3; ++k) put<f32>(pcs, p.centroid[k]);
        put<u32>(pcs, p.rangeStart); put<u32>(pcs, p.rangeCount); put<u32>(pcs, p.hullStart); put<u32>(pcs, p.hullCount);
    }
    putArray(hul, d.hull);
    for (const OcFracLink& l : d.links) { put<u32>(lnk, l.a); put<u32>(lnk, l.b); put<f32>(lnk, l.area); put<f32>(lnk, l.threshold); }

    Avr1File file;
    file.subtype = kAvrSubtypeFrac;
    file.add(kOcFracChunkHeader, std::move(hdr), kAvrChunkRequired);
    file.add(kOcFracChunkVerts, std::move(vtx), kAvrChunkRequired);
    file.add(kOcFracChunkIndices, std::move(idx), kAvrChunkRequired);
    file.add(kOcFracChunkRanges, std::move(rng), kAvrChunkRequired);
    file.add(kOcFracChunkPieces, std::move(pcs), kAvrChunkRequired);
    file.add(kOcFracChunkHull, std::move(hul), kAvrChunkRequired);
    file.add(kOcFracChunkLinks, std::move(lnk), 0);
    file.add(kOcFracChunkAnchors, std::vector<u8>(d.anchors), 0);
    return writeAvr1(file, out, why);
}

bool saveOcFrac(const std::string& path, const OcFracData& d, std::string* why) {
    std::vector<u8> bytes;
    if (!writeOcFrac(d, bytes, why)) return false;
    Avr1File file;
    if (!parseAvr1(bytes.data(), bytes.size(), file, why)) return false;
    return saveAvr1(path, file, why);
}

bool parseOcFrac(const u8* bytes, usize size, OcFracData& out, std::string* why) {
    Avr1File file;
    if (!parseAvr1(bytes, size, file, why)) return false;
    if (file.subtype != kAvrSubtypeFrac) return fail(why, "not an .ocfrac container (wrong subtype)");
    const AvrChunk* hdr = file.find(kOcFracChunkHeader);
    const AvrChunk* vtx = file.find(kOcFracChunkVerts);
    const AvrChunk* idx = file.find(kOcFracChunkIndices);
    const AvrChunk* rng = file.find(kOcFracChunkRanges);
    const AvrChunk* pcs = file.find(kOcFracChunkPieces);
    const AvrChunk* hul = file.find(kOcFracChunkHull);
    if (!hdr || !vtx || !idx || !rng || !pcs || !hul) return fail(why, "a required chunk is missing");

    Cursor h{hdr->data.data(), hdr->data.size()};
    if (h.take<u32>() != kVersion) return fail(why, "unsupported .ocfrac version");
    const u32 nv = h.take<u32>(), ni = h.take<u32>(), nr = h.take<u32>(), np = h.take<u32>(), nh = h.take<u32>(), nl = h.take<u32>();
    OcFracData d;
    d.insideMaterial = h.take<i32>();
    for (int k = 0; k < 3; ++k) d.boundsMin[k] = h.take<f32>();
    for (int k = 0; k < 3; ++k) d.boundsMax[k] = h.take<f32>();
    d.sourceVolume = h.take<f32>();
    if (!h.ok) return fail(why, "FHDR ended early");
    if (np > kOcFracMaxPieces) return fail(why, "too many pieces");

    Cursor c{vtx->data.data(), vtx->data.size()};
    if (!c.takeArray(d.verts, static_cast<usize>(nv) * 8)) return fail(why, "FVTX is shorter than the header says");
    c = Cursor{idx->data.data(), idx->data.size()};
    if (!c.takeArray(d.indices, ni)) return fail(why, "FIDX is shorter than the header says");
    c = Cursor{rng->data.data(), rng->data.size()};
    for (u32 i = 0; i < nr; ++i) {
        OcFracRange r;
        r.material = c.take<i32>(); r.indexStart = c.take<u32>(); r.indexCount = c.take<u32>();
        d.ranges.push_back(r);
    }
    if (!c.ok) return fail(why, "FSUB is shorter than the header says");
    c = Cursor{pcs->data.data(), pcs->data.size()};
    for (u32 i = 0; i < np; ++i) {
        OcFracPiece p;
        p.volume = c.take<f32>(); p.area = c.take<f32>();
        for (int k = 0; k < 3; ++k) p.centroid[k] = c.take<f32>();
        p.rangeStart = c.take<u32>(); p.rangeCount = c.take<u32>(); p.hullStart = c.take<u32>(); p.hullCount = c.take<u32>();
        d.pieces.push_back(p);
    }
    if (!c.ok) return fail(why, "FPCS is shorter than the header says");
    c = Cursor{hul->data.data(), hul->data.size()};
    if (!c.takeArray(d.hull, static_cast<usize>(nh) * 3)) return fail(why, "FHUL is shorter than the header says");

    if (const AvrChunk* lk = file.find(kOcFracChunkLinks)) {
        c = Cursor{lk->data.data(), lk->data.size()};
        for (u32 i = 0; i < nl; ++i) {
            OcFracLink l;
            l.a = c.take<u32>(); l.b = c.take<u32>(); l.area = c.take<f32>(); l.threshold = c.take<f32>();
            d.links.push_back(l);
        }
        if (!c.ok) return fail(why, "FLNK is shorter than the header says");
    } else if (nl != 0) {
        return fail(why, "FLNK is missing");
    }
    if (const AvrChunk* an = file.find(kOcFracChunkAnchors)) d.anchors = an->data;
    d.anchors.resize(d.pieces.size(), 0);
    if (!validateOcFrac(d, why)) return false;
    out = std::move(d);
    return true;
}

bool loadOcFrac(const std::string& path, OcFracData& out, std::string* why) {
    Avr1File file;
    if (!loadAvr1(path, file, why)) return false;
    std::vector<u8> bytes;
    if (!writeAvr1(file, bytes, why)) return false;
    return parseOcFrac(bytes.data(), bytes.size(), out, why);
}

}  // namespace aver::fmt
