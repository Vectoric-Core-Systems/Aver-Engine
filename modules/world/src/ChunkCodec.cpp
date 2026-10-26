#include "aver/world/ChunkCodec.hpp"

#include "aver/core/Log.hpp"
#include "ByteIo.hpp"

namespace aver::world {
namespace {

constexpr u8 kFlagMesh = 1u << 0;
constexpr u8 kFlagBody = 1u << 1;
constexpr u8 kFlagExtras = 1u << 2;   // v2: an extras list follows the body box
constexpr u8 kKnownFlagsV2 = kFlagMesh | kFlagBody | kFlagExtras;

// A sanity ceiling on the entity count, so a corrupt length cannot make the decoder reserve
// gigabytes before it discovers the buffer is short. 1M entities is far past any real chunk and far
// below anything that hurts.
constexpr u32 kMaxEntitiesPerChunk = 1'000'000;

} // namespace

std::vector<u8> encodeChunk(const ChunkPayload& p) {
    ByteWriter w;
    w.u32v(kChunkCodecVersion);
    w.u32v(static_cast<u32>(p.entities.size()));
    for (const PayloadEntity& e : p.entities) {
        w.str(e.name);
        w.u64v(e.objectId);
        w.i32v(e.parent);
        w.f32v(e.local.position.x); w.f32v(e.local.position.y); w.f32v(e.local.position.z);
        w.f32v(e.local.rotation.x); w.f32v(e.local.rotation.y);
        w.f32v(e.local.rotation.z); w.f32v(e.local.rotation.w);
        w.f32v(e.local.scale.x); w.f32v(e.local.scale.y); w.f32v(e.local.scale.z);
        w.u32v(e.tags);

        // An extra the u16 fields cannot hold is dropped, loudly: the encoder has no error path and a truncated TLV would
        // decode as garbage. The typed packer (ChunkPayloadExtras) never produces one.
        usize extraCount = 0;
        for (const PayloadExtra& x : e.extras)
            if (x.bytes.size() <= kMaxExtraBytes && extraCount < kMaxExtrasPerEntity) ++extraCount;
            else AVER_WARN("[ChunkCodec] dropping a {}-byte extra (tag {:#x}) that does not fit a chunk payload record",
                           x.bytes.size(), x.tag);

        u8 flags = 0;
        if (e.hasMesh) flags |= kFlagMesh;
        if (e.hasBody) flags |= kFlagBody;
        if (extraCount) flags |= kFlagExtras;
        w.u8v(flags);

        if (e.hasMesh) {
            w.u64v(e.mesh);
            w.str(e.material);          // the NAME. See ChunkPayload.hpp.
            w.u32v(e.meshFlags);
            for (int a = 0; a < 3; ++a) w.f32v(e.aabbMin[a]);
            for (int a = 0; a < 3; ++a) w.f32v(e.aabbMax[a]);
        }
        if (e.hasBody) for (int a = 0; a < 3; ++a) w.f32v(e.bodyHalfExtentCm[a]);
        if (extraCount) {
            w.u16v(static_cast<u16>(extraCount));
            usize written = 0;
            for (const PayloadExtra& x : e.extras) {
                if (x.bytes.size() > kMaxExtraBytes || written == extraCount) continue;
                w.u32v(x.tag);
                w.u16v(static_cast<u16>(x.bytes.size()));
                w.bytes.insert(w.bytes.end(), x.bytes.begin(), x.bytes.end());
                ++written;
            }
        }
    }
    return std::move(w.bytes);
}

bool decodeChunk(const u8* data, usize size, ChunkPayload& out, std::string* why) {
    const auto fail = [why](const char* m) { if (why) *why = m; return false; };
    if (!data) return fail("chunk payload is null");

    ByteReader r(data, size);
    const u32 version = r.u32v();
    if (r.bad) return fail("chunk payload is too short for its header");
    if (version < kChunkCodecVersionMin || version > kChunkCodecVersion) return fail("chunk payload version is not understood");

    const u32 count = r.u32v();
    if (r.bad) return fail("chunk payload is too short for its entity count");
    if (count > kMaxEntitiesPerChunk) return fail("chunk payload declares an implausible entity count");

    out.entities.clear();
    out.entities.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        PayloadEntity e;
        e.name = r.str();
        e.objectId = r.u64v();
        e.parent = r.i32v();
        e.local.position = Vec3{r.f32v(), r.f32v(), r.f32v()};
        const f32 qx = r.f32v(), qy = r.f32v(), qz = r.f32v(), qw = r.f32v();
        e.local.rotation = Quat{qx, qy, qz, qw};
        e.local.scale = Vec3{r.f32v(), r.f32v(), r.f32v()};
        e.tags = r.u32v();

        const u8 flags = r.u8v();
        // v1 never checked its flags; v2 refuses a bit it cannot interpret, since an unread field desynchronises the stream.
        if (version >= 2 && !r.bad && (flags & ~kKnownFlagsV2) != 0)
            return fail("chunk payload entity has flag bits this reader does not know");
        e.hasMesh = (flags & kFlagMesh) != 0;
        e.hasBody = (flags & kFlagBody) != 0;

        if (e.hasMesh) {
            e.mesh = r.u64v();
            e.material = r.str();
            e.meshFlags = r.u32v();
            for (int a = 0; a < 3; ++a) e.aabbMin[a] = r.f32v();
            for (int a = 0; a < 3; ++a) e.aabbMax[a] = r.f32v();
        }
        if (e.hasBody) for (int a = 0; a < 3; ++a) e.bodyHalfExtentCm[a] = r.f32v();
        if (version >= 2 && (flags & kFlagExtras)) {
            const u16 n = r.u16v();
            // Each extra is at least 6 bytes, so a count the remaining buffer cannot hold is corrupt before anything is reserved.
            if (!r.bad && static_cast<usize>(n) * 6 > r.left) return fail("chunk payload extras count exceeds the buffer");
            e.extras.reserve(n);
            for (u16 k = 0; k < n && !r.bad; ++k) {
                PayloadExtra x;
                x.tag = r.u32v();
                const u16 len = r.u16v();
                if (!r.need(len)) break;
                x.bytes.assign(r.p, r.p + len);
                r.p += len; r.left -= len;
                e.extras.push_back(std::move(x));
            }
        }

        if (r.bad) return fail("chunk payload ends in the middle of an entity");

        // A parent index must point BACKWARDS, at an entity already decoded. Forward or self
        // references would build a cycle in an intrusive hierarchy, which is a hang or a crash
        // rather than a wrong picture -- so it is rejected here, at the boundary, and not trusted.
        if (e.parent >= 0 && static_cast<u32>(e.parent) >= i)
            return fail("chunk payload has a parent index that is not already decoded");

        out.entities.push_back(std::move(e));
    }
    // Trailing bytes mean the writer and reader disagree about the format, which is exactly the
    // situation where carrying on produces something plausible and wrong.
    if (r.left != 0) return fail("chunk payload has unread trailing bytes");
    return true;
}

} // namespace aver::world
