// Chunk payload codec v2 and the typed placement extras (LARGE_WORLD 2.4.3). Exit code = failure count.
//
//   V1 STILL READS       a hand-built version 1 stream decodes, with no extras.
//   COSTS NOTHING        an entity without extras encodes to the v1 bytes, only the version word differs.
//   ROUND TRIP           extras survive encode and decode in order, a tag may repeat, an UNKNOWN tag is kept.
//   TYPED EXTRAS         PLID, ANIM, COLL, CMPN (ordered, several), LABL, SNAP round-trip through set/get; set replaces only
//                        the known tags; a value the u16 fields cannot hold is refused and leaves the entity alone.
//   REFUSALS             a truncated extras list, a count the buffer cannot hold, an unknown flag bit and a malformed known tag.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/world/ChunkCodec.hpp"
#include "aver/world/ChunkPayloadExtras.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::world;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

// Little-endian writer for the hand-built version 1 stream (independent of the codec's own).
struct Le {
    std::vector<u8> b;
    void u8v(u8 v) { b.push_back(v); }
    void u16v(u16 v) { for (int i = 0; i < 2; ++i) b.push_back(static_cast<u8>(v >> (8 * i))); }
    void u32v(u32 v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(v >> (8 * i))); }
    void u64v(u64 v) { for (int i = 0; i < 8; ++i) b.push_back(static_cast<u8>(v >> (8 * i))); }
    void f32v(f32 v) { u32 x; std::memcpy(&x, &v, 4); u32v(x); }
    void str(const std::string& s) { u16v(static_cast<u16>(s.size())); b.insert(b.end(), s.begin(), s.end()); }
};

// A v1 entity record for makePayload(): the same fields the v2 encoder writes for an entity without extras.
static void v1Entity(Le& w, const PayloadEntity& e) {
    w.str(e.name); w.u64v(e.objectId); w.u32v(static_cast<u32>(e.parent));
    w.f32v(e.local.position.x); w.f32v(e.local.position.y); w.f32v(e.local.position.z);
    w.f32v(e.local.rotation.x); w.f32v(e.local.rotation.y); w.f32v(e.local.rotation.z); w.f32v(e.local.rotation.w);
    w.f32v(e.local.scale.x); w.f32v(e.local.scale.y); w.f32v(e.local.scale.z);
    w.u32v(e.tags);
    w.u8v(static_cast<u8>((e.hasMesh ? 1 : 0) | (e.hasBody ? 2 : 0)));
    if (e.hasMesh) {
        w.u64v(e.mesh); w.str(e.material); w.u32v(e.meshFlags);
        for (int a = 0; a < 3; ++a) w.f32v(e.aabbMin[a]);
        for (int a = 0; a < 3; ++a) w.f32v(e.aabbMax[a]);
    }
    if (e.hasBody) for (int a = 0; a < 3; ++a) w.f32v(e.bodyHalfExtentCm[a]);
}

static PayloadEntity makeEntity(u32 salt, bool mesh, bool body) {
    PayloadEntity e;
    e.name = "Meshes/prop_" + std::to_string(salt) + ".ocmesh";
    e.objectId = 0xAB00'0000'0000'0000ull + salt;
    e.parent = -1;
    e.local.position = Vec3{100.25f + static_cast<f32>(salt), 7.5f, -3.0f};
    e.local.rotation = Quat{0.0f, 0.0f, 0.38268343f, 0.92387953f};
    e.local.scale = Vec3{1.0f, 2.0f, 0.5f};
    e.tags = salt * 3;
    e.hasMesh = mesh;
    if (mesh) {
        e.mesh = 0x1234'5678'9ABC'DEF0ull ^ salt;
        e.material = "M_Wall";
        e.meshFlags = 5;
        e.aabbMin[0] = -10.0f; e.aabbMax[2] = 40.0f;
    }
    e.hasBody = body;
    if (body) { e.bodyHalfExtentCm[0] = 50.0f; e.bodyHalfExtentCm[2] = 12.5f; }
    return e;
}

