// Reader and writer for .ocnav. See the header for why navigation is a grid and why it is baked.

#include "aver/formats/OcNav.hpp"

#include "aver/formats/Avr1.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>

namespace aver::fmt {
namespace {

constexpr u32 kChunkNHDR = avrFourCC("NHDR");
constexpr u32 kChunkNCEL = avrFourCC("NCEL");
// Reserved and unwritten in v1: off-mesh links (a jump, a ladder, a dropped-down ledge). Named here
// rather than left for later so the shape of the answer is on record -- and, being optional, a v1
// file stays byte-identical to what a v2 reader would produce from it.
constexpr u32 kChunkNLNK = avrFourCC("NLNK");
constexpr u32 kSubtypeNav = avrFourCC("NAV ");

// Bytes per cell on disk. Written explicitly rather than as sizeof(OcNavCell), so a change to the
// struct cannot silently change the format.
constexpr usize kCellBytes = 8;

bool fail(std::string* why, std::string m) { if (why) *why = std::move(m); return false; }

struct W {
    std::vector<u8>& b;
    void u8v (u8 v)  { b.push_back(v); }
    void u16v(u16 v) { b.push_back(u8(v)); b.push_back(u8(v >> 8)); }
    void u32v(u32 v) { for (int i = 0; i < 4; ++i) b.push_back(u8(v >> (i * 8))); }
    void f32v(f32 v) { u32 x; std::memcpy(&x, &v, 4); u32v(x); }
};

struct R {
    const u8* p; const u8* e; bool ok = true;
    bool need(usize n) { if (usize(e - p) < n) { ok = false; return false; } return true; }
    u8  u8v () { if (!need(1)) return 0; return *p++; }
    u16 u16v() { if (!need(2)) return 0; u16 v = u16(p[0]) | u16(u16(p[1]) << 8); p += 2; return v; }
    u32 u32v() { if (!need(4)) return 0; u32 v; std::memcpy(&v, p, 4); p += 4; return v; }
    f32 f32v() { u32 x = u32v(); f32 f; std::memcpy(&f, &x, 4); return f; }
};

} // namespace

bool OcNavData::valid() const {
    if (cellSizeCm <= 0.0f) return false;
    if (agentRadiusCm <= 0.0f || agentHeightCm <= 0.0f) return false;
    if (maxSlopeDeg <= 0.0f || maxSlopeDeg >= 90.0f) return false;
    if (maxStepCm < 0.0f) return false;
    // An EMPTY grid is legal and means "this level has no baked navigation" -- but a grid that
    // claims dimensions must actually hold them, or every lookup past the end reads a hole.
    const u64 want = static_cast<u64>(widthCells) * heightCells;
    if (want != cells.size()) return false;
    for (const OcNavCell& c : cells) {
        // A walkable cell with no region, or a region on an unwalkable cell, means the flood fill
        // and the walkable mask disagree -- and the O(1) reachability precheck is built entirely on
        // them agreeing.
        const bool walkable = (c.flags & kOcNavWalkable) != 0;
        if (walkable != (c.regionId != 0)) return false;
    }
    return true;
}

bool writeOcNav(const OcNavData& in, std::vector<u8>& out, std::string* why) {
    if (!in.valid())
        return fail(why, ".ocnav: refusing to write an invalid grid -- the dimensions disagree with "
                         "the cell count, a parameter is out of range, or a cell's walkable flag "
                         "and its region id disagree");

    std::vector<u8> nhdr;
    {
        W w{nhdr};
        w.f32v(in.cellSizeCm);
        w.f32v(in.originXCm);
        w.f32v(in.originYCm);
        w.u32v(in.widthCells);
        w.u32v(in.heightCells);
        w.f32v(in.agentRadiusCm);
        w.f32v(in.agentHeightCm);
        w.f32v(in.maxSlopeDeg);
        w.f32v(in.maxStepCm);
    }

    std::vector<u8> ncel;
    ncel.reserve(in.cells.size() * kCellBytes);
    {
        W w{ncel};
        for (const OcNavCell& c : in.cells) {
            w.f32v(c.floorZCm);
            w.u16v(c.regionId);
            w.u8v(c.flags);
            w.u8v(c.reserved);
        }
    }

    Avr1File f;
    f.subtype = kSubtypeNav;
    f.contentVersion = 1;
    f.flags = kAvrFlagCooked;
    f.add(kChunkNHDR, std::move(nhdr), kAvrChunkRequired);
    f.add(kChunkNCEL, std::move(ncel), kAvrChunkRequired);
    return writeAvr1(f, out, why);
}

bool parseOcNav(const u8* bytes, usize size, OcNavData& out, std::string* why) {
    Avr1File f;
    if (!parseAvr1(bytes, size, f, why)) return false;
    if (f.subtype != kSubtypeNav) return fail(why, ".ocnav: container subtype is not NAV");

    const AvrChunk* nhdr = f.find(kChunkNHDR);
    const AvrChunk* ncel = f.find(kChunkNCEL);
    if (!nhdr) return fail(why, ".ocnav: no NHDR chunk");
    if (!ncel) return fail(why, ".ocnav: no NCEL chunk");
    // Present and ignored: this reader has no off-mesh links, and the container records the chunk's
    // length so stepping over it costs nothing. That is the whole point of an optional chunk.
    (void)f.find(kChunkNLNK);

    R h{nhdr->data.data(), nhdr->data.data() + nhdr->data.size()};
    out.cellSizeCm    = h.f32v();
    out.originXCm     = h.f32v();
    out.originYCm     = h.f32v();
    out.widthCells    = h.u32v();
    out.heightCells   = h.u32v();
    out.agentRadiusCm = h.f32v();
    out.agentHeightCm = h.f32v();
    out.maxSlopeDeg   = h.f32v();
    out.maxStepCm     = h.f32v();
    if (!h.ok) return fail(why, ".ocnav: truncated NHDR");

    // BOUNDED AGAINST THE CHUNK before reserving: a corrupt width x height could otherwise ask for
    // an enormous allocation on its way to failing. The multiply is done in u64 so it cannot wrap.
    const u64 want = static_cast<u64>(out.widthCells) * out.heightCells;
    if (want * kCellBytes > ncel->data.size())
        return fail(why, ".ocnav: NHDR claims " + std::to_string(want) +
                         " cells, more than NCEL can hold");

    out.cells.clear();
    out.cells.resize(static_cast<usize>(want));
    R r{ncel->data.data(), ncel->data.data() + ncel->data.size()};
    for (usize i = 0; i < out.cells.size(); ++i) {
        OcNavCell& c = out.cells[i];
        c.floorZCm = r.f32v();
        c.regionId = r.u16v();
        c.flags    = r.u8v();
        c.reserved = r.u8v();
    }
    if (!r.ok) return fail(why, ".ocnav: truncated NCEL");
    if (!out.valid())
        return fail(why, ".ocnav: the file parses but its grid does not hold together");
    return true;
}

bool loadOcNav(const std::string& path, OcNavData& out, std::string* why) {
    aver::traceFileOpen(path);
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return fail(why, ".ocnav: cannot open " + path);
    const std::streamoff n = f.tellg();
    if (n <= 0) return fail(why, ".ocnav: empty file " + path);
    std::vector<u8> bytes(static_cast<usize>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(bytes.data()), n);
    if (!f) return fail(why, ".ocnav: short read on " + path);
    return parseOcNav(bytes.data(), bytes.size(), out, why);
}

bool saveOcNav(const std::string& path, const OcNavData& in, std::string* why) {
    std::vector<u8> bytes;
    if (!writeOcNav(in, bytes, why)) return false;
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    // NOT atomic, unlike .ocsave, and the difference is deliberate: a nav grid is BUILD OUTPUT that
    // an author regenerates with one command, not the one artefact a player asked the game to keep.
    // The same distinction .ocworld already makes.
    if (!writeFileBytes(path, bytes.data(), bytes.size()))
        return fail(why, ".ocnav: cannot write " + path);
    return true;
}

} // namespace aver::fmt
