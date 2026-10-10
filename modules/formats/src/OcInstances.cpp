// `.ocinst` reader and writer: the IHDR/ISTR/IGRP/IXFM chunks of a baked instance table in an AVR1
// container. See the header for the transform convention and the fast-path rationale.
#include "aver/formats/OcInstances.hpp"

#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace aver::fmt {
namespace {

// Byte cursor over a chunk payload, ported from OcLand.cpp's own Cursor rather than shared -- every
// .oc* binary reader in this tree carries its own tiny one (see OcFoliage.cpp's comment on this exact
// duplication).
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
};

// Appends the raw bytes of `v` to `b`.
template <typename T> void put(std::vector<u8>& b, T v) {
    const usize at = b.size();
    b.resize(at + sizeof(T));
    std::memcpy(b.data() + at, &v, sizeof(T));
}

bool fail(std::string* why, std::string msg) { if (why) *why = std::move(msg); return false; }

// One group record on disk: StringRef assetRef, u32 flags, u32 first, u32 count.
constexpr usize kGroupRecordBytes = sizeof(u32) * 4;
// IHDR layout: u32 groupCount, u32 instanceCount.
constexpr usize kHeaderBytes = sizeof(u32) * 2;
// ICEL cell record: i32 cx, cy; f32 min[3], max[3]; u32 firstRun, runCount. IRUN run: u32 x3.
constexpr usize kCellRecordBytes = sizeof(u32) * 10;
constexpr usize kRunRecordBytes = sizeof(u32) * 3;

} // namespace

bool OcInstanceData::valid() const {
    if (transforms.size() % 12 != 0) return false;
    const usize instanceCount = transforms.size() / 12;
    for (const OcInstanceGroup& g : groups) {
        if (g.asset.empty()) return false;
        // SUBTRACTION, NOT ADDITION -- `g.first + g.count` could wrap a u32 for a hand-built or
        // corrupt group; comparing against `instanceCount - g.first` cannot, since g.first is already
        // known <= instanceCount on the line before it runs. Avr1.cpp's own chunk-bounds checks use
        // the identical rearrangement, for the identical reason.
        if (g.first > instanceCount || instanceCount - g.first < g.count) return false;
    }
    for (const OcInstanceRun& r : runs) {
        if (r.group >= groups.size()) return false;
        if (r.first > instanceCount || instanceCount - r.first < r.count) return false;
    }
    for (const OcInstanceCell& c : cells) {
        if (c.firstRun > runs.size() || runs.size() - c.firstRun < c.runCount) return false;
    }
    if (!cells.empty() && !(cellCm > 0.0f && std::isfinite(cellCm))) return false;
    for (f32 v : transforms) if (!std::isfinite(v)) return false;
    return true;
}

