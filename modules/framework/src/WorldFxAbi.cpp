// World effects store behind framework_worldfx_abi.h: sun / fog / sky-light overrides and named post layers. One
// file-static store, single game thread; the hosts read it every frame (Runtime GameApp, the editor's Play path).

#include "aver/framework/framework_worldfx_abi.h"

#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcWorld.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <vector>

namespace {

constexpr int kEnvKeys = 12;   // index 0 unused: the keys are 1..11

struct EnvSlot {
    bool set = false;
    float v[3] = {0, 0, 0};
};

// Blends toward `target` from `from` over `total` seconds, linearly.
struct Blend {
    float cur = 0, from = 0, target = 0, left = 0, total = 0;

    void begin(float to, float seconds) {
        from = cur;
        target = to;
        if (seconds > 0.0f) { left = total = seconds; }
        else { cur = to; left = total = 0.0f; }
    }
    void tick(float dt) {
        if (left <= 0.0f) return;
        left = std::max(0.0f, left - dt);
        cur = left > 0.0f ? from + (target - from) * (1.0f - left / total) : target;
    }
};

struct PostValue {
    int64_t keyHash = 0;
    float v[3] = {0, 0, 0};
};

struct PostLayer {
    int64_t id = 0;
    Blend weight;
    std::vector<PostValue> values;
};

struct VolumeScale {
    int64_t nameHash = 0;
    Blend scale;
};

struct Store {
    EnvSlot env[kEnvKeys];
    std::vector<PostLayer> layers;
    std::vector<VolumeScale> scales;
    std::unordered_set<int64_t> badKeys;   // unknown post keys already logged
    bool fullLogged = false;
    int32_t lastPlayState = AVER_FW_PLAY_EDITOR;
};

Store& store() {
    static Store s;
    return s;
}

bool finite3(float a, float b, float c) { return std::isfinite(a) && std::isfinite(b) && std::isfinite(c); }

// The POSTVOLUME key table hashed once (lower-case key, fnv1a64): a key hash is valid exactly when it is in here.
bool knownPostKey(int64_t hash) {
    static const std::vector<int64_t> hashes = [] {
        std::vector<int64_t> h;
        for (const aver::fmt::OcPostKeyInfo& k : aver::fmt::kOcPostKeys)
            h.push_back(static_cast<int64_t>(aver::fnv1a64(std::string_view(k.key))));
        return h;
    }();
    return std::find(hashes.begin(), hashes.end(), hash) != hashes.end();
}

PostLayer* findLayer(int64_t id) {
    for (PostLayer& l : store().layers)
        if (l.id == id) return &l;
    return nullptr;
}

VolumeScale* findScale(int64_t hash) {
    for (VolumeScale& s : store().scales)
        if (s.nameHash == hash) return &s;
    return nullptr;
}

void logFullOnce(const char* what) {
    Store& s = store();
    if (s.fullLogged) return;
    s.fullLogged = true;
    AVER_WARN("[WorldFx] too many {}; the request was refused", what);
}

} // namespace

int32_t aver_fw_worldfx_abi_version(void) { return AVER_FW_WORLDFX_ABI_VERSION; }

// ---- environment ----

int32_t aver_fw_env_set(int32_t key, float v0, float v1, float v2) {
    if (key < AVER_FW_ENV_SUN_DIR || key > AVER_FW_ENV_SKY_LIGHT) return 0;
    if (!finite3(v0, v1, v2)) return 0;
    float v[3] = {v0, v1, v2};
    switch (key) {
    case AVER_FW_ENV_SUN_DIR: {
        const float len = std::sqrt(v0 * v0 + v1 * v1 + v2 * v2);
        if (!(len > 1e-6f)) return 0;
        for (float& c : v) c /= len;
        break;
    }
    case AVER_FW_ENV_SUN_COLOR:
    case AVER_FW_ENV_FOG_COLOR:
        for (float& c : v) c = std::max(c, 0.0f);
        break;
    case AVER_FW_ENV_SUN_TEMP:
        v[0] = std::clamp(v0, 0.0f, 40000.0f); v[1] = v[2] = 0.0f;
        break;
    case AVER_FW_ENV_FOG_MAXOPAC:
        v[0] = std::clamp(v0, 0.0f, 1.0f); v[1] = v[2] = 0.0f;
        break;
    case AVER_FW_ENV_FOG_HEIGHT:
        v[1] = v[2] = 0.0f;
        break;
    default:   // lux, density, falloff, start, sky light: one non-negative value
        v[0] = std::max(v0, 0.0f); v[1] = v[2] = 0.0f;
        break;
    }
    EnvSlot& s = store().env[key];
    s.set = true;
    for (int i = 0; i < 3; ++i) s.v[i] = v[i];
    return 1;
}

int32_t aver_fw_env_clear(int32_t key) {
    if (key == 0) {
        for (EnvSlot& s : store().env) s = EnvSlot{};
        return 1;
    }
    if (key < AVER_FW_ENV_SUN_DIR || key > AVER_FW_ENV_SKY_LIGHT) return 0;
    store().env[key] = EnvSlot{};
    return 1;
}

int32_t aver_fw_env_get(int32_t key, float* outV3) {
    if (key < AVER_FW_ENV_SUN_DIR || key > AVER_FW_ENV_SKY_LIGHT) return 0;
    const EnvSlot& s = store().env[key];
    if (!s.set) return 0;
    if (outV3) for (int i = 0; i < 3; ++i) outV3[i] = s.v[i];
    return 1;
}

// ---- post layers ----

