#include "aver/world/ChunkPayloadExtras.hpp"

#include "aver/world/ChunkCodec.hpp"
#include "ByteIo.hpp"

#include <algorithm>

namespace aver::world {
namespace {

bool isKnown(u32 tag) {
    return tag == kExtraPlacementId || tag == kExtraAnim || tag == kExtraCollide || tag == kExtraComponent ||
           tag == kExtraLabel || tag == kExtraSnap || tag == kExtraEuler;
}

// A string the codec's u16 length can carry.
bool fits(const std::string& s) { return s.size() <= 0xFFFF; }

void push(std::vector<PayloadExtra>& v, u32 tag, ByteWriter& w) { v.push_back(PayloadExtra{tag, std::move(w.bytes)}); }

} // namespace

const PayloadExtra* findExtra(const PayloadEntity& e, u32 tag) {
    for (const PayloadExtra& x : e.extras)
        if (x.tag == tag) return &x;
    return nullptr;
}

bool setPlacementExtras(PayloadEntity& e, const PlacementExtras& x, std::string* why) {
    const auto fail = [why](const std::string& m) { if (why) *why = m; return false; };

    // Build the new known tags first, so a value that cannot fit leaves `e` as it was.
    std::vector<PayloadExtra> fresh;
    if (x.hasPlacementId) {
        ByteWriter w;
        w.u64v(x.placementId);
        push(fresh, kExtraPlacementId, w);
    }
    if (x.hasAnim) {
        if (!fits(x.animClip)) return fail("animation clip path is longer than a payload string");
        ByteWriter w;
        w.str(x.animClip);
        w.f32v(x.animSpeed);
        w.f32v(x.animTime);
        w.u8v(x.animOnce ? 1 : 0);
        push(fresh, kExtraAnim, w);
    }
    if (x.hasCollide) {
        ByteWriter w;
        w.u8v(x.collide ? 1 : 0);
        push(fresh, kExtraCollide, w);
    }
    for (const fmt::OcPlacedComponent& c : x.components) {
        if (c.kv.size() > 0xFFFF) return fail("component '" + c.type + "' has too many keys for a payload");
        ByteWriter w;
        if (!fits(c.type)) return fail("component type name is longer than a payload string");
        w.str(c.type);
        w.u16v(static_cast<u16>(c.kv.size()));
        for (const auto& kv : c.kv) {
            if (!fits(kv.first) || !fits(kv.second)) return fail("component '" + c.type + "' key '" + kv.first + "' does not fit a payload string");
            w.str(kv.first);
            w.str(kv.second);
        }
        if (w.bytes.size() > kMaxExtraBytes) return fail("component '" + c.type + "' is larger than one payload extra can hold");
        push(fresh, kExtraComponent, w);
    }
    if (x.hasLabel) {
        if (!fits(x.label)) return fail("label is longer than a payload string");
        ByteWriter w;
        w.str(x.label);
        push(fresh, kExtraLabel, w);
    }
    if (x.snapToGround) {
        ByteWriter w;
        w.u8v(1);
        push(fresh, kExtraSnap, w);
    }
    if (x.hasEuler) {
        ByteWriter w;
        w.f32v(x.euler[0]);
        w.f32v(x.euler[1]);
        w.f32v(x.euler[2]);
        push(fresh, kExtraEuler, w);
    }
    if (e.extras.size() + fresh.size() > kMaxExtrasPerEntity) return fail("too many extras for one payload entity");

    // Drop the old known tags, keep everything else in order, then append the new ones.
    e.extras.erase(std::remove_if(e.extras.begin(), e.extras.end(), [](const PayloadExtra& p) { return isKnown(p.tag); }),
                   e.extras.end());
    for (PayloadExtra& f : fresh) e.extras.push_back(std::move(f));
    return true;
}

bool getPlacementExtras(const PayloadEntity& e, PlacementExtras& out, std::string* why) {
    const auto fail = [why](const char* tag, const char* m) {
        if (why) *why = std::string("payload extra ") + tag + ": " + m;
        return false;
    };
    out = PlacementExtras{};
    bool sawPlid = false, sawAnim = false, sawColl = false, sawLabl = false, sawSnap = false, sawEuler = false;

    for (const PayloadExtra& x : e.extras) {
        ByteReader r(x.bytes.data(), x.bytes.size());
        if (x.tag == kExtraPlacementId) {
            if (sawPlid) return fail("PLID", "appears twice");
            sawPlid = true;
            out.hasPlacementId = true;
            out.placementId = r.u64v();
            if (r.bad || r.left) return fail("PLID", "is not exactly 8 bytes");
        } else if (x.tag == kExtraAnim) {
            if (sawAnim) return fail("ANIM", "appears twice");
            sawAnim = true;
            out.hasAnim = true;
            out.animClip = r.str();
            out.animSpeed = r.f32v();
            out.animTime = r.f32v();
            out.animOnce = r.u8v() != 0;
            if (r.bad || r.left) return fail("ANIM", "is truncated or has trailing bytes");
        } else if (x.tag == kExtraCollide) {
            if (sawColl) return fail("COLL", "appears twice");
            sawColl = true;
            out.hasCollide = true;
            out.collide = r.u8v() != 0;
            if (r.bad || r.left) return fail("COLL", "is not exactly 1 byte");
        } else if (x.tag == kExtraComponent) {
            fmt::OcPlacedComponent c;
            c.type = r.str();
            const u16 n = r.u16v();
            // Every pair is at least two length words, so a count the buffer cannot hold is corrupt before anything is reserved.
            if (r.bad || static_cast<usize>(n) * 4 > r.left) return fail("CMPN", "key count exceeds the extra");
            c.kv.reserve(n);
            for (u16 i = 0; i < n && !r.bad; ++i) {
                std::string k = r.str();
                std::string v = r.str();
                c.kv.emplace_back(std::move(k), std::move(v));
            }
            if (r.bad || r.left) return fail("CMPN", "is truncated or has trailing bytes");
            out.components.push_back(std::move(c));
        } else if (x.tag == kExtraLabel) {
            if (sawLabl) return fail("LABL", "appears twice");
            sawLabl = true;
            out.hasLabel = true;
            out.label = r.str();
            if (r.bad || r.left) return fail("LABL", "is truncated or has trailing bytes");
        } else if (x.tag == kExtraSnap) {
            if (sawSnap) return fail("SNAP", "appears twice");
            sawSnap = true;
            out.snapToGround = r.u8v() != 0;
            if (r.bad || r.left) return fail("SNAP", "is not exactly 1 byte");
        } else if (x.tag == kExtraEuler) {
            if (sawEuler) return fail("EULR", "appears twice");
            sawEuler = true;
            out.hasEuler = true;
            out.euler[0] = r.f32v();
            out.euler[1] = r.f32v();
            out.euler[2] = r.f32v();
            if (r.bad || r.left) return fail("EULR", "is not exactly 12 bytes");
        }
        // Any other tag is a newer cook's; skipped, never an error.
    }
    return true;
}

} // namespace aver::world
