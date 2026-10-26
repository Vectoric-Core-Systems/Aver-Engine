// Floating-origin relay behind framework_origin_abi.h: one provider the host installs, and the exact conversions about O.
// The WorldPos arithmetic is aver/world/WorldPos.hpp itself (header-only, Core-only: no link to Aver.World), so there is one
// implementation of the split and the C++ and C# sides cannot disagree about it. Single game thread; file-static state.

#include "aver/framework/framework_origin_abi.h"

#include "aver/world/WorldPos.hpp"

namespace {

aver_fw_origin_provider g_provider{};
bool g_hasProvider = false;

aver::world::OriginCm currentOrigin() {
    int64_t xy[2] = {0, 0};
    if (g_hasProvider && g_provider.get) g_provider.get(g_provider.user, xy);
    return aver::world::OriginCm{xy[0], xy[1]};
}

} // namespace

int32_t aver_fw_origin_abi_version(void) { return AVER_FW_ORIGIN_ABI_VERSION; }

int32_t aver_fw_set_origin_provider(const aver_fw_origin_provider* provider) {
    if (!provider) {
        g_provider = aver_fw_origin_provider{};
        g_hasProvider = false;
        return 1;
    }
    if (provider->size < static_cast<int32_t>(sizeof(aver_fw_origin_provider)) || !provider->get || !provider->generation ||
        !provider->enabled || !provider->request_shift)
        return 0;
    g_provider = *provider;
    g_hasProvider = true;
    return 1;
}

int32_t aver_fw_origin_get(int64_t* outXyCm) {
    int64_t xy[2] = {0, 0};
    const bool ok = g_hasProvider && g_provider.get(g_provider.user, xy) != 0;
    if (!ok) xy[0] = xy[1] = 0;
    if (outXyCm) { outXyCm[0] = xy[0]; outXyCm[1] = xy[1]; }
    return ok ? 1 : 0;
}

uint32_t aver_fw_origin_generation(void) { return g_hasProvider ? g_provider.generation(g_provider.user) : 0u; }

int32_t aver_fw_origin_enabled(void) { return g_hasProvider && g_provider.enabled(g_provider.user) != 0 ? 1 : 0; }

void aver_fw_origin_to_local(double ax, double ay, double az, float* outXyz) {
    if (!outXyz) return;
    const aver::world::OriginCm o = currentOrigin();
    outXyz[0] = static_cast<float>(ax - static_cast<double>(o.x));
    outXyz[1] = static_cast<float>(ay - static_cast<double>(o.y));
    outXyz[2] = static_cast<float>(az);
}

void aver_fw_origin_to_abs(float x, float y, float z, double* outXyz) {
    if (!outXyz) return;
    const aver::world::OriginCm o = currentOrigin();
    outXyz[0] = static_cast<double>(x) + static_cast<double>(o.x);
    outXyz[1] = static_cast<double>(y) + static_cast<double>(o.y);
    outXyz[2] = static_cast<double>(z);
}

void aver_fw_worldpos_from_abs(double ax, double ay, double az, int32_t* outChunk3, float* outLocal3) {
    const aver::world::WorldPos p = aver::world::worldPosOfAbs(ax, ay, az);
    if (outChunk3) { outChunk3[0] = p.chunk.x; outChunk3[1] = p.chunk.y; outChunk3[2] = p.chunk.z; }
    if (outLocal3) { outLocal3[0] = p.local.x; outLocal3[1] = p.local.y; outLocal3[2] = p.local.z; }
}

void aver_fw_worldpos_to_abs(const int32_t* chunk3, const float* local3, double* outXyz) {
    if (!outXyz) return;
    if (!chunk3 || !local3) { outXyz[0] = outXyz[1] = outXyz[2] = 0.0; return; }
    aver::world::WorldPos p;
    p.chunk = aver::world::ChunkCoord{chunk3[0], chunk3[1], chunk3[2]};
    p.local = aver::Vec3{local3[0], local3[1], local3[2]};
    const aver::world::Vec3d a = aver::world::absOf(p);
    outXyz[0] = a.x; outXyz[1] = a.y; outXyz[2] = a.z;
}

int32_t aver_fw_origin_request_shift(int32_t dxRegions, int32_t dyRegions) {
    if (!g_hasProvider || (dxRegions == 0 && dyRegions == 0)) return 0;
    return g_provider.request_shift(g_provider.user, dxRegions, dyRegions) != 0 ? 1 : 0;
}
