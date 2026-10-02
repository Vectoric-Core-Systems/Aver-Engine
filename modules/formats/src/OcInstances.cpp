// `.ocinst` reader and writer: the IHDR/ISTR/IGRP/IXFM chunks of a baked instance table in an AVR1
// container. See the header for the transform convention and the fast-path rationale.
#include "aver/formats/OcInstances.hpp"

#include "aver/platform/FileSystem.hpp"

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

    // THE FAST PATH: one bulk memcpy of the whole span, not a per-instance or per-float loop --
    // see the header's own top comment for why a multi-million-instance file needs this to stay a
    // single allocation and a single copy rather than 12 million individual f32 reads.
    tmp.transforms.resize(usize(instanceCount) * 12);
    if (!tmp.transforms.empty())
        std::memcpy(tmp.transforms.data(), xfm->data.data(), wantBytes);

    for (f32 v : tmp.transforms)
        if (!std::isfinite(v)) return fail(why, ".ocinst: a transform contains a non-finite float");

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

} // namespace aver::fmt
