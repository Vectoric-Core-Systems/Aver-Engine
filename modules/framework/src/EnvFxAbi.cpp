// Fog volume and reflection probe store behind framework_envfx_abi.h. File-static, single game thread; the hosts read it every frame.

#include "aver/framework/framework_envfx_abi.h"

#include "GodotEnvInternal.hpp"
#include "aver/core/FogVolume.hpp"
#include "aver/framework/framework_godotenv_abi.h"
#include "aver/core/Log.hpp"
#include "aver/core/ReflectionProbe.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Fog {
    int32_t id = 0;
    std::string key;
    aver::FogVolumeDesc desc;
    bool set = false;       // aver_fw_fogvol_set was called
    bool enabled = true;
    std::string shaderName; // G6 SH3: a registered Godot fog shader (framework_godotenv_abi.h), empty = none
};

struct Probe {
    int32_t id = 0;
    std::string key;
    aver::ReflectionProbeDesc desc;
    bool enabled = true;
};

struct Store {
    std::vector<Fog> fog;
    std::vector<Probe> probes;
    int32_t nextId = 1;
    int64_t fogRev = 0, probeRev = 0;
    int32_t lastPlayState = AVER_FW_PLAY_EDITOR;
    bool fogFullLogged = false, probeFullLogged = false;
};

Store& store() {
    static Store s;
    return s;
}

Fog* fogOf(int32_t id) {
    for (Fog& f : store().fog) if (f.id == id) return &f;
    return nullptr;
}

Probe* probeOf(int32_t id) {
    for (Probe& p : store().probes) if (p.id == id) return &p;
    return nullptr;
}

bool finite3(const float* v) { return v && std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

bool finite12(const float* v) {
    if (!v) return false;
    for (int i = 0; i < 12; ++i) if (!std::isfinite(v[i])) return false;
    return true;
}

} // namespace

