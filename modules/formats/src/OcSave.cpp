// Reader and writer for .ocsave -- a whole world captured generically. See the header for why this
// is a snapshot rather than a delta, and why an array index is the identity.

#include "aver/formats/OcSave.hpp"

#include "aver/formats/Avr1.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>

namespace aver::fmt {
namespace {

constexpr u32 kChunkSHDR = avrFourCC("SHDR");
constexpr u32 kChunkSENT = avrFourCC("SENT");
constexpr u32 kChunkSTRT = avrFourCC("STRT");
// Four characters exactly -- avrFourCC takes const char(&)[5], so "SAVE " would not compile and
// "SAVE" is what every other four-letter subtype in Avr1.hpp does.
constexpr u32 kSubtypeSave = avrFourCC("SAVE");

// MIRRORS aver::scene::FieldKind / AVER_SCENE_KIND_* (scene_abi.h:37-45) VALUE FOR VALUE, and is
// duplicated here rather than included because Aver.Formats sits BELOW Aver.Scene and must not gain
// an edge to it -- the same rule that keeps a loaded .ocskel out of Aver.Scene, running the other
// way. A kind crosses this boundary as a plain u32, exactly as an asset ObjectId crosses as a plain
// u64.
//
// The drift check lives where it can: Aver.Save links BOTH and static_asserts these against the
// real enum. A mirror with no assert anywhere is how the three PerFrame cbuffer copies in this
// engine's RHI became a standing hazard.
constexpr u32 kKindF32    = 0;
constexpr u32 kKindVec3   = 1;
constexpr u32 kKindQuat   = 2;
constexpr u32 kKindI32    = 3;
constexpr u32 kKindBool   = 4;
constexpr u32 kKindI64    = 5;
constexpr u32 kKindEntity = 6;
constexpr u32 kKindString = 7;
constexpr u32 kKindMat4   = 8;

bool fail(std::string* why, std::string m) { if (why) *why = std::move(m); return false; }

struct W {
    std::vector<u8>& b;
    void u32v(u32 v) { for (int i = 0; i < 4; ++i) b.push_back(u8(v >> (i * 8))); }
    void i32v(i32 v) { u32v(static_cast<u32>(v)); }
    void u64v(u64 v) { for (int i = 0; i < 8; ++i) b.push_back(u8(v >> (i * 8))); }
    void i64v(i64 v) { u64v(static_cast<u64>(v)); }
    void f32v(f32 v) { u32 x; std::memcpy(&x, &v, 4); u32v(x); }
};

struct R {
    const u8* p; const u8* e; bool ok = true;
    bool need(usize n) { if (usize(e - p) < n) { ok = false; return false; } return true; }
    u32 u32v() { if (!need(4)) return 0; u32 v; std::memcpy(&v, p, 4); p += 4; return v; }
    i32 i32v() { return static_cast<i32>(u32v()); }
    u64 u64v() { if (!need(8)) return 0; u64 v; std::memcpy(&v, p, 8); p += 8; return v; }
    i64 i64v() { return static_cast<i64>(u64v()); }
    f32 f32v() { u32 x = u32v(); f32 f; std::memcpy(&f, &x, 4); return f; }
};

} // namespace

u32 ocSaveFloatCount(u32 kind) {
    switch (kind) {
        case kKindF32:  return 1;
        case kKindVec3: return 3;
        case kKindQuat: return 4;
        case kKindMat4: return 16;
        default:        return 0;
    }
}

namespace {
// True when a kind carries its value in OcSaveField::i.
bool kindIsInt(u32 k) { return k == kKindI32 || k == kKindBool || k == kKindI64 || k == kKindEntity; }
} // namespace

bool OcSaveData::valid() const {
    const i64 n = static_cast<i64>(entities.size());
    for (i64 idx = 0; idx < n; ++idx) {
        const OcSaveEntity& en = entities[static_cast<usize>(idx)];
        // A PARENT MUST SIT AT A LOWER INDEX. This is not tidiness -- restore walks the array once,
        // forwards, and reads the parent's already-created handle out of the same array. A forward
        // reference would read a hole.
        if (en.parent >= idx) return false;
        if (en.parent < -1) return false;
        for (const OcSaveComponent& c : en.components) {
            if (c.type.empty()) return false;
            for (const OcSaveField& f : c.fields) {
                if (f.name.empty()) return false;
                const u32 want = ocSaveFloatCount(f.kind);
                if (want != 0) {
                    if (f.f.size() != want) return false;
                } else if (!kindIsInt(f.kind) && f.kind != kKindString) {
                    return false;   // a kind this format has no representation for
                }
                // An ENTITY field may point anywhere in the snapshot, INCLUDING forwards -- unlike a
                // parent. A door referring to the switch that opens it is an ordinary cycle-free
                // reference with no ordering meaning, and restore fixes those up in a second pass
                // precisely so they need no ordering.
                if (f.kind == kKindEntity && (f.i < -1 || f.i >= n)) return false;
            }
        }
    }
    return true;
}

bool writeOcSave(const OcSaveData& in, std::vector<u8>& out, std::string* why) {
    if (!in.valid())
        return fail(why, ".ocsave: refusing to write an invalid snapshot -- a parent index points "
                         "forward, an entity reference is out of range, or a field's value count "
                         "disagrees with its kind");

    AvrStringTable strt;

    std::vector<u8> shdr;
    {
        W w{shdr};
        w.u32v(in.contentVersion);
        w.u32v(strt.add(in.levelPath));
        w.u32v(strt.add(in.engineVersion));
        w.u32v(static_cast<u32>(in.entities.size()));
    }

    std::vector<u8> sent;
    {
        W w{sent};
        for (const OcSaveEntity& en : in.entities) {
            w.u32v(strt.add(en.name));
            w.u64v(en.objectId);
            w.i32v(en.parent);
            w.u32v(strt.add(en.className));
            w.u32v(static_cast<u32>(en.components.size()));
            for (const OcSaveComponent& c : en.components) {
                w.u32v(strt.add(c.type));
                w.u32v(static_cast<u32>(c.fields.size()));
                for (const OcSaveField& f : c.fields) {
                    // THE NAME IS INTERNED, which is most of why this format is not enormous: a
                    // world of a thousand entities carrying CLocal writes "position" once, not a
                    // thousand times.
                    w.u32v(strt.add(f.name));
                    w.u32v(f.kind);
                    const u32 fc = ocSaveFloatCount(f.kind);
                    if (fc != 0)                    { for (u32 k = 0; k < fc; ++k) w.f32v(f.f[k]); }
                    else if (f.kind == kKindString) { w.u32v(strt.add(f.s)); }
                    else                            { w.i64v(f.i); }
                }
            }
        }
    }

    Avr1File f;
    f.subtype = kSubtypeSave;
    f.contentVersion = static_cast<u16>(in.contentVersion);
    f.flags = kAvrFlagCooked;
    f.add(kChunkSHDR, std::move(shdr), kAvrChunkRequired);
    f.add(kChunkSENT, std::move(sent), kAvrChunkRequired);
    // LAST, because everything above interns into it.
    f.add(kChunkSTRT, strt.bytes());
    return writeAvr1(f, out, why);
}

bool parseOcSave(const u8* bytes, usize size, OcSaveData& out, std::string* why) {
    Avr1File f;
    if (!parseAvr1(bytes, size, f, why)) return false;
    if (f.subtype != kSubtypeSave) return fail(why, ".ocsave: container subtype is not SAVE");

    const AvrChunk* shdr = f.find(kChunkSHDR);
    const AvrChunk* sent = f.find(kChunkSENT);
    if (!shdr) return fail(why, ".ocsave: no SHDR chunk");
    if (!sent) return fail(why, ".ocsave: no SENT chunk");

    AvrStringTable strt;
    if (const AvrChunk* s = f.find(kChunkSTRT)) strt.setBytes(s->data);

    R h{shdr->data.data(), shdr->data.data() + shdr->data.size()};
    out.contentVersion = h.u32v();
    out.levelPath     = std::string(strt.get(h.u32v()));
    out.engineVersion = std::string(strt.get(h.u32v()));
    const u32 count   = h.u32v();
    if (!h.ok) return fail(why, ".ocsave: truncated SHDR");

    // BOUNDED BEFORE RESERVING. The smallest an entity can be on disk is 24 bytes (name, objectId,
    // parent, className, componentCount), so a corrupt count cannot ask for an enormous allocation
    // on the way to failing. The reader below still checks every read; this only stops the alloc.
    if (count > sent->data.size() / 24)
        return fail(why, ".ocsave: SENT says it holds more entities than it can");

    out.entities.clear();
    out.entities.resize(count);

    R r{sent->data.data(), sent->data.data() + sent->data.size()};
    for (u32 i = 0; i < count; ++i) {
        OcSaveEntity& en = out.entities[i];
        en.name      = std::string(strt.get(r.u32v()));
        en.objectId  = r.u64v();
        en.parent    = r.i32v();
        en.className = std::string(strt.get(r.u32v()));
        const u32 comps = r.u32v();
        if (!r.ok) return fail(why, ".ocsave: truncated entity record");
        if (comps > sent->data.size() / 8)
            return fail(why, ".ocsave: an entity claims more components than the chunk can hold");
        en.components.resize(comps);
        for (u32 ci = 0; ci < comps; ++ci) {
            OcSaveComponent& c = en.components[ci];
            c.type = std::string(strt.get(r.u32v()));
            const u32 fields = r.u32v();
            if (!r.ok) return fail(why, ".ocsave: truncated component record");
            if (fields > sent->data.size() / 8)
                return fail(why, ".ocsave: a component claims more fields than the chunk can hold");
            c.fields.resize(fields);
            for (u32 fi = 0; fi < fields; ++fi) {
                OcSaveField& fl = c.fields[fi];
                fl.name = std::string(strt.get(r.u32v()));
                fl.kind = r.u32v();
                const u32 fc = ocSaveFloatCount(fl.kind);
                if (fc != 0) {
                    fl.f.resize(fc);
                    for (u32 k = 0; k < fc; ++k) fl.f[k] = r.f32v();
                } else if (fl.kind == kKindString) {
                    fl.s = std::string(strt.get(r.u32v()));
                } else if (kindIsInt(fl.kind)) {
                    fl.i = r.i64v();
                } else {
                    // AN UNKNOWN KIND IS FATAL, unlike an unknown CHUNK. A chunk this reader does
                    // not know is a feature it does not have and can skip over, because the
                    // container records its length. A field kind is not length-prefixed -- the
                    // reader cannot tell how many bytes to skip -- so guessing would desynchronise
                    // every field after it and silently load garbage into the wrong components.
                    return fail(why, ".ocsave: field '" + fl.name + "' has unknown kind " +
                                     std::to_string(fl.kind) + ", and a kind carries no length to "
                                     "skip past");
                }
            }
        }
    }
    if (!r.ok) return fail(why, ".ocsave: truncated entity table");
    if (!out.valid())
        return fail(why, ".ocsave: the file parses but its indices do not hold -- a parent points "
                         "forward, or an entity reference is out of range");
    return true;
}

bool loadOcSave(const std::string& path, OcSaveData& out, std::string* why) {
    aver::traceFileOpen(path);
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return fail(why, ".ocsave: cannot open " + path);
    const std::streamoff n = f.tellg();
    if (n <= 0) return fail(why, ".ocsave: empty file " + path);
    std::vector<u8> bytes(static_cast<usize>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(bytes.data()), n);
    if (!f) return fail(why, ".ocsave: short read on " + path);
    return parseOcSave(bytes.data(), bytes.size(), out, why);
}

bool saveOcSave(const std::string& path, const OcSaveData& in, std::string* why) {
    std::vector<u8> bytes;
    if (!writeOcSave(in, bytes, why)) return false;

    // Matching saveOcworld (OcWorld.cpp:377-381), which creates the parent directory the same way.
    std::error_code ec;
    const std::filesystem::path fsPath(path);
    if (fsPath.has_parent_path()) std::filesystem::create_directories(fsPath.parent_path(), ec);

    // WRITE TO A TEMPORARY AND SWAP. Every other writer in this engine opens the destination with
    // ios::trunc and writes into it, which means a crash or a full disk part-way through leaves the
    // player's save destroyed rather than merely not updated. That is an acceptable outcome for a
    // level file an author can re-save and an unacceptable one for the thing a player asked the game
    // to keep. RegionFile::compact is the only other place in the tree that does this, for the same
    // stated reason, and renameFile is MoveFileExW with
    // MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH -- its own comment calls it "exactly the
    // operation write-to-temp-then-swap needs".
    const std::string tmp = path + ".tmp";
    if (!writeFileBytes(tmp, bytes.data(), bytes.size()))
        return fail(why, ".ocsave: cannot write " + tmp);
    if (!renameFile(tmp, path)) {
        // The temporary is removed on a failed swap so a later attempt does not inherit a stale one
        // and so the save directory does not accumulate debris. The ORIGINAL is untouched either
        // way, which is the whole point.
        deleteFile(tmp);
        return fail(why, ".ocsave: could not replace " + path + " (the previous save is intact)");
    }
    return true;
}

} // namespace aver::fmt