bool parseOcInstances(const u8* bytes, usize size, OcInstanceData& out, std::string* why) {
    Avr1File f;
    if (!parseAvr1(bytes, size, f, why)) return false;
    if (f.subtype != kAvrSubtypeInst) return fail(why, ".ocinst: container subtype is not INST");

    const AvrChunk* hdr = f.find(kOcInstChunkHeader);
    if (!hdr || hdr->data.size() < kHeaderBytes) return fail(why, ".ocinst: no IHDR chunk (or too short)");
    Cursor hc{hdr->data.data(), hdr->data.size()};
    const u32 groupCount = hc.take<u32>();
    const u32 instanceCount = hc.take<u32>();
    if (!hc.ok) return fail(why, ".ocinst: IHDR ended early");

    AvrStringTable strt;
    if (const AvrChunk* s = f.find(kOcInstChunkStrings)) strt.setBytes(s->data);

    const AvrChunk* grp = f.find(kOcInstChunkGroups);
    if (!grp) return fail(why, ".ocinst: no IGRP chunk");
    const usize groupBytes = usize(groupCount) * kGroupRecordBytes;
    if (grp->data.size() < groupBytes)
        return fail(why, ".ocinst: IGRP is smaller than groupCount implies");

    const AvrChunk* xfm = f.find(kOcInstChunkTransforms);
    if (!xfm) return fail(why, ".ocinst: no IXFM chunk");
    const usize wantBytes = usize(instanceCount) * 12 * sizeof(f32);
    if (xfm->data.size() != wantBytes)
        return fail(why, ".ocinst: IXFM size does not match instanceCount * 12 floats -- the header "
                         "and the payload disagree");

    OcInstanceData tmp;
    tmp.groups.reserve(groupCount);
    for (u32 i = 0; i < groupCount; ++i) {
        Cursor gc{grp->data.data() + usize(i) * kGroupRecordBytes, kGroupRecordBytes};
        const u32 assetRef = gc.take<u32>();
        OcInstanceGroup g;
        g.flags = gc.take<u32>();
        g.first = gc.take<u32>();
        g.count = gc.take<u32>();
        if (!gc.ok) return fail(why, ".ocinst: IGRP record truncated");
        g.asset = std::string(strt.get(assetRef));
        if (g.asset.empty()) return fail(why, ".ocinst: a group names no asset");
        // SUBTRACTION, NOT ADDITION -- see OcInstanceData::valid()'s own comment on this exact check;
        // `first` and `count` are read straight off disk, so a crafted file can make either one huge.
        if (g.first > instanceCount || instanceCount - g.first < g.count)
            return fail(why, ".ocinst: a group's [first, first+count) range runs past instanceCount");
        tmp.groups.push_back(std::move(g));
    }

    if (const AvrChunk* cel = f.find(kOcInstChunkCells)) {
        Cursor cc{cel->data.data(), cel->data.size()};
        tmp.cellCm = cc.take<f32>();
        const u32 cellCount = cc.take<u32>();
        if (!cc.ok || cc.left / kCellRecordBytes < cellCount)
            return fail(why, ".ocinst: ICEL is smaller than cellCount implies");
        tmp.cells.resize(cellCount);
        for (OcInstanceCell& c : tmp.cells) {
            c.cx = cc.take<i32>(); c.cy = cc.take<i32>();
            for (f32& v : c.min) v = cc.take<f32>();
            for (f32& v : c.max) v = cc.take<f32>();
            c.firstRun = cc.take<u32>();
            c.runCount = cc.take<u32>();
        }
        const AvrChunk* run = f.find(kOcInstChunkRuns);
        const usize runBytes = run ? run->data.size() : 0;
        if (runBytes % kRunRecordBytes != 0) return fail(why, ".ocinst: IRUN size is not a whole number of runs");
        tmp.runs.resize(runBytes / kRunRecordBytes);
        if (run) {
            Cursor rc{run->data.data(), run->data.size()};
            for (OcInstanceRun& r : tmp.runs) {
                r.group = rc.take<u32>(); r.first = rc.take<u32>(); r.count = rc.take<u32>();
            }
        }
    }

    // THE FAST PATH: one bulk memcpy of the whole span, not a per-instance or per-float loop --
    // see the header's own top comment for why a multi-million-instance file needs this to stay a
    // single allocation and a single copy rather than 12 million individual f32 reads.
    tmp.transforms.resize(usize(instanceCount) * 12);
    if (!tmp.transforms.empty())
        std::memcpy(tmp.transforms.data(), xfm->data.data(), wantBytes);

    for (f32 v : tmp.transforms)
        if (!std::isfinite(v)) return fail(why, ".ocinst: a transform contains a non-finite float");

    for (const OcInstanceRun& r : tmp.runs) {
        if (r.group >= groupCount) return fail(why, ".ocinst: an IRUN run names a group that does not exist");
        if (r.first > instanceCount || instanceCount - r.first < r.count)
            return fail(why, ".ocinst: an IRUN run's range runs past instanceCount");
    }
    for (const OcInstanceCell& c : tmp.cells)
        if (c.firstRun > tmp.runs.size() || tmp.runs.size() - c.firstRun < c.runCount)
            return fail(why, ".ocinst: an ICEL cell's run range runs past the IRUN table");

    out = std::move(tmp);
    return true;
}