extern "C" {

int32_t aver_fw_envfx_abi_version(void) { return AVER_FW_ENVFX_ABI_VERSION; }

// ---- fog volumes ----

int32_t aver_fw_fogvol_create(const char* key) {
    if (!key || !*key) return 0;
    Store& s = store();
    for (const Fog& f : s.fog) if (f.key == key) return f.id;
    if (s.fog.size() >= AVER_FW_FOGVOL_MAX) {
        if (!s.fogFullLogged) {
            s.fogFullLogged = true;
            AVER_WARN("[EnvFx] fog volume '{}' refused: {} volumes are already registered", key, AVER_FW_FOGVOL_MAX);
        }
        return 0;
    }
    Fog f;
    f.id = s.nextId++;
    f.key = key;
    s.fog.push_back(f);
    ++s.fogRev;
    return f.id;
}

int32_t aver_fw_fogvol_destroy(int32_t id) {
    Store& s = store();
    for (auto it = s.fog.begin(); it != s.fog.end(); ++it)
        if (it->id == id) { s.fog.erase(it); ++s.fogRev; return 1; }
    return 0;
}

int32_t aver_fw_fogvol_find(const char* key) {
    if (!key) return 0;
    for (const Fog& f : store().fog) if (f.key == key) return f.id;
    return 0;
}

int32_t aver_fw_fogvol_set(int32_t id, int32_t shape, const float* world12, const float* halfExtentCm3, float densityPerM, const float* albedo3,
                           const float* emission3, float heightFalloff, float edgeFade) {
    Fog* f = fogOf(id);
    if (!f || !finite12(world12) || !finite3(halfExtentCm3) || !std::isfinite(densityPerM) || !std::isfinite(heightFalloff) || !std::isfinite(edgeFade)) return 0;
    if (albedo3 && !finite3(albedo3)) return 0;
    if (emission3 && !finite3(emission3)) return 0;
    aver::FogVolumeDesc d;
    d.shape = static_cast<aver::FogVolumeShape>(std::clamp<int32_t>(shape, 0, 4));
    std::memcpy(d.world, world12, sizeof(d.world));
    for (int i = 0; i < 3; ++i) d.halfExtentCm[i] = std::max(halfExtentCm3[i], 0.0f);
    d.densityPerM = densityPerM;
    for (int i = 0; i < 3; ++i) {
        d.albedo[i] = albedo3 ? albedo3[i] : 1.0f;
        d.emission[i] = emission3 ? emission3[i] : 0.0f;
    }
    d.heightFalloff = heightFalloff;
    d.edgeFade = edgeFade;
    // A matrix that cannot be inverted would never draw; say so now rather than at the host.
    aver::f32 probeBlock[aver::kFogVolumeVecs][4];
    if (!aver::fogv::pack(d, probeBlock)) return 0;
    f->desc = d;
    f->set = true;
    ++store().fogRev;
    return 1;
}

int32_t aver_fw_fogvol_set_shader(int32_t id, const char* name) {
    Fog* f = fogOf(id);
    if (!f) return 0;
    const std::string want = name ? name : "";
    if (f->shaderName != want) { f->shaderName = want; ++store().fogRev; }
    return 1;
}

int32_t aver_fw_fogvol_set_enabled(int32_t id, int32_t on) {
    Fog* f = fogOf(id);
    if (!f) return 0;
    if (f->enabled != (on != 0)) { f->enabled = on != 0; ++store().fogRev; }
    return 1;
}

int32_t aver_fw_fogvol_count(void) { return static_cast<int32_t>(store().fog.size()); }

float aver_fw_fogvol_density_at(float x, float y, float z) {
    const float p[3] = {x, y, z};
    float sum = 0.0f;
    for (const Fog& f : store().fog) {
        if (!f.set || !f.enabled) continue;
        float pk[aver::kFogVolumeVecs][4];
        if (aver::fogv::pack(f.desc, pk)) sum += aver::fogv::density(pk, p);
    }
    return sum;
}

int64_t aver_fw_fogvol_revision(void) { return store().fogRev; }

int32_t aver_fw_fogvol_pack(float x, float y, float z, float* outFloats, int32_t capFloats) {
    const int32_t need = static_cast<int32_t>(sizeof(aver::FogVolumeBlock) / sizeof(float));
    if (!outFloats || capFloats < need) return 0;
    // Nearest the camera first: when more than 16 exist the far ones are the ones dropped.
    struct Cand { const Fog* f; float d2; };
    std::vector<Cand> c;
    for (const Fog& f : store().fog) {
        if (!f.set || !f.enabled) continue;
        const float dx = f.desc.world[9] - x, dy = f.desc.world[10] - y, dz = f.desc.world[11] - z;
        c.push_back({&f, f.desc.shape == aver::FogVolumeShape::World ? -1.0f : dx * dx + dy * dy + dz * dz});
    }
    std::stable_sort(c.begin(), c.end(), [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
    std::vector<aver::FogVolumeDesc> descs;
    for (const Cand& k : c) {
        descs.push_back(k.f->desc);
        // G6 SH3: the shader's place in the renderer's fog-shader table (0 = none, or not registered).
        descs.back().shader = k.f->shaderName.empty() ? 0u : aver_fw_genv_fog_index(k.f->shaderName);
    }
    aver::FogVolumeBlock block;
    const uint32_t n = aver::fogv::packAll(descs.data(), static_cast<uint32_t>(descs.size()), block);
    std::memcpy(outFloats, &block, sizeof(block));
    return static_cast<int32_t>(n);
}

// ---- reflection probes ----

int32_t aver_fw_probe_create(const char* key) {
    if (!key || !*key) return 0;
    Store& s = store();
    for (const Probe& p : s.probes) if (p.key == key) return p.id;
    if (s.probes.size() >= AVER_FW_PROBE_MAX) {
        if (!s.probeFullLogged) {
            s.probeFullLogged = true;
            AVER_WARN("[EnvFx] reflection probe '{}' refused: {} probes are already registered", key, AVER_FW_PROBE_MAX);
        }
        return 0;
    }
    Probe p;
    p.id = s.nextId++;
    p.key = key;
    s.probes.push_back(p);
    ++s.probeRev;
    return p.id;
}

int32_t aver_fw_probe_destroy(int32_t id) {
    Store& s = store();
    for (auto it = s.probes.begin(); it != s.probes.end(); ++it)
        if (it->id == id) { s.probes.erase(it); ++s.probeRev; return 1; }
    return 0;
}

int32_t aver_fw_probe_find(const char* key) {
    if (!key) return 0;
    for (const Probe& p : store().probes) if (p.key == key) return p.id;
    return 0;
}

int32_t aver_fw_probe_set_box(int32_t id, const float* world12, const float* halfExtentCm3, const float* originOffsetCm3) {
    Probe* p = probeOf(id);
    if (!p || !finite12(world12) || !finite3(halfExtentCm3)) return 0;
    if (originOffsetCm3 && !finite3(originOffsetCm3)) return 0;
    std::memcpy(p->desc.world, world12, sizeof(p->desc.world));
    for (int i = 0; i < 3; ++i) {
        p->desc.halfExtentCm[i] = std::max(halfExtentCm3[i], 0.0f);
        p->desc.originOffsetCm[i] = originOffsetCm3 ? originOffsetCm3[i] : 0.0f;
    }
    ++store().probeRev;
    return 1;
}

int32_t aver_fw_probe_set_look(int32_t id, float intensity, float blendDistanceCm, float maxDistanceCm, int32_t boxProjection, int32_t interior,
                               int32_t updateMode, int32_t resolution) {
    Probe* p = probeOf(id);
    if (!p || !std::isfinite(intensity) || !std::isfinite(blendDistanceCm) || !std::isfinite(maxDistanceCm)) return 0;
    p->desc.intensity = std::max(intensity, 0.0f);
    p->desc.blendDistanceCm = std::max(blendDistanceCm, 0.0f);
    p->desc.maxDistanceCm = std::max(maxDistanceCm, 0.0f);
    p->desc.boxProjection = boxProjection != 0;
    p->desc.interior = interior != 0;
    p->desc.update = updateMode == 1 ? aver::ProbeUpdate::Always : aver::ProbeUpdate::Once;
    p->desc.resolution = static_cast<uint32_t>(std::clamp<int32_t>(resolution, 32, 2048));
    ++store().probeRev;
    return 1;
}

int32_t aver_fw_probe_set_ambient(int32_t id, int32_t mode, const float* colour3, float energy) {
    Probe* p = probeOf(id);
    if (!p || !std::isfinite(energy) || (colour3 && !finite3(colour3))) return 0;
    p->desc.ambient = static_cast<aver::ProbeAmbient>(std::clamp<int32_t>(mode, 0, 2));
    for (int i = 0; i < 3; ++i) p->desc.ambientColour[i] = colour3 ? colour3[i] : 0.0f;
    p->desc.ambientEnergy = std::max(energy, 0.0f);
    ++store().probeRev;
    return 1;
}

int32_t aver_fw_probe_set_masks(int32_t id, uint32_t cullMask, uint32_t reflectionMask) {
    Probe* p = probeOf(id);
    if (!p) return 0;
    p->desc.cullMask = cullMask;
    p->desc.reflectionMask = reflectionMask;
    ++store().probeRev;
    return 1;
}

int32_t aver_fw_probe_set_enabled(int32_t id, int32_t on) {
    Probe* p = probeOf(id);
    if (!p) return 0;
    if (p->enabled != (on != 0)) { p->enabled = on != 0; ++store().probeRev; }
    return 1;
}

int32_t aver_fw_probe_count(void) { return static_cast<int32_t>(store().probes.size()); }

int32_t aver_fw_probe_select(float x, float y, float z, float cx, float cy, float cz, int32_t* outIds, float* outWeights, float* outRest) {
    std::vector<aver::ReflectionProbeDesc> descs;
    std::vector<int32_t> ids;
    for (const Probe& p : store().probes) {
        if (!p.enabled) continue;
        descs.push_back(p.desc);
        ids.push_back(p.id);
    }
    const aver::probe::Selection sel = aver::probe::select(descs.data(), static_cast<uint32_t>(descs.size()), aver::Vec3{x, y, z}, aver::Vec3{cx, cy, cz});
    if (outRest) *outRest = sel.rest;
    for (uint32_t i = 0; i < sel.count; ++i) {
        if (outIds) outIds[i] = ids[sel.pick[i].index];
        if (outWeights) outWeights[i] = sel.pick[i].weight;
    }
    return static_cast<int32_t>(sel.count);
}

int32_t aver_fw_probe_project(int32_t id, float x, float y, float z, float dx, float dy, float dz, float* out3) {
    const aver::Vec3 d = aver::Vec3{dx, dy, dz}.getSafeNormal();
    aver::Vec3 r = d;
    const Probe* p = probeOf(id);
    if (p) r = aver::probe::boxProject(p->desc, aver::probe::frameOf(p->desc), aver::Vec3{x, y, z}, d);
    if (out3) { out3[0] = r.x; out3[1] = r.y; out3[2] = r.z; }
    return p ? 1 : 0;
}

int64_t aver_fw_probe_revision(void) { return store().probeRev; }

void aver_fw_envfx_reset(void) {
    Store& s = store();
    if (!s.fog.empty()) { s.fog.clear(); ++s.fogRev; }
    if (!s.probes.empty()) { s.probes.clear(); ++s.probeRev; }
    s.fogFullLogged = s.probeFullLogged = false;
    aver_fw_genv_reset();   // G6 SH3: the converted shaders a level registered go with it
}

void aver_fw_envfx_tick(void) {
    Store& s = store();
    // The editor keeps this DLL loaded across Play presses: what a level placed is dropped when the play state falls back to Editor.
    const int32_t state = aver_fw_play_state();
    if (state == AVER_FW_PLAY_EDITOR && s.lastPlayState != AVER_FW_PLAY_EDITOR) aver_fw_envfx_reset();
    s.lastPlayState = state;
}

} // extern "C"
