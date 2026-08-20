// Readers and writers for .ocskel (skeletons) and .ocanim (animations), both AVR1 containers.

#include "aver/formats/OcAnim.hpp"

#include "aver/formats/Avr1.hpp"
#include "aver/core/Hash.hpp"   // fnv1a64: how a component names a socket

#include <cstring>
#include "aver/platform/FileSystem.hpp"

#include <fstream>

namespace aver::fmt {
namespace {

constexpr u32 kChunkSKEL = avrFourCC("SKEL");
// OPTIONAL, exactly as NOTF is on a clip and for the same reason: a rig with no sockets emits no
// chunk, so a file written now is byte-identical to one written before sockets existed and an
// older reader never asks for what it does not know about.
constexpr u32 kChunkSOCK = avrFourCC("SOCK");
constexpr u32 kChunkSTRT = avrFourCC("STRT");
constexpr u32 kChunkAHDR = avrFourCC("AHDR");
constexpr u32 kChunkTRKS = avrFourCC("TRKS");
// OPTIONAL, and that is the whole backward-compatibility story. A reader asks for a chunk by id
// and gets null when it is absent, so an engine that predates notifies opens a notify-bearing
// clip and plays it exactly as it always did -- it simply never asks for NOTF. Marking it
// Required instead would have made those files unopenable, which is the difference between an
// additive format change and a breaking one.
constexpr u32 kChunkNOTF = avrFourCC("NOTF");

// Sets `why` and returns false.
bool fail(std::string* why, std::string m) { if (why) *why = std::move(m); return false; }

// Little-endian byte writer over a growing buffer.
struct W {
    std::vector<u8>& b;
    void u8v (u8 v)  { b.push_back(v); }
    void u16v(u16 v) { b.push_back(u8(v)); b.push_back(u8(v >> 8)); }
    void u32v(u32 v) { for (int i = 0; i < 4; ++i) b.push_back(u8(v >> (i * 8))); }
    void i32v(i32 v) { u32v(static_cast<u32>(v)); }
    void u64v(u64 v) { for (int i = 0; i < 8; ++i) b.push_back(u8(v >> (i * 8))); }
    void f32v(f32 v) { u32 x; std::memcpy(&x, &v, 4); u32v(x); }
};

// Bounds-checked little-endian byte reader. `ok` goes false on the first short read.
struct R {
    const u8* p; const u8* e; bool ok = true;
    bool need(usize n) { if (usize(e - p) < n) { ok = false; return false; } return true; }
    u8  u8v () { if (!need(1)) return 0; return *p++; }
    u16 u16v() { if (!need(2)) return 0; u16 v = u16(p[0]) | u16(u16(p[1]) << 8); p += 2; return v; }
    u32 u32v() { if (!need(4)) return 0; u32 v; std::memcpy(&v, p, 4); p += 4; return v; }
    i32 i32v() { return static_cast<i32>(u32v()); }
    u64 u64v() { if (!need(8)) return 0; u64 v; std::memcpy(&v, p, 8); p += 8; return v; }
    f32 f32v() { u32 x = u32v(); f32 f; std::memcpy(&f, &x, 4); return f; }
};

// Reads a whole file into `out`. `what` prefixes any error message.
bool readWholeFile(const std::string& path, std::vector<u8>& out, std::string* why, const char* what) {
    // Traced like the text loaders are. These open their own ifstream rather than going
    // through platform::readFileBytes, so without this line a .ocmesh/.ocanim/AVR1 read from
    // OUTSIDE a package would not appear in --trace-opens and verify-game.ps1 would pass a
    // package that reaches into the dev tree for its geometry.
    aver::traceFileOpen(path);
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return fail(why, std::string(what) + ": cannot open " + path);
    const std::streamoff n = f.tellg();
    if (n <= 0) return fail(why, std::string(what) + ": empty file " + path);
    out.resize(static_cast<usize>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out.data()), n);
    if (!f) return fail(why, std::string(what) + ": short read on " + path);
    return true;
}

// Writes `bytes` to a file, truncating it. `what` prefixes any error message.
bool writeWholeFile(const std::string& path, const std::vector<u8>& bytes, std::string* why, const char* what) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return fail(why, std::string(what) + ": cannot write " + path);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!f) return fail(why, std::string(what) + ": write failed on " + path);
    return true;
}

} // namespace

// ---------------------------------------------------------------- skeleton