bool loadOcInstances(const std::string& path, OcInstanceData& out, std::string* why) {
    // ONE READ OF THE WHOLE FILE, straight into one buffer -- OcFoliage.cpp's loadOcFoliage shape,
    // NOT OcLand.cpp's loadOcLand (parse to an Avr1File, re-serialise it back to bytes with writeAvr1,
    // then parse THAT). That round trip is harmless for a landscape section's kilobytes, but it would
    // cost this format TWO EXTRA full copies of its one big chunk -- a 384 MB IXFM at
    // kMaxFoliageInstances copied into `probe`'s chunk, out to a fresh buffer, then back into
    // `out.transforms` -- exactly what this header's own "no per-instance allocation" fast-path
    // comment is asking a multi-million-instance file to avoid.
    aver::traceFileOpen(path);
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return fail(why, ".ocinst: cannot open " + path);
    const std::streamoff n = in.tellg();
    if (n <= 0) return fail(why, ".ocinst: empty file " + path);
    std::vector<u8> bytes(static_cast<usize>(n));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(bytes.data()), n);
    if (!in) return fail(why, ".ocinst: short read on " + path);
    return parseOcInstances(bytes.data(), bytes.size(), out, why);
}

bool writeOcInstances(const OcInstanceData& in, std::vector<u8>& out, std::string* why) {
    if (!in.valid())
        return fail(why, ".ocinst: refusing to write an invalid table -- every group must name an "
                         "asset, every group's [first, first+count) must lie inside the instance "
                         "count, and every transform float must be finite");

    const u32 instanceCount = static_cast<u32>(in.transforms.size() / 12);
    const u32 groupCount = static_cast<u32>(in.groups.size());

    AvrStringTable strt;
    std::vector<u8> grp;
    grp.reserve(usize(groupCount) * kGroupRecordBytes);
    for (const OcInstanceGroup& g : in.groups) {
        const u32 assetRef = strt.add(g.asset);
        put<u32>(grp, assetRef);
        put<u32>(grp, g.flags);
        put<u32>(grp, g.first);
        put<u32>(grp, g.count);
    }

    std::vector<u8> hdr;
    hdr.reserve(kHeaderBytes);
    put<u32>(hdr, groupCount);
    put<u32>(hdr, instanceCount);

    // THE FAST PATH'S WRITE SIDE: one bulk memcpy of the whole vector<f32>, not a per-instance W::f32v
    // loop -- the identical "no per-instance allocation" reason parseOcInstances reads it in one copy.
    std::vector<u8> xfm(in.transforms.size() * sizeof(f32));
    if (!xfm.empty()) std::memcpy(xfm.data(), in.transforms.data(), xfm.size());

    Avr1File file;
    file.subtype = kAvrSubtypeInst;
    file.add(kOcInstChunkHeader, std::move(hdr), kAvrChunkRequired);
    file.add(kOcInstChunkStrings, strt.bytes());
    file.add(kOcInstChunkGroups, std::move(grp), kAvrChunkRequired);
    file.add(kOcInstChunkTransforms, std::move(xfm), kAvrChunkGpuUploadable);
    if (!in.cells.empty()) {
        std::vector<u8> cel, run;
        put<f32>(cel, in.cellCm);
        put<u32>(cel, static_cast<u32>(in.cells.size()));
        for (const OcInstanceCell& c : in.cells) {
            put<i32>(cel, c.cx); put<i32>(cel, c.cy);
            for (f32 v : c.min) put<f32>(cel, v);
            for (f32 v : c.max) put<f32>(cel, v);
            put<u32>(cel, c.firstRun); put<u32>(cel, c.runCount);
        }
        for (const OcInstanceRun& r : in.runs) {
            put<u32>(run, r.group); put<u32>(run, r.first); put<u32>(run, r.count);
        }
        file.add(kOcInstChunkCells, std::move(cel));
        file.add(kOcInstChunkRuns, std::move(run));
    }
    return writeAvr1(file, out, why);
}

bool saveOcInstances(const std::string& path, const OcInstanceData& in, std::string* why) {
    std::vector<u8> bytes;
    if (!writeOcInstances(in, bytes, why)) return false;
    // WRITES THE SERIALISED BYTES DIRECTLY -- OcMesh.cpp's saveOcMesh shape, not OcLand.cpp's
    // saveOcLand (parse the bytes back into an Avr1File just to hand them to saveAvr1, which
    // re-serialises them a second time). A baked, tool-written table this size cannot afford a second
    // full copy-and-resegment of its one big chunk for no benefit.
    //
    // ATOMIC, NOT TRUNCATE-THEN-WRITE, for the identical reason saveOcMesh gives: an ofstream opened
    // with ios::trunc zeroes the file the instant it is constructed, before a byte of `bytes` has
    // landed, so a crash or a full disk mid-write would destroy a previously-good table rather than
    // merely fail to update it -- and a `.ocinst` is exactly the kind of large, re-importable build
    // product where that matters.
    if (!writeFileBytesAtomic(path, bytes.data(), bytes.size()))
        return fail(why, ".ocinst: write failed on " + path);
    return true;
}

