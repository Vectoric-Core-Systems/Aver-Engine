#include "aver/world/ChunkCodec.hpp"

#if AVER_MODULE_SCENE

#  include "ByteIo.hpp"

namespace aver::world {
namespace {

constexpr u8 kFlagMesh = 1u << 0;
constexpr u8 kFlagBody = 1u << 1;

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

        u8 flags = 0;
        if (e.hasMesh) flags |= kFlagMesh;
        if (e.hasBody) flags |= kFlagBody;
        w.u8v(flags);

        if (e.hasMesh) {
            w.u64v(e.mesh);
            w.str(e.material);          // the NAME. See ChunkPayload.hpp.
            w.u32v(e.meshFlags);
            for (int a = 0; a < 3; ++a) w.f32v(e.aabbMin[a]);
            for (int a = 0; a < 3; ++a) w.f32v(e.aabbMax[a]);
        }
        if (e.hasBody) for (int a = 0; a < 3; ++a) w.f32v(e.bodyHalfExtentCm[a]);
    }
    return std::move(w.bytes);
}

bool decodeChunk(const u8* data, usize size, ChunkPayload& out, std::string* why) {
    const auto fail = [why](const char* m) { if (why) *why = m; return false; };
    if (!data) return fail("chunk payload is null");

    ByteReader r(data, size);
    const u32 version = r.u32v();
    if (r.bad) return fail("chunk payload is too short for its header");
    if (version != kChunkCodecVersion) return fail("chunk payload version is not understood");

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

#endif // AVER_MODULE_SCENE