static bool sameExtras(const std::vector<PayloadExtra>& a, const std::vector<PayloadExtra>& b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i)
        if (a[i].tag != b[i].tag || a[i].bytes != b[i].bytes) return false;
    return true;
}

int main() {
    // ---- version 1 streams still decode, and an entity without extras costs nothing over v1 ----------------
    {
        ChunkPayload p;
        p.entities.push_back(makeEntity(1, true, false));
        p.entities.push_back(makeEntity(2, false, true));
        p.entities.push_back(makeEntity(3, true, true));
        p.entities[1].parent = 0;

        Le v1;
        v1.u32v(1); v1.u32v(static_cast<u32>(p.entities.size()));
        for (const PayloadEntity& e : p.entities) v1Entity(v1, e);

        ChunkPayload got;
        std::string why;
        check(decodeChunk(v1.b.data(), v1.b.size(), got, &why), "a version 1 stream decodes");
        check(got.entities.size() == 3 && got.entities[0].name == p.entities[0].name &&
              got.entities[2].material == "M_Wall" && got.entities[1].parent == 0 &&
              got.entities[1].bodyHalfExtentCm[2] == 12.5f, "...with every v1 field intact");
        check(got.entities[0].extras.empty() && got.entities[1].extras.empty() && got.entities[2].extras.empty(),
              "...and no extras");

        const std::vector<u8> enc = encodeChunk(p);
        std::vector<u8> asV1 = enc;
        check(asV1.size() >= 4 && asV1[0] == 2, "the encoder writes version 2");
        asV1[0] = 1;
        check(asV1 == v1.b, "an entity without extras encodes to exactly the v1 bytes (only the version word differs)");
    }

    // ---- extras round trip: order, repeats, unknown tags ------------------------------------------------------------
    {
        ChunkPayload p;
        PayloadEntity a = makeEntity(10, true, false);
        a.extras.push_back(PayloadExtra{kExtraPlacementId, {1, 2, 3, 4, 5, 6, 7, 8}});
        a.extras.push_back(PayloadExtra{extraTag('Z', 'Z', 'Z', '1'), {9, 9, 9}});          // a tag this build has never heard of
        a.extras.push_back(PayloadExtra{kExtraComponent, {1}});
        a.extras.push_back(PayloadExtra{kExtraComponent, {}});                              // repeated tag, empty body
        p.entities.push_back(a);
        p.entities.push_back(makeEntity(11, true, false));                                  // no extras between two that have some
        PayloadEntity c = makeEntity(12, false, false);
        c.parent = 0;
        c.extras.push_back(PayloadExtra{kExtraLabel, {0, 0}});
        p.entities.push_back(c);

        const std::vector<u8> e1 = encodeChunk(p);
        check(e1 == encodeChunk(p), "encoding a payload with extras twice gives identical bytes");
        ChunkPayload back;
        std::string why;
        check(decodeChunk(e1.data(), e1.size(), back, &why), "it decodes");
        check(back.entities.size() == 3 && sameExtras(back.entities[0].extras, a.extras),
              "extras keep their order, repeated tags and unknown tags");
        check(back.entities[1].extras.empty(), "an entity between two with extras has none");
        check(sameExtras(back.entities[2].extras, c.extras), "a child's extras survive");
        check(encodeChunk(back) == e1, "decode then encode is the identity");

        // Truncation anywhere is refused, never read as a shorter valid payload.
        bool anyAccepted = false;
        for (usize cut = 0; cut + 1 < e1.size(); ++cut) {
            ChunkPayload junk;
            if (decodeChunk(e1.data(), cut, junk, nullptr)) anyAccepted = true;
        }
        check(!anyAccepted, "EVERY truncation of a payload with extras is refused");
    }

    // ---- refusals --------------------------------------------------------------------------------------------------------
    {
        ChunkPayload p;
        PayloadEntity e = makeEntity(20, false, false);
        e.extras.push_back(PayloadExtra{kExtraSnap, {1}});
        p.entities.push_back(e);
        const std::vector<u8> good = encodeChunk(p);
        ChunkPayload junk;
        std::string why;

        // The extras count is the two bytes after the flags byte; claim more than the buffer can hold.
        std::vector<u8> bad = good;
        const usize countAt = bad.size() - (2 + 4 + 2 + 1);   // u16 count, then tag u32, len u16, 1 byte
        bad[countAt] = 0xFF; bad[countAt + 1] = 0x7F;
        check(!decodeChunk(bad.data(), bad.size(), junk, &why), "a count the buffer cannot hold is refused");

        // An extra that claims more bytes than remain.
        bad = good;
        bad[bad.size() - 3] = 200;
        check(!decodeChunk(bad.data(), bad.size(), junk, &why), "an extra longer than the buffer is refused");

        // An unknown flag bit in a v2 payload: an unread field would desynchronise everything after it.
        bad = good;
        const usize flagsAt = bad.size() - (2 + 4 + 2 + 1) - 1;
        check(bad[flagsAt] == 4, "(the flags byte is where the test expects it)");
        bad[flagsAt] = 0x80 | 4;
        check(!decodeChunk(bad.data(), bad.size(), junk, &why), "an unknown flag bit is refused in version 2");

        // A version this reader does not know.
        bad = good;
        bad[0] = 3;
        check(!decodeChunk(bad.data(), bad.size(), junk, &why), "version 3 is refused");
        bad[0] = 0;
        check(!decodeChunk(bad.data(), bad.size(), junk, &why), "version 0 is refused");

        // An extra the u16 length cannot hold is dropped by the encoder (with a warning), not truncated into garbage.
        ChunkPayload big;
        PayloadEntity f = makeEntity(21, false, false);
        f.extras.push_back(PayloadExtra{kExtraLabel, std::vector<u8>(70000, 1)});
        f.extras.push_back(PayloadExtra{kExtraSnap, {1}});
        big.entities.push_back(f);
        const std::vector<u8> be = encodeChunk(big);
        ChunkPayload bb;
        check(decodeChunk(be.data(), be.size(), bb, &why) && bb.entities.size() == 1, "a payload with an oversize extra still decodes");
        check(bb.entities[0].extras.size() == 1 && bb.entities[0].extras[0].tag == kExtraSnap,
              "...with the oversize extra dropped and the others kept");
    }

    // ---- typed extras ---------------------------------------------------------------------------------------------------
    {
        PayloadEntity e = makeEntity(30, true, false);

        PlacementExtras x;
        x.hasPlacementId = true; x.placementId = 0xFEED'FACE'0000'0042ull;
        x.hasAnim = true; x.animClip = "Anim/spin.ocanim"; x.animSpeed = 1.5f; x.animTime = 0.25f; x.animOnce = true;
        x.hasCollide = true; x.collide = false;
        fmt::OcPlacedComponent c1; c1.type = "Light"; c1.kv = {{"color", "1 0.5 0"}, {"intensity", "250"}};
        fmt::OcPlacedComponent c2; c2.type = "Trigger"; c2.kv = {};
        fmt::OcPlacedComponent c3; c3.type = "Sound"; c3.kv = {{"clip", "a/b.wav"}, {"loop", "1"}, {"gain", "0.5"}};
        x.components = {c1, c2, c3};
        x.hasLabel = true; x.label = "Lobby sign";
        x.snapToGround = true;

        std::string why;
        check(setPlacementExtras(e, x, &why), "setPlacementExtras accepts a full set");
        check(e.extras.size() == 8, "...as PLID, ANIM, COLL, three CMPN, LABL, SNAP");
        check(e.extras[0].tag == kExtraPlacementId && e.extras[3].tag == kExtraComponent && e.extras[7].tag == kExtraSnap,
              "...in the documented order");

        PlacementExtras y;
        check(getPlacementExtras(e, y, &why), "getPlacementExtras reads them back");
        check(y.hasPlacementId && y.placementId == x.placementId, "PLID round-trips");
        check(y.hasAnim && y.animClip == x.animClip && y.animSpeed == 1.5f && y.animTime == 0.25f && y.animOnce, "ANIM round-trips");
        check(y.hasCollide && !y.collide, "COLL round-trips (false is not 'absent')");
        check(y.components.size() == 3 && y.components[0] == c1 && y.components[1] == c2 && y.components[2] == c3,
              "CMPN round-trips in order, including a component with no keys");
        check(y.hasLabel && y.label == x.label, "LABL round-trips");
        check(y.snapToGround, "SNAP round-trips");

        // Through the codec too.
        ChunkPayload p;
        p.entities.push_back(e);
        const std::vector<u8> enc = encodeChunk(p);
        ChunkPayload back;
        check(decodeChunk(enc.data(), enc.size(), back, &why), "the entity encodes and decodes");
        PlacementExtras z;
        check(getPlacementExtras(back.entities[0], z, &why) && z.placementId == x.placementId && z.components.size() == 3 &&
              z.components[2] == c3 && z.label == x.label, "...and the typed view survives the codec");

        // set replaces the known tags and leaves the unknown ones.
        e.extras.insert(e.extras.begin() + 1, PayloadExtra{extraTag('Q', 'Q', 'Q', 'Q'), {7}});
        PlacementExtras only;
        only.hasLabel = true; only.label = "new";
        check(setPlacementExtras(e, only, &why), "set again with a smaller set");
        check(e.extras.size() == 2 && e.extras[0].tag == extraTag('Q', 'Q', 'Q', 'Q') && e.extras[1].tag == kExtraLabel,
              "...replaces every known tag, keeps the unknown one, appends the new");
        PlacementExtras none;
        check(getPlacementExtras(e, none, &why) && !none.hasPlacementId && !none.hasAnim && !none.hasCollide &&
              none.components.empty() && !none.snapToGround && none.hasLabel, "a missing tag reads as its default with has* false");

        // A value the codec cannot hold is refused and the entity is untouched.
        PayloadEntity keep = makeEntity(31, false, false);
        keep.extras.push_back(PayloadExtra{kExtraSnap, {1}});
        const std::vector<PayloadExtra> before = keep.extras;
        PlacementExtras huge;
        huge.hasLabel = true; huge.label = std::string(70000, 'x');
        check(!setPlacementExtras(keep, huge, &why), "a 70,000-character label is refused");
        check(sameExtras(keep.extras, before), "...and the entity's extras are untouched");
        PlacementExtras fat;
        fmt::OcPlacedComponent big; big.type = "Big";
        for (int i = 0; i < 40; ++i) big.kv.emplace_back("k" + std::to_string(i), std::string(2000, 'v'));
        fat.components = {big};
        check(!setPlacementExtras(keep, fat, &why), "a component larger than one extra can hold is refused");
        check(why.find("Big") != std::string::npos, "...and the reason names it: " + why);
    }

    // ---- malformed known tags are named -------------------------------------------------------------------------------
    {
        PayloadEntity e = makeEntity(40, false, false);
        e.extras.push_back(PayloadExtra{kExtraPlacementId, {1, 2, 3}});   // 3 bytes, not 8
        PlacementExtras out;
        std::string why;
        check(!getPlacementExtras(e, out, &why), "a PLID of the wrong length is refused");
        check(why.find("PLID") != std::string::npos, "...naming the tag: " + why);

        PayloadEntity d = makeEntity(41, false, false);
        d.extras.push_back(PayloadExtra{kExtraSnap, {1}});
        d.extras.push_back(PayloadExtra{kExtraSnap, {1}});
        check(!getPlacementExtras(d, out, &why) && why.find("SNAP") != std::string::npos, "a repeated SNAP is refused by name");

        PayloadEntity t = makeEntity(42, false, false);
        t.extras.push_back(PayloadExtra{kExtraComponent, {5, 0, 'a', 'b'}});   // type "ab"? length 5 but only 2 bytes follow
        check(!getPlacementExtras(t, out, &why) && why.find("CMPN") != std::string::npos, "a truncated CMPN is refused by name");

        PayloadEntity u = makeEntity(43, false, false);
        u.extras.push_back(PayloadExtra{extraTag('N', 'E', 'W', 'T'), {1, 2, 3}});
        check(getPlacementExtras(u, out, &why), "a tag a newer cook wrote is skipped, not an error");
    }

    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
