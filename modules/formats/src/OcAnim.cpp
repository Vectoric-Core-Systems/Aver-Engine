// Readers and writers for .ocskel (skeletons) and .ocanim (animations), both AVR1 containers.

#include "aver/formats/OcAnim.hpp"

#include "aver/formats/Avr1.hpp"
#include "aver/core/Hash.hpp"   // fnv1a64: how a component names a socket

#include <cmath>                // std::isfinite: guards NTFD and CTAN against a NaN/Inf payload
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
// Optional, exactly as NOTF and SOCK are.
constexpr u32 kChunkCRVE = avrFourCC("CRVE");
// OPTIONAL, and DENSE rather than sparse: when present it holds exactly notifies.size() floats, one
// per NOTF entry in the same order, entry i being how long notifies[i] stays open (0 = instant). A
// clip where every notify is instantaneous -- which is every clip written before notify states
// existed -- omits this chunk entirely rather than writing a column of zeros, which is what keeps
// such a clip's rewrite byte-identical. See OcAnimation::notifyDurations for the full reasoning.
constexpr u32 kChunkNTFD = avrFourCC("NTFD");
// OPTIONAL, and DENSE PER CURVE rather than sparse, for NTFD's own reason applied one level deeper:
// when present it holds, for EVERY curve in CRVE (same order, same key count), a KeyCount-long
// in-tangent array followed by a KeyCount-long out-tangent array. A clip where every curve's every
// tangent is zero -- which is every clip written before tangents existed, Linear and Step curves
// included -- omits this chunk entirely, which is what keeps such a clip's rewrite byte-identical.
// See OcCurve::inTangents/outTangents for the full reasoning, including the one place this differs
// from a per-curve chunk would: a curve with no tangents of its own still gets a dense zero-filled
// pair back from disk once another curve in the same clip needed the chunk written at all.
constexpr u32 kChunkCTAN = avrFourCC("CTAN");

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
    // ATOMIC, NOT TRUNCATE-THEN-WRITE. An ofstream opened with ios::trunc zeroes the file when the
    // STREAM IS CONSTRUCTED, before a byte of `bytes` is written, and this overwrites a real asset in
    // place -- so a crash, a kill or a full disk in that window destroyed the previously-good file and
    // not merely the unsaved edit. writeFileBytesAtomic writes a temporary beside the target and swaps
    // only a complete one into place; see its comment in FileSystem.hpp, which already names this
    // class of caller.
    if (!writeFileBytesAtomic(path, bytes.data(), bytes.size()))
        return fail(why, std::string(what) + ": write failed on " + path);
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

const OcCurve* OcAnimation::curve(const std::string& name) const {
    for (const OcCurve& c : curves) if (c.name == name) return &c;
    return nullptr;
}

f32 OcAnimation::notifyDuration(u32 index) const {
    if (index >= notifyDurations.size()) return 0.0f;   // absent chunk, or a stale/out-of-range index
    return notifyDurations[index];
}

