// The decal C ABI's translation unit: one process-wide DecalPool over the global world.
#include "aver/scene/decal_abi.h"

#include "aver/scene/Components.hpp"
#include "aver/scene/DecalPool.hpp"
#include "aver/scene/World.hpp"

#include <algorithm>
#include <cstring>

using namespace aver;
using namespace aver::scene;

static_assert(AVER_DECAL_NO_COLOUR    == kDecalNoColour,    "AVER_DECAL_NO_COLOUR drift");
static_assert(AVER_DECAL_NO_NORMAL    == kDecalNoNormal,    "AVER_DECAL_NO_NORMAL drift");
static_assert(AVER_DECAL_NO_ROUGHNESS == kDecalNoRoughness, "AVER_DECAL_NO_ROUGHNESS drift");
static_assert(sizeof(AverDecalSpawnDesc) % 8 == 0, "AverDecalSpawnDesc must be padding-free for the C# mirror");

namespace {

constexpr u32 kDefaultCapacity = 256;
constexpr u32 kMaxCapacity = 8192;

DecalPool& pool() {
    static DecalPool p(kDefaultCapacity);
    return p;
}
DecalPool& active() { return pool(); }

} // namespace

extern "C" {

int32_t aver_decal_abi_version(void) { return AVER_DECAL_ABI_VERSION; }

int32_t aver_decal_pool_set_capacity(int32_t capacity) {
    World& w = World::instance();
    const u32 n = static_cast<u32>(std::min<int32_t>(std::max<int32_t>(capacity, 0), static_cast<int32_t>(kMaxCapacity)));
    active().destroyAll(w);   // the old pool's entities are queued for destruction
    active() = DecalPool(n);
    return static_cast<int32_t>(n);
}

int32_t aver_decal_spawn(const AverDecalSpawnDesc* d) {
    if (!d) return 0;
    DecalSpawn sp;
    sp.position = Vec3{d->position[0], d->position[1], d->position[2]};
    const Vec3 n{d->normal[0], d->normal[1], d->normal[2]};
    if (n.sizeSquared() > 1e-8f) {
        const Vec3 up{d->up[0], d->up[1], d->up[2]};
        sp.rotation = decalRotationForSurface(n, d->rollRad, up.sizeSquared() > 1e-8f ? up : Vec3{0, 0, 1});
    } else {
        sp.rotation = Quat{d->rotation[0], d->rotation[1], d->rotation[2], d->rotation[3]}.normalized();
    }
    CDecal& c = sp.params;
    c.baseTexture = d->baseTexture; c.normalTexture = d->normalTexture; c.ormTexture = d->ormTexture;
    for (int i = 0; i < 3; ++i) { c.sizeCm[i] = d->sizeCm[i]; c.tint[i] = d->tint[i]; }
    c.transparency = d->transparency; c.normalStrength = d->normalStrength;
    c.roughness = d->roughness; c.metallic = d->metallic; c.edgeFade = d->edgeFade;
    c.angleFadeStartDeg = d->angleFadeStartDeg; c.angleFadeEndDeg = d->angleFadeEndDeg;
    c.fadeDistanceCm = d->fadeDistanceCm;
    for (int i = 0; i < 2; ++i) { c.uvScale[i] = d->uvScale[i]; c.uvOffset[i] = d->uvOffset[i]; }
    c.lifetimeSec = d->lifetimeSec; c.fadeOutSec = d->fadeOutSec;
    c.sortOrder = d->sortOrder;
    c.flags = d->flags & (kDecalNoColour | kDecalNoNormal | kDecalNoRoughness);
    return static_cast<int32_t>(active().spawn(World::instance(), sp));
}

int32_t aver_decal_release(int32_t entity) {
    return active().release(World::instance(), static_cast<Entity>(entity)) ? 1 : 0;
}

void aver_decal_clear(void) { active().clear(World::instance()); }

void aver_decal_tick(float dt) { active().tick(World::instance(), dt); }

int32_t aver_decal_active_count(void) { return static_cast<int32_t>(active().activeCount()); }

int32_t aver_decal_capacity(void) { return static_cast<int32_t>(active().capacity()); }

} // extern "C"