// True when every bone parent is in range and stored before its child.
bool OcSkeleton::valid() const {
    if (bones.empty()) return false;
    for (usize i = 0; i < bones.size(); ++i) {
        const i32 p = bones[i].parent;
        if (p == kOcBoneNoParent) continue;
        if (p < 0 || usize(p) >= bones.size()) return false;
        if (usize(p) >= i) return false;
    }
    // A SOCKET POINTING AT A BONE THAT DOES NOT EXIST is the failure this catches, and it is the one
    // that actually happens: a rig is re-exported with fewer bones and every socket past the new end
    // becomes an out-of-range index. Refusing it here means the bad file is rejected at load with a
    // message, instead of indexing off the end of the bone array the first time something attaches.
    for (const OcSocket& k : sockets) if (usize(k.bone) >= bones.size()) return false;
    return true;
}

// The socket of that name, or nullptr. FIRST MATCH WINS on a duplicate: the format does not enforce
// unique names (see OcSkeleton::sockets), so this has to answer something, and answering the first
// is the only choice that is stable under an edit to a later entry.
const OcSocket* OcSkeleton::socket(const std::string& name) const {
    for (const OcSocket& k : sockets) if (k.name == name) return &k;
    return nullptr;
}

const OcSocket* OcSkeleton::socketById(u64 id) const {
    if (id == 0) return nullptr;   // 0 is "unset", not a name that happens to hash to zero
    for (const OcSocket& k : sockets) if (fnv1a64(k.name) == id) return &k;
    return nullptr;
}

// Encodes a skeleton into an .ocskel container. Returns false with `why` set on invalid input.
bool writeOcSkel(const OcSkeleton& in, std::vector<u8>& out, std::string* why) {
    if (in.bones.empty()) return fail(why, ".ocskel: no bones");
    if (in.bones.size() > 0xFFFF) return fail(why, ".ocskel: more than 65535 bones");
    if (!in.valid()) return fail(why, ".ocskel: bone parents are out of range, cyclic, or not in parent-before-child order");

    AvrStringTable strt;
    std::vector<u8> skel;
    {
        W w{skel};
        w.u32v(static_cast<u32>(in.bones.size()));
        w.u32v(in.rootBone);
        for (const OcBone& b : in.bones) {
            w.u32v(strt.add(b.name));
            w.i32v(b.parent);
            w.f32v(b.translation.x); w.f32v(b.translation.y); w.f32v(b.translation.z);
            w.f32v(b.rotation.x); w.f32v(b.rotation.y); w.f32v(b.rotation.z); w.f32v(b.rotation.w);
            w.f32v(b.scale.x); w.f32v(b.scale.y); w.f32v(b.scale.z);
            for (int k = 0; k < 16; ++k) w.f32v(b.inverseBind[k]);
        }
    }

    // Sockets, interned into the same string table the bone names use. Built before the table is
    // written, or their names would not be in it.
    std::vector<u8> sock;
    if (!in.sockets.empty()) {
        W w{sock};
        w.u32v(static_cast<u32>(in.sockets.size()));
        for (const OcSocket& s : in.sockets) {
            w.u32v(strt.add(s.name));
            w.u32v(s.bone);
            w.f32v(s.translation.x); w.f32v(s.translation.y); w.f32v(s.translation.z);
            w.f32v(s.rotation.x); w.f32v(s.rotation.y); w.f32v(s.rotation.z); w.f32v(s.rotation.w);
            w.f32v(s.scale.x); w.f32v(s.scale.y); w.f32v(s.scale.z);
        }
    }

    Avr1File f;
    f.subtype = kAvrSubtypeSkel;
    f.contentVersion = 1;
    f.flags = kAvrFlagCooked;
    f.add(kChunkSKEL, std::move(skel), kAvrChunkRequired);
    if (!sock.empty()) f.add(kChunkSOCK, std::move(sock));
    f.add(kChunkSTRT, strt.bytes());
    return writeAvr1(f, out, why);
}