const OcCurve* OcAnimation::curveById(u64 id) const {
    if (id == 0) return nullptr;
    for (const OcCurve& c : curves) if (fnv1a64(c.name) == id) return &c;
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

    // A CURVE WITH MISMATCHED TIMES AND VALUES IS REFUSED AT WRITE, not silently truncated. The
    // sampler indexes values by the key it found in times, so a short values array would read past
    // the end -- and the guard belongs here, where the caller still knows what it built, rather than
    // in the sampler where the only available response is to invent a zero.
    for (const OcCurve& c : in.curves) {
        if (c.times.size() != c.values.size())
            return fail(why, ".ocanim: curve '" + c.name + "' has " + std::to_string(c.times.size()) +
                             " time(s) and " + std::to_string(c.values.size()) + " value(s)");
    }

    // TANGENTS -- same three refusals NTFD applies to notifyDurations, at curve granularity: only one
    // side set, a side whose length does not match the curve's own keys, or a non-finite value. All
    // caught here, where the caller still knows what it built, rather than reshaped into something
    // that silently samples wrong.
    for (const OcCurve& c : in.curves) {
        if (c.inTangents.empty() != c.outTangents.empty())
            return fail(why, ".ocanim: curve '" + c.name + "' has tangents on only one side");
        if (!c.inTangents.empty() &&
            (c.inTangents.size() != c.times.size() || c.outTangents.size() != c.times.size()))
            return fail(why, ".ocanim: curve '" + c.name + "' has " + std::to_string(c.inTangents.size()) +
                             " in-tangent(s) and " + std::to_string(c.outTangents.size()) +
                             " out-tangent(s) for " + std::to_string(c.times.size()) + " key(s)");
        for (const f32 v : c.inTangents)
            if (!std::isfinite(v)) return fail(why, ".ocanim: curve '" + c.name + "' has a non-finite in-tangent");
        for (const f32 v : c.outTangents)
            if (!std::isfinite(v)) return fail(why, ".ocanim: curve '" + c.name + "' has a non-finite out-tangent");
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

    // Notify DURATIONS -- see notifyDurations' own comment for why this is a separate chunk rather
    // than a wider NOTF. Refused rather than silently reshaped when the caller built a mismatched
    // pair: truncating or padding here would save a bad edit instead of surfacing it.
    if (!in.notifyDurations.empty() && in.notifyDurations.size() != in.notifies.size())
        return fail(why, ".ocanim: " + std::to_string(in.notifyDurations.size()) + " notify duration(s) for " +
                         std::to_string(in.notifies.size()) + " notify(ies)");
    for (const f32 d : in.notifyDurations)
        if (!(d >= 0.0f)) return fail(why, ".ocanim: a notify duration is negative or NaN");

    // OMITTED WHENEVER EVERY DURATION IS ZERO, checked by VALUE rather than by "is the vector
    // empty" -- an editor that always sizes this array to match notifies (rather than leaving it
    // empty until the first state is authored) must still get a byte-identical file for a clip
    // where nobody ever set one above zero. That is what keeps an old clip's round trip intact
    // regardless of which shape the caller happens to build.
    std::vector<u8> ntfd;
    bool anyDuration = false;
    for (const f32 d : in.notifyDurations) if (d > 0.0f) { anyDuration = true; break; }
    if (anyDuration) {
        W w{ntfd};
        w.u32v(static_cast<u32>(in.notifyDurations.size()));
        for (const f32 d : in.notifyDurations) w.f32v(d);
    }

    // Curves. Same placement reasoning as the notifies above: built before AHDR only because the
    // string table both intern into is written last.
    std::vector<u8> crve;
    if (!in.curves.empty()) {
        W w{crve};
        w.u32v(static_cast<u32>(in.curves.size()));
        for (const OcCurve& c : in.curves) {
            w.u32v(strt.add(c.name));
            w.u32v(static_cast<u32>(c.interp));
            w.u32v(static_cast<u32>(c.times.size()));
            for (const f32 t : c.times) w.f32v(t);
            for (const f32 v : c.values) w.f32v(v);
        }
    }

    // Curve TANGENTS -- see OcCurve::inTangents and kChunkCTAN's own comment for why this is a
    // separate chunk, parallel-indexed to CRVE, rather than a widening of it. OMITTED WHENEVER EVERY
    // TANGENT IN THE WHOLE CLIP IS ZERO, checked BY VALUE rather than by a vector being empty -- the
    // same rule NTFD applies to notifyDurations, so an editor that always sizes a curve's tangent
    // arrays (rather than leaving them empty until a handle is first dragged) still gets a byte-
    // identical file for a clip nobody has touched.
    std::vector<u8> ctan;
    bool anyTangent = false;
    for (const OcCurve& c : in.curves) {
        for (const f32 v : c.inTangents)  if (v != 0.0f) { anyTangent = true; break; }
        if (anyTangent) break;
        for (const f32 v : c.outTangents) if (v != 0.0f) { anyTangent = true; break; }
        if (anyTangent) break;
    }
    if (anyTangent) {
        W w{ctan};
        w.u32v(static_cast<u32>(in.curves.size()));
        for (const OcCurve& c : in.curves) {
            const u32 n = static_cast<u32>(c.times.size());
            w.u32v(n);
            // DENSE even for a curve whose own tangents were never authored (inTangents empty): the
            // chunk's granularity is the whole clip, so every curve gets an entry once ANY curve
            // needs one, and an absent side reads as all zero here exactly as notifyDuration() reads
            // an absent NTFD entry as zero.
            for (u32 k = 0; k < n; ++k) w.f32v(k < c.inTangents.size()  ? c.inTangents[k]  : 0.0f);
            for (u32 k = 0; k < n; ++k) w.f32v(k < c.outTangents.size() ? c.outTangents[k] : 0.0f);
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
    if (!ntfd.empty()) f.add(kChunkNTFD, std::move(ntfd));
    if (!crve.empty()) f.add(kChunkCRVE, std::move(crve));
    if (!ctan.empty()) f.add(kChunkCTAN, std::move(ctan));
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
    // CURVES. Absent is ordinary.
    out.curves.clear();
    if (const AvrChunk* crve = f.find(kChunkCRVE)) {
        R c{crve->data.data(), crve->data.data() + crve->data.size()};
        const u32 count = c.u32v();
        // A curve is at least 12 bytes (name, interp, key count) even with no keys.
        if (count > crve->data.size() / 12) return fail(why, ".ocanim: CRVE says it holds more curves than it can");
        out.curves.reserve(count);
        for (u32 i = 0; i < count; ++i) {
            OcCurve cur;
            cur.name = std::string(strt.get(c.u32v()));
            const u32 interp = c.u32v();
            // An UNKNOWN interpolation mode reads as Linear rather than refusing the file. A curve
            // is additive data: a reader that predates a mode should still play the clip, and the
            // worst case is a value that eases where it should hold. Refusing would make a clip
            // unopenable over a field nothing else in the file depends on.
            cur.interp = interp <= static_cast<u32>(OcInterp::CubicSpline)
                       ? static_cast<OcInterp>(interp) : OcInterp::Linear;
            const u32 keys = c.u32v();
            if (!c.ok) return fail(why, ".ocanim: truncated CRVE header");
            // Bounded before reserving: each key is 8 bytes (a time and a value).
            if (keys > crve->data.size() / 8) return fail(why, ".ocanim: a curve says it holds more keys than the chunk can");
            cur.times.resize(keys);
            cur.values.resize(keys);
            for (u32 k = 0; k < keys; ++k) cur.times[k] = c.f32v();
            for (u32 k = 0; k < keys; ++k) cur.values[k] = c.f32v();
            out.curves.push_back(std::move(cur));
        }
        if (!c.ok) return fail(why, ".ocanim: truncated curve table");
    }

    // CURVE TANGENTS. Absent is ordinary -- every clip predating tangents, and every clip whose
    // curves' tangents are all still zero, has no CTAN chunk. Read AFTER curves, exactly as NTFD is
    // read after NOTF, so the per-curve size checks below have something to check against; each
    // curve's own key count (already bounds-checked above) is what bounds the tangent counts read
    // here, so there is no separate huge-allocation guard to write.
    if (const AvrChunk* ctan = f.find(kChunkCTAN)) {
        R c{ctan->data.data(), ctan->data.data() + ctan->data.size()};
        const u32 curveCount = c.u32v();
        if (!c.ok) return fail(why, ".ocanim: truncated CTAN header");
        // DENSE AND PARALLEL, to CRVE this time rather than to NOTF: a curve count that does not
        // match cannot be lined up against it.
        if (curveCount != out.curves.size())
            return fail(why, ".ocanim: CTAN holds tangents for " + std::to_string(curveCount) +
                             " curve(s) but CRVE has " + std::to_string(out.curves.size()) + " curve(s)");
        for (u32 i = 0; i < curveCount; ++i) {
            OcCurve& cur = out.curves[i];
            const u32 keys = c.u32v();
            if (!c.ok) return fail(why, ".ocanim: truncated CTAN curve header");
            if (keys != cur.times.size())
                return fail(why, ".ocanim: CTAN curve '" + cur.name + "' has " + std::to_string(keys) +
                                 " tangent key(s) for " + std::to_string(cur.times.size()) + " CRVE key(s)");
            cur.inTangents.resize(keys);
            cur.outTangents.resize(keys);
            for (u32 k = 0; k < keys; ++k) cur.inTangents[k] = c.f32v();
            for (u32 k = 0; k < keys; ++k) cur.outTangents[k] = c.f32v();
        }
        if (!c.ok) return fail(why, ".ocanim: truncated CTAN chunk");
        for (const OcCurve& cur : out.curves) {
            for (const f32 v : cur.inTangents)
                if (!std::isfinite(v)) return fail(why, ".ocanim: curve '" + cur.name + "' has a non-finite in-tangent");
            for (const f32 v : cur.outTangents)
                if (!std::isfinite(v)) return fail(why, ".ocanim: curve '" + cur.name + "' has a non-finite out-tangent");
        }
    }

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

    // NOTIFY DURATIONS. Absent is ordinary -- every clip predating notify states, and every clip
    // whose notifies are all still instantaneous, has no NTFD chunk. Read AFTER notifies so the
    // size check below has something to check against.
    out.notifyDurations.clear();
    if (const AvrChunk* ntfd = f.find(kChunkNTFD)) {
        R d{ntfd->data.data(), ntfd->data.data() + ntfd->data.size()};
        const u32 count = d.u32v();
        // Bounded by what the chunk could possibly hold (4 bytes each) before reserving, the same
        // guard NOTF and CRVE use against a corrupt count driving an enormous allocation.
        if (count > ntfd->data.size() / 4) return fail(why, ".ocanim: NTFD says it holds more durations than it can");
        out.notifyDurations.resize(count);
        for (u32 i = 0; i < count; ++i) out.notifyDurations[i] = d.f32v();
        if (!d.ok) return fail(why, ".ocanim: truncated NTFD chunk");
        // DENSE AND PARALLEL is the whole contract: a count that does not match NOTF's cannot be
        // lined up against it, so this is a malformed file rather than something to pad or trim.
        if (out.notifyDurations.size() != out.notifies.size())
            return fail(why, ".ocanim: NTFD holds " + std::to_string(out.notifyDurations.size()) +
                             " duration(s) for " + std::to_string(out.notifies.size()) + " notify(ies)");
        for (const f32 dur : out.notifyDurations)
            if (!(dur >= 0.0f)) return fail(why, ".ocanim: a notify duration is negative or NaN");
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