bool cellOcInstances(OcInstanceData& in, f32 cellCm, const std::vector<f32>& groupRadiusCm, std::string* why) {
    if (!(cellCm > 0.0f)) { if (why) *why = "cell size must be positive"; return false; }
    const usize rows = in.transforms.size() / 12;
    std::vector<u32> rowGroup(rows, 0xFFFFFFFFu);
    if (!in.runs.empty()) {
        for (const OcInstanceRun& r : in.runs)
            for (u32 k = 0; k < r.count && usize(r.first) + k < rows; ++k) rowGroup[usize(r.first) + k] = r.group;
    } else {
        for (usize g = 0; g < in.groups.size(); ++g)
            for (u32 k = 0; k < in.groups[g].count && usize(in.groups[g].first) + k < rows; ++k)
                rowGroup[usize(in.groups[g].first) + k] = static_cast<u32>(g);
    }
    for (const u32 g : rowGroup)
        if (g == 0xFFFFFFFFu) { if (why) *why = "a row lies in no group"; return false; }

    struct Key { i32 cx, cy; u32 group, row; };
    std::vector<Key> keys(rows);
    for (usize r = 0; r < rows; ++r) {
        const f32* m = &in.transforms[r * 12];
        keys[r] = Key{static_cast<i32>(std::floor(m[9] / cellCm)), static_cast<i32>(std::floor(m[10] / cellCm)),
                      rowGroup[r], static_cast<u32>(r)};
    }
    std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
        if (a.cx != b.cx) return a.cx < b.cx;
        if (a.cy != b.cy) return a.cy < b.cy;
        if (a.group != b.group) return a.group < b.group;
        return a.row < b.row;
    });

    std::vector<f32> sorted(in.transforms.size());
    in.cells.clear();
    in.runs.clear();
    in.cellCm = cellCm;
    f32 lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    const auto closeCell = [&] {
        OcInstanceCell& c = in.cells.back();
        for (int a = 0; a < 3; ++a) { c.min[a] = lo[a]; c.max[a] = hi[a]; }
    };
    for (usize r = 0; r < rows; ++r) {
        const Key& k = keys[r];
        std::memcpy(&sorted[r * 12], &in.transforms[usize(k.row) * 12], 12 * sizeof(f32));
        const bool newCell = r == 0 || k.cx != keys[r - 1].cx || k.cy != keys[r - 1].cy;
        if (newCell) {
            if (r) closeCell();
            OcInstanceCell c{};
            c.cx = k.cx;
            c.cy = k.cy;
            c.firstRun = static_cast<u32>(in.runs.size());
            in.cells.push_back(c);
        }
        if (newCell || k.group != keys[r - 1].group) {
            in.runs.push_back(OcInstanceRun{k.group, static_cast<u32>(r), 0});
            ++in.cells.back().runCount;
        }
        ++in.runs.back().count;
        const f32* m = &sorted[r * 12];
        f32 s2 = 0.0f;
        for (int row = 0; row < 3; ++row)
            s2 = std::max(s2, m[row * 3] * m[row * 3] + m[row * 3 + 1] * m[row * 3 + 1] + m[row * 3 + 2] * m[row * 3 + 2]);
        const f32 rad = (k.group < groupRadiusCm.size() ? groupRadiusCm[k.group] : 0.0f) * std::sqrt(s2);
        for (int a = 0; a < 3; ++a) {
            const f32 vlo = m[9 + a] - rad, vhi = m[9 + a] + rad;
            if (newCell) { lo[a] = vlo; hi[a] = vhi; }
            else { lo[a] = std::min(lo[a], vlo); hi[a] = std::max(hi[a], vhi); }
        }
    }
    if (rows) closeCell();
    in.transforms = std::move(sorted);
    return true;
}

} // namespace aver::fmt