// Decodes an .ocskel container into `out`. Returns false with `why` set on a malformed file.
bool parseOcSkel(const u8* bytes, usize size, OcSkeleton& out, std::string* why) {
    Avr1File f;
    if (!parseAvr1(bytes, size, f, why)) return false;
    if (f.subtype != kAvrSubtypeSkel) return fail(why, ".ocskel: container subtype is not SKEL");

    const AvrChunk* skel = f.find(kChunkSKEL);
    if (!skel) return fail(why, ".ocskel: no SKEL chunk");
    AvrStringTable strt;
    if (const AvrChunk* s = f.find(kChunkSTRT)) strt.setBytes(s->data);

    R r{skel->data.data(), skel->data.data() + skel->data.size()};
    const u32 count = r.u32v();
    out.rootBone = r.u32v();
    if (!r.ok) return fail(why, ".ocskel: truncated SKEL header");
    if (count == 0) return fail(why, ".ocskel: BoneCount is 0");

    out.bones.clear();
    out.bones.resize(count);
    for (u32 i = 0; i < count; ++i) {
        OcBone& b = out.bones[i];
        b.name = std::string(strt.get(r.u32v()));
        b.parent = r.i32v();
        b.translation = Vec3{r.f32v(), r.f32v(), r.f32v()};
        b.rotation = Quat{r.f32v(), r.f32v(), r.f32v(), r.f32v()};
        b.scale = Vec3{r.f32v(), r.f32v(), r.f32v()};
        for (int k = 0; k < 16; ++k) b.inverseBind[k] = r.f32v();
    }
    if (!r.ok) return fail(why, ".ocskel: truncated bone table");

    // Sockets. Absent is the ordinary case -- every rig written before they existed has no SOCK.
    out.sockets.clear();
    if (const AvrChunk* sock = f.find(kChunkSOCK)) {
        R s{sock->data.data(), sock->data.data() + sock->data.size()};
        const u32 n = s.u32v();
        // Bounded by what the chunk COULD hold (44 bytes each) before reserving, so a corrupt count
        // cannot ask for an enormous allocation on the way to failing.
        if (n > sock->data.size() / 44) return fail(why, ".ocskel: SOCK says it holds more sockets than it can");
        out.sockets.resize(n);
        for (u32 i = 0; i < n; ++i) {
            OcSocket& k = out.sockets[i];
            k.name = std::string(strt.get(s.u32v()));
            k.bone = s.u32v();
            k.translation = Vec3{s.f32v(), s.f32v(), s.f32v()};
            k.rotation = Quat{s.f32v(), s.f32v(), s.f32v(), s.f32v()};
            k.scale = Vec3{s.f32v(), s.f32v(), s.f32v()};
        }
        if (!s.ok) return fail(why, ".ocskel: truncated socket table");
    }

    if (!out.valid()) return fail(why, ".ocskel: bone parents are out of range, cyclic, or not in "
                                      "parent-before-child order, or a socket names a bone that does "
                                      "not exist");
    return true;
}

// Reads an .ocskel file from disk. Returns false with `why` set.
bool loadOcSkel(const std::string& path, OcSkeleton& out, std::string* why) {
    std::vector<u8> bytes;
    if (!readWholeFile(path, bytes, why, ".ocskel")) return false;
    return parseOcSkel(bytes.data(), bytes.size(), out, why);
}

// Writes a skeleton to an .ocskel file. Returns false with `why` set.
bool saveOcSkel(const std::string& path, const OcSkeleton& in, std::string* why) {
    std::vector<u8> bytes;
    if (!writeOcSkel(in, bytes, why)) return false;
    return writeWholeFile(path, bytes, why, ".ocskel");
}

// ---------------------------------------------------------------- animation

// Floats stored per key. CubicSpline triples it: in-tangent, value, out-tangent per component.
u32 OcTrack::componentsPerKey() const {
    u32 n = 0;
    if (channels & kOcChannelTranslation) n += 3;
    if (channels & kOcChannelRotation)    n += 4;
    if (channels & kOcChannelScale)       n += 3;
    return interp == OcInterp::CubicSpline ? n * 3 : n;
}

// True when the track has a channel mask, keys, a matching value count, and ascending times.
bool OcTrack::valid() const {
    const u32 stride = componentsPerKey();
    if (stride == 0) return false;                                   // an empty channel mask animates nothing
    if (times.empty()) return false;
    if (values.size() != times.size() * stride) return false;
    for (usize i = 1; i < times.size(); ++i) if (times[i] < times[i - 1]) return false;  // must ascend
    return true;
}

// True when the animation has tracks, a non-negative duration, and every track is valid.
bool OcAnimation::valid() const {
    if (tracks.empty()) return false;
    if (!(duration >= 0.0f)) return false;                           // also rejects NaN
    for (const OcTrack& t : tracks) if (!t.valid()) return false;
    if (storage == OcAnimStorage::BakedUniform && sampleRate == 0) return false;
    return true;
}