int32_t aver_fw_post_set(int64_t layer, int64_t keyHash, float v0, float v1, float v2) {
    if (layer == 0 || !finite3(v0, v1, v2)) return 0;
    Store& s = store();
    if (!knownPostKey(keyHash)) {
        if (s.badKeys.insert(keyHash).second)
            AVER_WARN("[WorldFx] a post value names a key the POSTVOLUME table does not know (hash {:#x}); refused", static_cast<uint64_t>(keyHash));
        return 0;
    }
    PostLayer* l = findLayer(layer);
    if (!l) {
        if (static_cast<int>(s.layers.size()) >= AVER_FW_POST_MAX_LAYERS) { logFullOnce("post layers"); return 0; }
        s.layers.emplace_back();
        l = &s.layers.back();
        l->id = layer;
        l->weight.begin(1.0f, AVER_FW_POST_AUTO_BLEND);   // engages by itself until a script gives it a weight
    }
    for (PostValue& v : l->values)
        if (v.keyHash == keyHash) { v.v[0] = v0; v.v[1] = v1; v.v[2] = v2; return 1; }
    PostValue nv;
    nv.keyHash = keyHash;
    nv.v[0] = v0; nv.v[1] = v1; nv.v[2] = v2;
    l->values.push_back(nv);
    return 1;
}

int32_t aver_fw_post_set_weight(int64_t layer, float weight, float blendSeconds) {
    if (layer == 0 || !std::isfinite(weight) || !std::isfinite(blendSeconds)) return 0;
    Store& s = store();
    PostLayer* l = findLayer(layer);
    if (!l) {
        if (static_cast<int>(s.layers.size()) >= AVER_FW_POST_MAX_LAYERS) { logFullOnce("post layers"); return 0; }
        s.layers.emplace_back();
        l = &s.layers.back();
        l->id = layer;
    }
    l->weight.begin(std::clamp(weight, 0.0f, 1.0f), blendSeconds);
    return 1;
}

int32_t aver_fw_post_clear(int64_t layer) {
    Store& s = store();
    if (layer == 0) { s.layers.clear(); return 1; }
    const auto it = std::find_if(s.layers.begin(), s.layers.end(), [&](const PostLayer& l) { return l.id == layer; });
    if (it == s.layers.end()) return 0;
    s.layers.erase(it);
    return 1;
}

int32_t aver_fw_post_volume_weight(int64_t volumeNameHash, float weight, float blendSeconds) {
    if (volumeNameHash == 0 || !std::isfinite(weight) || !std::isfinite(blendSeconds)) return 0;
    Store& s = store();
    if (weight < 0.0f) {
        const auto it = std::find_if(s.scales.begin(), s.scales.end(), [&](const VolumeScale& v) { return v.nameHash == volumeNameHash; });
        if (it == s.scales.end()) return 0;
        s.scales.erase(it);
        return 1;
    }
    VolumeScale* v = findScale(volumeNameHash);
    if (!v) {
        if (static_cast<int>(s.scales.size()) >= AVER_FW_POST_MAX_SCALES) { logFullOnce("post volume scales"); return 0; }
        s.scales.emplace_back();
        v = &s.scales.back();
        v->nameHash = volumeNameHash;
        v->scale.cur = 1.0f;   // an authored volume starts at its full weight
    }
    v->scale.begin(std::min(weight, 1.0f), blendSeconds);
    return 1;
}

// ---- host side ----

void aver_fw_worldfx_tick(float dt) {
    Store& s = store();
    // A play session ending hands the world back: the editor keeps this DLL loaded across Play presses, so every
    // override, layer and scale a script left is dropped when the state falls back to Editor.
    const int32_t state = aver_fw_play_state();
    if (state == AVER_FW_PLAY_EDITOR && s.lastPlayState != AVER_FW_PLAY_EDITOR) {
        for (EnvSlot& e : s.env) e = EnvSlot{};
        s.layers.clear();
        s.scales.clear();
    }
    s.lastPlayState = state;
    if (!(dt > 0.0f) || !std::isfinite(dt)) return;
    for (PostLayer& l : s.layers) l.weight.tick(dt);
    for (VolumeScale& v : s.scales) v.scale.tick(dt);
}

int32_t aver_fw_post_layer_count(void) { return static_cast<int32_t>(store().layers.size()); }

int32_t aver_fw_post_layer_info(int32_t index, int64_t* outLayer, float* outWeight, int32_t* outValueCount) {
    const Store& s = store();
    if (index < 0 || index >= static_cast<int32_t>(s.layers.size())) return 0;
    const PostLayer& l = s.layers[static_cast<size_t>(index)];
    if (outLayer) *outLayer = l.id;
    if (outWeight) *outWeight = l.weight.cur;
    if (outValueCount) *outValueCount = static_cast<int32_t>(l.values.size());
    return 1;
}

int32_t aver_fw_post_layer_value(int32_t index, int32_t valueIndex, int64_t* outKeyHash, float* outV3) {
    const Store& s = store();
    if (index < 0 || index >= static_cast<int32_t>(s.layers.size())) return 0;
    const PostLayer& l = s.layers[static_cast<size_t>(index)];
    if (valueIndex < 0 || valueIndex >= static_cast<int32_t>(l.values.size())) return 0;
    const PostValue& v = l.values[static_cast<size_t>(valueIndex)];
    if (outKeyHash) *outKeyHash = v.keyHash;
    if (outV3) for (int i = 0; i < 3; ++i) outV3[i] = v.v[i];
    return 1;
}

int32_t aver_fw_post_volume_scale(int64_t volumeNameHash, float* outWeight) {
    const Store& s = store();
    for (const VolumeScale& v : s.scales)
        if (v.nameHash == volumeNameHash) {
            if (outWeight) *outWeight = v.scale.cur;
            return 1;
        }
    return 0;
}
