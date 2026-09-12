// Reader and writer for .ocfoliage. See the header for the field shape (it mirrors
// aver::world::ScatterSpecies one for one) and for the two fields deliberately left out.
#include "aver/formats/OcFoliage.hpp"

#include "aver/platform/FileSystem.hpp"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace aver::fmt {
namespace {

constexpr u32 kChunkFHDR = avrFourCC("FHDR");
constexpr u32 kChunkSTRT = avrFourCC("STRT");
constexpr u32 kSubtypeFoliage = avrFourCC("FOLI");

bool fail(std::string* why, std::string m) { if (why) *why = std::move(m); return false; }

// Little-endian byte packing, copied rather than shared -- OcBt.cpp/OcNav.cpp's own comment on this
// exact duplication applies here unchanged: every .oc* format carries its own tiny W/R pair.
struct W {
    std::vector<u8>& b;
    void u8v (u8 v)  { b.push_back(v); }
    void u32v(u32 v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(v >> (i * 8))); }
    void f32v(f32 v) { u32 x; std::memcpy(&x, &v, 4); u32v(x); }
};

// UNLIKE OcBt.cpp's R, a failed read here does NOT leave the field at binary zero -- see the header's
// own "ABSENT-VS-ZERO RULE" comment. `ok` still latches false permanently once the payload runs out
// (so every field after a truncation point is correctly reported as unread), but each call site below
// only commits the read value into OcFoliageData when `ok` is still true immediately afterward,
// leaving an unread field at whatever OcFoliageData's own member initialiser already put there.
struct R {
    const u8* p; const u8* e; bool ok = true;
    bool need(usize n) { if (static_cast<usize>(e - p) < n) { ok = false; return false; } return true; }
    u8  u8v () { if (!need(1)) return 0; return *p++; }
    u32 u32v() { if (!need(4)) return 0; u32 v; std::memcpy(&v, p, 4); p += 4; return v; }
    f32 f32v() { const u32 x = u32v(); f32 f; std::memcpy(&f, &x, 4); return f; }
};

} // namespace

bool OcFoliageData::valid() const {
    if (meshPath.empty()) return false;
    if (!std::isfinite(scaleMin) || !std::isfinite(scaleMax)) return false;
    if (!(scaleMin > 0.0f)) return false;
    if (scaleMax < scaleMin) return false;
    if (!std::isfinite(weight)) return false;
    if (!std::isfinite(collisionRadiusCm) || collisionRadiusCm < 0.0f) return false;
    return true;
}

bool writeOcFoliage(const OcFoliageData& in, std::vector<u8>& out, std::string* why) {
    if (!in.valid())
        return fail(why, ".ocfoliage: refusing to write an invalid type -- meshPath must be set, "
                         "scaleMin must be positive and no greater than scaleMax, and weight/"
                         "collisionRadiusCm must be finite with collisionRadiusCm >= 0");

    AvrStringTable strt;
    const u32 meshRef = strt.add(in.meshPath);
    const u32 matRef  = strt.add(in.material);

    std::vector<u8> fhdr;
    {
        W w{fhdr};
        w.u32v(meshRef);
        w.u32v(matRef);
        w.f32v(in.scaleMin);
        w.f32v(in.scaleMax);
        w.f32v(in.weight);
        w.u8v(in.randomizeYaw ? 1 : 0);
        w.f32v(in.collisionRadiusCm);
        w.u8v(in.alignToNormal ? 1 : 0);
    }

    Avr1File f;
    f.subtype = kSubtypeFoliage;
    f.contentVersion = 1;
    f.flags = kAvrFlagCooked;
    f.add(kChunkFHDR, std::move(fhdr), kAvrChunkRequired);
    f.add(kChunkSTRT, strt.bytes());
    return writeAvr1(f, out, why);
}

bool parseOcFoliage(const u8* bytes, usize size, OcFoliageData& out, std::string* why) {
    Avr1File f;
    if (!parseAvr1(bytes, size, f, why)) return false;
    if (f.subtype != kSubtypeFoliage) return fail(why, ".ocfoliage: container subtype is not FOLI");

    const AvrChunk* fhdr = f.find(kChunkFHDR);
    if (!fhdr) return fail(why, ".ocfoliage: no FHDR chunk");

    AvrStringTable strt;
    if (const AvrChunk* s = f.find(kChunkSTRT)) strt.setBytes(s->data);

    // Starts from OcFoliageData's own compiled-in defaults (scaleMin 0.75, scaleMax 1.25, weight 1.0,
    // randomizeYaw true, collisionRadiusCm 0, alignToNormal false) -- see the header's "ABSENT-VS-ZERO
    // RULE" comment. Every field below is committed only when the read that produced it succeeded, so
    // a chunk shorter than this reader expects (an old writer predating a later additive field, or
    // OcFoliageTest's synthetic truncation) leaves the untouched tail at these defaults, never at
    // binary zero -- the distinction that matters for randomizeYaw (default true) and weight (default
    // 1.0), neither of which a blind zero-fill would reproduce.
    OcFoliageData tmp;

    R r{fhdr->data.data(), fhdr->data.data() + fhdr->data.size()};

    // meshPath is REQUIRED -- unlike every field after it, a file too short to carry even this one is
    // a truncated/corrupt file, not an old-but-valid one, so this fails outright rather than defaulting.
    const u32 meshRef = r.u32v();
    if (!r.ok) return fail(why, ".ocfoliage: FHDR ended before meshPath");
    tmp.meshPath = std::string(strt.get(meshRef));
    if (tmp.meshPath.empty()) return fail(why, ".ocfoliage: meshPath is empty");

    const u32 matRef = r.u32v();
    if (r.ok) tmp.material = std::string(strt.get(matRef));

    const f32 sMin = r.f32v();
    if (r.ok) tmp.scaleMin = sMin;
    const f32 sMax = r.f32v();
    if (r.ok) tmp.scaleMax = sMax;
    const f32 w = r.f32v();
    if (r.ok) tmp.weight = w;
    const u8 ry = r.u8v();
    if (r.ok) tmp.randomizeYaw = (ry != 0);
    const f32 cr = r.f32v();
    if (r.ok) tmp.collisionRadiusCm = cr;
    const u8 an = r.u8v();
    if (r.ok) tmp.alignToNormal = (an != 0);

    if (!tmp.valid())
        return fail(why, ".ocfoliage: the file parses but its fields do not hold together "
                         "(scaleMin/scaleMax/weight/collisionRadiusCm)");
    out = tmp;
    return true;
}

bool loadOcFoliage(const std::string& path, OcFoliageData& out, std::string* why) {
    aver::traceFileOpen(path);
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return fail(why, ".ocfoliage: cannot open " + path);
    const std::streamoff n = in.tellg();
    if (n <= 0) return fail(why, ".ocfoliage: empty file " + path);
    std::vector<u8> bytes(static_cast<usize>(n));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(bytes.data()), n);
    if (!in) return fail(why, ".ocfoliage: short read on " + path);
    return parseOcFoliage(bytes.data(), bytes.size(), out, why);
}

bool saveOcFoliage(const std::string& path, const OcFoliageData& in, std::string* why) {
    std::vector<u8> bytes;
    if (!writeOcFoliage(in, bytes, why)) return false;
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    // NOT atomic, matching .ocbt's own choice (OcBt.cpp) and for the identical reason: a foliage type
    // is authored from its own editor tab, which is the one place that regenerates it, not a player's
    // save file.
    if (!writeFileBytes(path, bytes.data(), bytes.size()))
        return fail(why, ".ocfoliage: cannot write " + path);
    return true;
}

} // namespace aver::fmt