// Encodes an animation into an .ocanim container. Returns false with `why` set on invalid input.
bool writeOcAnim(const OcAnimation& in, std::vector<u8>& out, std::string* why) {
    if (in.tracks.empty()) return fail(why, ".ocanim: no tracks");
    if (in.tracks.size() > 0xFFFFFFFFull) return fail(why, ".ocanim: too many tracks");
    for (usize i = 0; i < in.tracks.size(); ++i) {
        const OcTrack& t = in.tracks[i];
        if (t.componentsPerKey() == 0)
            return fail(why, ".ocanim: track " + std::to_string(i) + " has an empty channel mask");
        if (t.values.size() != t.times.size() * t.componentsPerKey())
            return fail(why, ".ocanim: track " + std::to_string(i) + " has " + std::to_string(t.values.size()) +
                             " values for " + std::to_string(t.times.size()) + " keys");
        for (usize k = 1; k < t.times.size(); ++k)
            if (t.times[k] < t.times[k - 1])
                return fail(why, ".ocanim: track " + std::to_string(i) + " key times are not ascending");
    }
    if (in.storage == OcAnimStorage::BakedUniform && in.sampleRate == 0)
        return fail(why, ".ocanim: baked-uniform storage needs a non-zero sample rate");

    AvrStringTable strt;
    const u32 skelRef = strt.add(in.skeletonRef);

    // TRKS first: the header records offsets into it.
    std::vector<u8> trks;
    std::vector<u64> keyOffsets(in.tracks.size());
    {
        W w{trks};
        for (usize i = 0; i < in.tracks.size(); ++i) {
            const OcTrack& t = in.tracks[i];
            keyOffsets[i] = trks.size();
            // All times, then all values, per track. Never interleaved.
            for (const f32 v : t.times)  w.f32v(v);
            for (const f32 v : t.values) w.f32v(v);
        }
    }

    // Notifies, before AHDR only because both intern into the same string table and the table is
    // written last. A clip with none adds no chunk at all rather than an empty one.
    std::vector<u8> notf;
    if (!in.notifies.empty()) {
        W w{notf};
        w.u32v(static_cast<u32>(in.notifies.size()));
        for (const OcNotify& n : in.notifies) {
            w.f32v(n.time);
            w.u32v(strt.add(n.name));
        }
    }

    std::vector<u8> ahdr;
    {
        W w{ahdr};
        w.f32v(in.duration);
        w.u32v(static_cast<u32>(in.tracks.size()));
        w.u8v(static_cast<u8>(in.storage));
        w.u8v(in.flags);
        w.u16v(in.sampleRate);
        w.u32v(skelRef);
        for (usize i = 0; i < in.tracks.size(); ++i) {
            const OcTrack& t = in.tracks[i];
            w.u16v(t.boneIndex);
            w.u8v(t.channels);
            w.u8v(static_cast<u8>(t.interp));
            w.u64v(keyOffsets[i]);
            w.u32v(static_cast<u32>(t.times.size()));
            w.u32v(0);                                   // Reserved
        }
    }

    Avr1File f;
    f.subtype = kAvrSubtypeAnim;
    f.contentVersion = 1;
    f.flags = kAvrFlagCooked;
    f.add(kChunkAHDR, std::move(ahdr), kAvrChunkRequired);
    f.add(kChunkTRKS, std::move(trks));
    if (!notf.empty()) f.add(kChunkNOTF, std::move(notf));
    // AFTER the notify names have been interned, or the table would be written without them.
    f.add(kChunkSTRT, strt.bytes());
    return writeAvr1(f, out, why);
}

