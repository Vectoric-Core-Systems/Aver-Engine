// The framework relay's provider for strand hair (framework_hair_abi.h): every op forwards to the HairSystem. The ops are between-step calls and
// apply at once, in arrival order, which is what a replay needs. Field writes (the scales, the flags, the asset) do not come through here; they
// are the generic scene ABI's, on the CHair component.
#include "aver/hair/HairSystem.hpp"

#include <cmath>

namespace aver::hair {
namespace {

// AVER_FW_HAIR_OP_* and AVER_FW_HAIR_TI_* (framework_hair_abi.h); tests/abi pins them against the C# mirror.
constexpr i32 kOpTeleport = 1, kOpAddCollider = 2, kOpClearColliders = 3, kOpState = 4;
constexpr i32 kTiState = 0, kTiLod = 1, kTiRenderStrands = 2, kTiSimStrands = 3, kTiColliders = 4, kTiBuilt = 5, kTiCount = 6;

bool allFinite(const float* in, i32 n) {
    for (i32 i = 0; i < n; ++i)
        if (!std::isfinite(in[i])) return false;
    return true;
}

} // namespace

i32 hairProvider(i32 op, i32 entity, const float* in, i32 inCount, const char* text, float* /*outF*/, i32* outI, i32 cap, void* user) {
    if (!user) return 0;
    HairSystem& sys = *static_cast<HairSystem*>(user);
    scene::World& world = scene::World::instance();
    if (entity <= 0) return 0;
    const scene::Entity e = static_cast<scene::Entity>(entity);
    if (!world.valid(e)) return 0;
    if (inCount > 0 && (!in || !allFinite(in, inCount))) return 0;
    switch (op) {
        case kOpTeleport:
            return sys.teleport(e) ? 1 : 0;
        case kOpAddCollider: {
            // shape sx sy sz ox oy oz; text = the bone. A sphere reads sx (radius), a capsule sx (radius) and sy (half height), a box sx sy sz.
            if (inCount < 7 || !text || !*text) return 0;
            const long shape = std::lround(in[0]);
            if (shape < 0 || shape > 2) return 0;
            fmt::OcHairProxy p;
            p.name = "script";
            p.bone = text;
            p.shape = static_cast<fmt::OcHairProxyShape>(shape);
            for (int i = 0; i < 3; ++i) {
                p.sizeCm[i] = in[1 + i];
                p.offsetCm[i] = in[4 + i];
            }
            if (!sys.addCollider(world, e, p)) return 0;
            if (outI && cap > 0) outI[0] = static_cast<i32>(sys.colliderCount(e));
            return 1;
        }
        case kOpClearColliders:
            return sys.clearColliders(e) ? 1 : 0;
        case kOpState: {
            HairSystem::StateOut st;
            if (!sys.stateOf(world, e, st)) return 0;
            if (outI && cap >= kTiCount) {
                outI[kTiState] = static_cast<i32>(st.outState & 0xFFu);
                outI[kTiLod] = static_cast<i32>(st.outState >> kHairLodShift);
                outI[kTiRenderStrands] = static_cast<i32>(st.renderStrands);
                outI[kTiSimStrands] = static_cast<i32>(st.simStrands);
                outI[kTiColliders] = static_cast<i32>(st.colliders);
                outI[kTiBuilt] = st.built ? 1 : 0;
            }
            return 1;
        }
        default:
            return 0;
    }
}

} // namespace aver::hair
