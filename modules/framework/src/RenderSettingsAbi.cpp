// Render settings and statistics store behind framework_render_abi.h (P17, P18). One file-static store, single game thread; the hosts read it once per
// frame (Runtime GameApp, the editor's Play path) before they build the scene. Pure CPU, so it is tested without a device.

#include "aver/framework/framework_render_abi.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

struct KeyRange { float lo, hi; };

// Indexed by key; entry 0 is unused. Mirrors the AVER_FW_RENDER_* comments.
constexpr KeyRange kRange[AVER_FW_RENDER_KEY_COUNT] = {
    {0.0f, 0.0f},        // 0
    {0.25f, 2.0f},       // SCALE
    {0.0f, 1.0f},        // SHARPNESS
    {0.0f, 1.0f},        // TAA
    {1.0f, 8.0f},        // MSAA
    {0.0f, 64.0f},       // LOD_THRESHOLD
    {0.0f, 1.0f},        // OCCLUSION
    {1.0f, 4.0f},        // SHADOW_CASCADES
    {0.0f, 1.0f},        // SHADOW_SPLIT
    {0.0f, 1.0e7f},      // SHADOW_DISTANCE
    {0.0f, 4.0f},        // SHADOW_FILTER
    {-0.95f, 0.95f},     // VOLFOG_ANISO
    {100.0f, 500000.0f}, // VOLFOG_LENGTH
    {0.0f, 0.98f},       // VOLFOG_TEMPORAL
    {0.0f, 16.0f},       // VOLFOG_INTENSITY
};

struct Store {
    bool set[AVER_FW_RENDER_KEY_COUNT] = {};
    float value[AVER_FW_RENDER_KEY_COUNT] = {};
    int32_t revision = 0;
    int32_t lastPlayState = AVER_FW_PLAY_EDITOR;
    bool statsWanted = false;
    bool statsPublished = false;
    int32_t stats[AVER_FW_RENDER_STAT_COUNT] = {};
};

Store& store() {
    static Store s;
    return s;
}

bool validKey(int32_t key) { return key >= 1 && key < AVER_FW_RENDER_KEY_COUNT; }

}  // namespace

int32_t aver_fw_render_abi_version(void) { return AVER_FW_RENDER_ABI_VERSION; }

int32_t aver_fw_render_set(int32_t key, float value) {
    if (!validKey(key) || !std::isfinite(value)) return 0;
    Store& s = store();
    float v = std::clamp(value, kRange[key].lo, kRange[key].hi);
    if (key == AVER_FW_RENDER_MSAA) v = v >= 8.0f ? 8.0f : v >= 4.0f ? 4.0f : v >= 2.0f ? 2.0f : 1.0f;
    if (key == AVER_FW_RENDER_SHADOW_CASCADES) v = std::floor(v + 0.5f);
    if (key == AVER_FW_RENDER_TAA || key == AVER_FW_RENDER_OCCLUSION) v = v >= 0.5f ? 1.0f : 0.0f;
    if (!s.set[key] || s.value[key] != v) ++s.revision;
    s.set[key] = true;
    s.value[key] = v;
    return 1;
}

int32_t aver_fw_render_clear(int32_t key) {
    Store& s = store();
    if (key == 0) {
        bool any = false;
        for (int k = 1; k < AVER_FW_RENDER_KEY_COUNT; ++k) { any = any || s.set[k]; s.set[k] = false; s.value[k] = 0.0f; }
        if (any) ++s.revision;
        return 1;
    }
    if (!validKey(key)) return 0;
    if (s.set[key]) ++s.revision;
    s.set[key] = false;
    s.value[key] = 0.0f;
    return 1;
}

int32_t aver_fw_render_get(int32_t key, float* out) {
    if (!validKey(key)) return 0;
    const Store& s = store();
    if (!s.set[key]) return 0;
    if (out) *out = s.value[key];
    return 1;
}

int32_t aver_fw_render_revision(void) {
    // A play session ending hands the project's values back, as framework_worldfx_abi.h does: the editor keeps this DLL loaded across Play presses.
    Store& s = store();
    const int32_t state = aver_fw_play_state();
    if (state == AVER_FW_PLAY_EDITOR && s.lastPlayState != AVER_FW_PLAY_EDITOR) aver_fw_render_reset();
    s.lastPlayState = state;
    return s.revision;
}

int32_t aver_fw_render_stats(int32_t* out6) {
    Store& s = store();
    s.statsWanted = true;
    if (out6) {
        if (s.statsPublished) std::memcpy(out6, s.stats, sizeof s.stats);
        else std::memset(out6, 0, sizeof s.stats);
    }
    return s.statsPublished ? 1 : 0;
}

int32_t aver_fw_render_stats_wanted(void) { return store().statsWanted ? 1 : 0; }

void aver_fw_render_publish_stats(const int32_t* in6) {
    if (!in6) return;
    Store& s = store();
    std::memcpy(s.stats, in6, sizeof s.stats);
    s.statsPublished = true;
}

void aver_fw_render_reset(void) {
    Store& s = store();
    bool any = false;
    for (int k = 1; k < AVER_FW_RENDER_KEY_COUNT; ++k) { any = any || s.set[k]; s.set[k] = false; s.value[k] = 0.0f; }
    if (any) ++s.revision;
    s.statsWanted = false;
    s.statsPublished = false;
    std::memset(s.stats, 0, sizeof s.stats);
}