// Decodes an .ocanim container into `out`. Returns false with `why` set on a malformed file.
bool parseOcAnim(const u8* bytes, usize size, OcAnimation& out, std::string* why) {
    Avr1File f;
    if (!parseAvr1(bytes, size, f, why)) return false;
    if (f.subtype != kAvrSubtypeAnim) return fail(why, ".ocanim: container subtype is not ANIM");

    const AvrChunk* ahdr = f.find(kChunkAHDR);
    const AvrChunk* trks = f.find(kChunkTRKS);
    if (!ahdr) return fail(why, ".ocanim: no AHDR chunk");
    if (!trks) return fail(why, ".ocanim: no TRKS chunk");
    AvrStringTable strt;
    if (const AvrChunk* s = f.find(kChunkSTRT)) strt.setBytes(s->data);

    R r{ahdr->data.data(), ahdr->data.data() + ahdr->data.size()};
    out.duration   = r.f32v();
    const u32 n    = r.u32v();
    out.storage    = static_cast<OcAnimStorage>(r.u8v());
    out.flags      = r.u8v();
    out.sampleRate = r.u16v();
    out.skeletonRef= std::string(strt.get(r.u32v()));

    // NOTIFIES. Absent is the ordinary case and not an error -- every clip written before they
    // existed has no NOTF chunk, and find() answering null is exactly how this format says "this
    // file predates that idea" rather than "this file is broken".
    if (const AvrChunk* notf = f.find(kChunkNOTF)) {
        R n{notf->data.data(), notf->data.data() + notf->data.size()};
        const u32 count = n.u32v();
        // Bounded by what the chunk could POSSIBLY hold (4 bytes of time + 4 of name offset each)
        // before reserving, so a corrupt count cannot ask for an enormous allocation on the way to
        // failing. The reader below still checks each read; this only stops the allocation.
        const usize maxPossible = notf->data.size() / 8;
        if (count > maxPossible) return fail(why, ".ocanim: NOTF says it holds more notifies than it can");
        out.notifies.reserve(count);
        for (u32 i = 0; i < count; ++i) {
            OcNotify entry;
            entry.time = n.f32v();
            entry.name = std::string(strt.get(n.u32v()));
            out.notifies.push_back(std::move(entry));
        }
    }
    if (!r.ok) return fail(why, ".ocanim: truncated AHDR");
    if (n == 0) return fail(why, ".ocanim: TrackCount is 0");
    if (out.storage != OcAnimStorage::Keyframed && out.storage != OcAnimStorage::BakedUniform)
        return fail(why, ".ocanim: unknown Storage value");

    out.tracks.clear();
    out.tracks.resize(n);
    std::vector<u64> offsets(n);
    std::vector<u32> counts(n);
    for (u32 i = 0; i < n; ++i) {
        OcTrack& t = out.tracks[i];
        t.boneIndex = r.u16v();
        t.channels  = r.u8v();
        const u8 interp = r.u8v();
        if (interp > static_cast<u8>(OcInterp::CubicSpline))
            return fail(why, ".ocanim: unknown interpolation mode on track " + std::to_string(i));
        t.interp = static_cast<OcInterp>(interp);
        offsets[i] = r.u64v();
        counts[i]  = r.u32v();
        r.u32v();                                        // Reserved
    }
    if (!r.ok) return fail(why, ".ocanim: truncated track table");

    for (u32 i = 0; i < n; ++i) {
        OcTrack& t = out.tracks[i];
        const u32 stride = t.componentsPerKey();
        if (stride == 0) return fail(why, ".ocanim: track " + std::to_string(i) + " has an empty channel mask");
        const u64 need = (u64(counts[i]) + u64(counts[i]) * stride) * 4ull;
        // SUBTRACTION, NOT ADDITION. `offsets[i]` is a u64 read straight from the file, so
        // `offsets[i] + need > size` wraps: a huge offset with a small need sums back into range,
        // passes, and `trks->data.data() + offsets[i]` becomes an arbitrary pointer that the two
        // memcpys below then read through.
        const u64 avail = u64(trks->data.size());
        if (offsets[i] > avail || need > avail - offsets[i])
            return fail(why, ".ocanim: track " + std::to_string(i) + " runs past the end of TRKS");

        const u8* p = trks->data.data() + offsets[i];
        t.times.resize(counts[i]);
        std::memcpy(t.times.data(), p, usize(counts[i]) * 4);
        t.values.resize(usize(counts[i]) * stride);
        std::memcpy(t.values.data(), p + usize(counts[i]) * 4, usize(counts[i]) * stride * 4);

        for (usize k = 1; k < t.times.size(); ++k)
            if (t.times[k] < t.times[k - 1])
                return fail(why, ".ocanim: track " + std::to_string(i) + " key times are not ascending");
    }
    return true;
}

// Reads an .ocanim file from disk. Returns false with `why` set.
bool loadOcAnim(const std::string& path, OcAnimation& out, std::string* why) {
    std::vector<u8> bytes;
    if (!readWholeFile(path, bytes, why, ".ocanim")) return false;
    return parseOcAnim(bytes.data(), bytes.size(), out, why);
}

// Writes an animation to an .ocanim file. Returns false with `why` set.
bool saveOcAnim(const std::string& path, const OcAnimation& in, std::string* why) {
    std::vector<u8> bytes;
    if (!writeOcAnim(in, bytes, why)) return false;
    return writeWholeFile(path, bytes, why, ".ocanim");
}

} // namespace aver::fmt
