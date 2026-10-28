// Runtime-mesh and MultiMesh store behind framework_rtmesh_abi.h (G4 R7). One file-static registry, single game thread; the hosts read it every frame
// (Runtime GameApp, the editor's Play path) and upload the meshes it reports changed. The geometry work is aver::fmt (MeshBuild.cpp), so it is tested without a device.

#include "aver/framework/framework_rtmesh_abi.h"

#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/MeshBuild.hpp"
#include "aver/formats/OcMesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using aver::fmt::OcMeshData;
using aver::fmt::RuntimeSurface;

constexpr int32_t kMaxInstances = 1 << 20;
constexpr size_t kMaxKey = 120;
constexpr const char* kPrefix = AVER_FW_RTMESH_PATH_PREFIX;

struct RtMesh {
    std::string key;
    std::vector<RuntimeSurface> surfaces;
    int multi = 0;                    // the MultiMesh that bakes into this mesh, 0 for a script-built one
    OcMeshData built;
    bool builtValid = false;          // `built` is current for `surfaces` (or the bake)
    bool buildOk = false;
    bool dirty = false;
    int32_t generation = 0;
    size_t vertices = 0;
    // view storage
    std::vector<int32_t> subStart, subCount;
    std::vector<std::string> subMatStr;
    std::vector<const char*> subMat;
};

struct Multi {
    std::string key;
    int32_t mesh = 0;
    std::string source;
    bool useColors = false, useCustom = false;
    std::vector<float> xf, col, custom;
    int32_t instances = 0;
    int32_t visible = -1;
    bool dirty = true;
    int32_t drawn = 0;
    bool warned = false;
    OcMeshData src;
    std::string srcLoaded;            // the source string `src` was read for
    bool srcValid = false;
};

struct Store {
    std::map<int32_t, RtMesh> meshes;
    std::unordered_map<std::string, int32_t> byKey;
    std::map<int32_t, Multi> multis;
    std::unordered_map<std::string, int32_t> multiByKey;
    std::vector<uint64_t> removed;
    int32_t nextMesh = 1, nextMulti = 1;
    int32_t maxMeshes = AVER_FW_RTMESH_DEFAULT_MAX_MESHES;
    int32_t maxVertices = AVER_FW_RTMESH_DEFAULT_MAX_VERTICES;
    int32_t lastPlayState = AVER_FW_PLAY_EDITOR;
    bool refusedLogged = false;
};

Store& store() {
    static Store s;
    return s;
}

bool goodKey(const char* key) { return key && *key && std::strlen(key) <= kMaxKey; }

uint64_t objectIdOf(const std::string& key) { return aver::fnv1a64(std::string(kPrefix) + key); }

RtMesh* meshOf(int32_t id) {
    auto it = store().meshes.find(id);
    return it == store().meshes.end() ? nullptr : &it->second;
}

Multi* multiOf(int32_t id) {
    auto it = store().multis.find(id);
    return it == store().multis.end() ? nullptr : &it->second;
}

void touch(RtMesh& m) {
    m.dirty = true;
    m.builtValid = false;
    ++m.generation;
    // A MultiMesh whose source is this mesh must bake again.
    const std::string path = std::string(kPrefix) + m.key;
    for (auto& [id, mm] : store().multis)
        if (mm.source == path) mm.dirty = true;
}

bool ensureBuilt(RtMesh& m) {
    if (m.builtValid) return m.buildOk;
    if (m.multi == 0) {
        std::string why;
        m.buildOk = !m.surfaces.empty() && aver::fmt::assembleSurfaces(m.surfaces, m.built, &why);
        if (!m.buildOk && !m.surfaces.empty()) AVER_WARN("[RuntimeMesh] '{}': {}", m.key, why);
        if (!m.buildOk) m.built = OcMeshData{};
    }
    m.builtValid = true;
    return m.buildOk;
}

void removeMesh(int32_t id) {
    Store& s = store();
    auto it = s.meshes.find(id);
    if (it == s.meshes.end()) return;
    s.removed.push_back(objectIdOf(it->second.key));
    s.byKey.erase(it->second.key);
    s.meshes.erase(it);
}

bool safeRelative(const std::string& p) {
    if (p.empty() || p[0] == '/' || p[0] == '\\' || p.find("..") != std::string::npos || p.find(':') != std::string::npos) return false;
    return true;
}

bool loadSource(Multi& mm) {
    if (mm.srcValid && mm.srcLoaded == mm.source) {
        if (mm.source.rfind(kPrefix, 0) != 0) return true;
    }
    mm.srcValid = false;
    mm.srcLoaded = mm.source;
    if (mm.source.empty()) return false;
    Store& s = store();
    if (mm.source.rfind(kPrefix, 0) == 0) {
        auto k = s.byKey.find(mm.source.substr(std::strlen(kPrefix)));
        RtMesh* rm = k == s.byKey.end() ? nullptr : meshOf(k->second);
        if (!rm || !ensureBuilt(*rm)) return false;
        mm.src = rm->built;
        mm.srcValid = true;
        return true;
    }
    if (!safeRelative(mm.source)) return false;
    const std::string dir = aver_fw_content_dir();
    if (dir.empty()) return false;
    std::string why;
    if (!aver::fmt::loadOcMesh(dir + "/" + mm.source, mm.src, &why)) {
        AVER_WARN("[MultiMesh] '{}': cannot read '{}': {}", mm.key, mm.source, why);
        return false;
    }
    mm.srcValid = true;
    return true;
}

void bake(int32_t id, Multi& mm) {
    mm.dirty = false;
    RtMesh* target = meshOf(mm.mesh);
    if (!target) return;
    target->surfaces.clear();
    target->built = OcMeshData{};
    target->buildOk = false;
    target->builtValid = true;
    target->dirty = true;
    ++target->generation;
    mm.drawn = 0;
    const int32_t want = mm.visible < 0 ? mm.instances : std::min(mm.visible, mm.instances);
    if (want <= 0 || !loadSource(mm)) return;
    aver::fmt::MeshInstanceSet set;
    set.transforms.assign(mm.xf.begin(), mm.xf.begin() + static_cast<long>(static_cast<size_t>(want) * 12));
    if (mm.useColors) set.colors.assign(mm.col.begin(), mm.col.begin() + static_cast<long>(static_cast<size_t>(want) * 4));
    if (mm.useCustom) set.custom.assign(mm.custom.begin(), mm.custom.begin() + static_cast<long>(static_cast<size_t>(want) * 4));
    uint32_t used = 0;
    std::string why;
    if (!aver::fmt::bakeInstances(mm.src, set, static_cast<uint32_t>(want), static_cast<uint32_t>(store().maxVertices), target->built, &used, &why)) {
        if (!mm.warned) { mm.warned = true; AVER_WARN("[MultiMesh] '{}': {}", mm.key, why); }
        target->built = OcMeshData{};
        return;
    }
    if (used < static_cast<uint32_t>(want) && !mm.warned) {
        mm.warned = true;
        AVER_WARN("[MultiMesh] '{}': {} of {} instances drawn (the mesh budget is {} vertices)", mm.key, used, want, store().maxVertices);
    }
    target->buildOk = true;
    target->vertices = target->built.vertexCount();
    mm.drawn = static_cast<int32_t>(used);
    (void)id;
}

}  // namespace

extern "C" {

int32_t aver_fw_rtmesh_abi_version(void) { return AVER_FW_RTMESH_ABI_VERSION; }

int32_t aver_fw_rtmesh_create(const char* key) {
    if (!goodKey(key)) return 0;
    Store& s = store();
    if (auto it = s.byKey.find(key); it != s.byKey.end()) return it->second;
    if (static_cast<int32_t>(s.meshes.size()) >= s.maxMeshes) {
        if (!s.refusedLogged) { s.refusedLogged = true; AVER_WARN("[RuntimeMesh] '{}' refused: {} meshes are live (the budget)", key, s.meshes.size()); }
        return 0;
    }
    const int32_t id = s.nextMesh++;
    RtMesh m;
    m.key = key;
    s.meshes.emplace(id, std::move(m));
    s.byKey[key] = id;
    return id;
}

int32_t aver_fw_rtmesh_destroy(int32_t id) {
    RtMesh* m = meshOf(id);
    if (!m || m->multi != 0) return 0;   // a MultiMesh's mesh goes with the MultiMesh
    removeMesh(id);
    return 1;
}

int32_t aver_fw_rtmesh_find(const char* key) {
    if (!key) return 0;
    auto it = store().byKey.find(key);
    return it == store().byKey.end() ? 0 : it->second;
}

int32_t aver_fw_rtmesh_clear(int32_t id) {
    RtMesh* m = meshOf(id);
    if (!m || m->multi != 0) return 0;
    m->surfaces.clear();
    m->vertices = 0;
    touch(*m);
    return 1;
}

int32_t aver_fw_rtmesh_add_surface(int32_t id, int32_t vertexCount, const float* pos, const float* nrm, const float* uv, const float* col, const float* uv1,
                                   const float* tan, int32_t indexCount, const uint32_t* idx, const char* material) {
    RtMesh* m = meshOf(id);
    if (!m || m->multi != 0 || vertexCount < 1 || !pos || indexCount < 0 || (indexCount > 0 && !idx)) return -1;
    if (m->vertices + static_cast<size_t>(vertexCount) > static_cast<size_t>(store().maxVertices)) {
        AVER_WARN("[RuntimeMesh] '{}': a surface of {} vertices would pass the {}-vertex mesh budget", m->key, vertexCount, store().maxVertices);
        return -1;
    }
    if (indexCount % 3 != 0) return -1;
    for (int32_t i = 0; i < indexCount; ++i)
        if (idx[i] >= static_cast<uint32_t>(vertexCount)) return -1;
    if (indexCount == 0 && vertexCount % 3 != 0) return -1;
    RuntimeSurface r;
    const size_t n = static_cast<size_t>(vertexCount);
    r.pos.assign(pos, pos + n * 3);
    if (nrm) r.nrm.assign(nrm, nrm + n * 3);
    if (uv) r.uv.assign(uv, uv + n * 2);
    if (col) r.col.assign(col, col + n * 4);
    if (uv1) r.uv1.assign(uv1, uv1 + n * 2);
    if (tan) r.tan.assign(tan, tan + n * 4);
    if (indexCount > 0) r.idx.assign(idx, idx + indexCount);
    if (material) r.material = material;
    m->surfaces.push_back(std::move(r));
    m->vertices += n;
    touch(*m);
    return static_cast<int32_t>(m->surfaces.size()) - 1;
}

int32_t aver_fw_rtmesh_surface_count(int32_t id) {
    const RtMesh* m = meshOf(id);
    return m ? static_cast<int32_t>(m->surfaces.size()) : 0;
}

int32_t aver_fw_rtmesh_info(int32_t id, int32_t* out4) {
    RtMesh* m = meshOf(id);
    if (!m || !out4) return 0;
    int32_t tris = 0;
    if (ensureBuilt(*m)) tris = static_cast<int32_t>(m->built.indices.size() / 3);
    out4[0] = static_cast<int32_t>(m->surfaces.size());
    out4[1] = static_cast<int32_t>(m->vertices);
    out4[2] = tris;
    out4[3] = m->generation;
    return 1;
}

uint64_t aver_fw_rtmesh_object_id(int32_t id) {
    const RtMesh* m = meshOf(id);
    return m ? objectIdOf(m->key) : 0;
}

int32_t aver_fw_rtmesh_key(int32_t id, char* out, int32_t cap) {
    const RtMesh* m = meshOf(id);
    if (!m) return 0;
    const int32_t len = static_cast<int32_t>(m->key.size());
    if (out && cap > 0) {
        const int32_t n = std::min(len, cap - 1);
        std::memcpy(out, m->key.data(), static_cast<size_t>(n));
        out[n] = 0;
    }
    return len;
}

// ---- MultiMesh ----

int32_t aver_fw_multimesh_create(const char* key) {
    if (!goodKey(key)) return 0;
    Store& s = store();
    if (auto it = s.multiByKey.find(key); it != s.multiByKey.end()) return it->second;
    const std::string meshKey = std::string("multimesh/") + key;
    const int32_t meshId = aver_fw_rtmesh_create(meshKey.c_str());
    if (meshId == 0) return 0;
    const int32_t id = s.nextMulti++;
    Multi mm;
    mm.key = key;
    mm.mesh = meshId;
    s.multis.emplace(id, std::move(mm));
    s.multiByKey[key] = id;
    meshOf(meshId)->multi = id;
    return id;
}

int32_t aver_fw_multimesh_destroy(int32_t id) {
    Multi* mm = multiOf(id);
    if (!mm) return 0;
    const int32_t meshId = mm->mesh;
    store().multiByKey.erase(mm->key);
    store().multis.erase(id);
    removeMesh(meshId);
    return 1;
}

int32_t aver_fw_multimesh_set_source(int32_t id, const char* meshPath) {
    Multi* mm = multiOf(id);
    if (!mm || !meshPath) return 0;
    mm->source = meshPath;
    mm->srcValid = false;
    mm->dirty = true;
    return 1;
}

int32_t aver_fw_multimesh_set_flags(int32_t id, int32_t useColors, int32_t useCustom) {
    Multi* mm = multiOf(id);
    if (!mm) return 0;
    mm->useColors = useColors != 0;
    mm->useCustom = useCustom != 0;
    mm->dirty = true;
    return 1;
}

int32_t aver_fw_multimesh_set_count(int32_t id, int32_t instanceCount) {
    Multi* mm = multiOf(id);
    if (!mm || instanceCount < 0 || instanceCount > kMaxInstances) return 0;
    const size_t n = static_cast<size_t>(instanceCount);
    const size_t old = static_cast<size_t>(mm->instances);
    mm->xf.resize(n * 12, 0.0f);
    mm->col.resize(n * 4, 1.0f);
    mm->custom.resize(n * 4, 0.0f);
    for (size_t i = old; i < n; ++i) {
        float* t = &mm->xf[i * 12];
        std::fill(t, t + 12, 0.0f);
        t[0] = t[4] = t[8] = 1.0f;
        std::fill(&mm->col[i * 4], &mm->col[i * 4] + 4, 1.0f);
    }
    mm->instances = instanceCount;
    mm->dirty = true;
    return 1;
}

int32_t aver_fw_multimesh_set_visible(int32_t id, int32_t visibleCount) {
    Multi* mm = multiOf(id);
    if (!mm) return 0;
    mm->visible = visibleCount < 0 ? -1 : visibleCount;
    mm->dirty = true;
    return 1;
}

int32_t aver_fw_multimesh_set_transform(int32_t id, int32_t index, const float* t12) {
    Multi* mm = multiOf(id);
    if (!mm || !t12 || index < 0 || index >= mm->instances) return 0;
    for (int i = 0; i < 12; ++i) if (!std::isfinite(t12[i])) return 0;
    std::memcpy(&mm->xf[static_cast<size_t>(index) * 12], t12, 12 * sizeof(float));
    mm->dirty = true;
    return 1;
}

int32_t aver_fw_multimesh_set_color(int32_t id, int32_t index, float r, float g, float b, float a) {
    Multi* mm = multiOf(id);
    if (!mm || index < 0 || index >= mm->instances) return 0;
    float* c = &mm->col[static_cast<size_t>(index) * 4];
    c[0] = r; c[1] = g; c[2] = b; c[3] = a;
    if (mm->useColors) mm->dirty = true;
    return 1;
}

int32_t aver_fw_multimesh_set_custom(int32_t id, int32_t index, float x, float y, float z, float w) {
    Multi* mm = multiOf(id);
    if (!mm || index < 0 || index >= mm->instances) return 0;
    float* c = &mm->custom[static_cast<size_t>(index) * 4];
    c[0] = x; c[1] = y; c[2] = z; c[3] = w;
    if (mm->useCustom) mm->dirty = true;
    return 1;
}

int32_t aver_fw_multimesh_set_buffer(int32_t id, int32_t count, const float* transforms12, const float* colors4, const float* custom4) {
    Multi* mm = multiOf(id);
    if (!mm || count < 0 || count > kMaxInstances || (count > 0 && !transforms12)) return 0;
    if (!aver_fw_multimesh_set_count(id, count)) return 0;
    const size_t n = static_cast<size_t>(count);
    if (n) std::memcpy(mm->xf.data(), transforms12, n * 12 * sizeof(float));
    if (colors4 && n) std::memcpy(mm->col.data(), colors4, n * 4 * sizeof(float));
    if (custom4 && n) std::memcpy(mm->custom.data(), custom4, n * 4 * sizeof(float));
    mm->dirty = true;
    return 1;
}

int32_t aver_fw_multimesh_info(int32_t id, int32_t* out4) {
    Multi* mm = multiOf(id);
    if (!mm || !out4) return 0;
    out4[0] = mm->instances;
    out4[1] = mm->visible < 0 ? mm->instances : std::min(mm->visible, mm->instances);
    out4[2] = mm->drawn;
    const RtMesh* t = meshOf(mm->mesh);
    out4[3] = t ? t->generation : 0;
    return 1;
}

int32_t aver_fw_multimesh_mesh(int32_t id) {
    const Multi* mm = multiOf(id);
    return mm ? mm->mesh : 0;
}

// ---- host ----

void aver_fw_rtmesh_configure(int32_t maxMeshes, int32_t maxVertices) {
    Store& s = store();
    if (maxMeshes >= 1) s.maxMeshes = std::min(maxMeshes, 4096);
    if (maxVertices >= 1) s.maxVertices = maxVertices;
}

int32_t aver_fw_rtmesh_pump(void) {
    Store& s = store();
    const int32_t state = aver_fw_play_state();
    if (state == AVER_FW_PLAY_EDITOR && s.lastPlayState != AVER_FW_PLAY_EDITOR) aver_fw_rtmesh_reset();
    s.lastPlayState = state;
    int32_t baked = 0;
    // A bake can mark another MultiMesh dirty (one instancing a baked mesh is not supported, but a source rebuild is): two passes bound the work.
    for (int pass = 0; pass < 2; ++pass)
        for (auto& [id, mm] : s.multis)
            if (mm.dirty) { bake(id, mm); ++baked; }
    return baked;
}

int32_t aver_fw_rtmesh_take_dirty(int32_t* outIds, int32_t max) {
    if (!outIds || max <= 0) return 0;
    int32_t n = 0;
    for (auto& [id, m] : store().meshes) {
        if (!m.dirty) continue;
        if (n >= max) break;
        m.dirty = false;
        outIds[n++] = id;
    }
    return n;
}

int32_t aver_fw_rtmesh_take_removed(uint64_t* outObjectIds, int32_t max) {
    Store& s = store();
    if (!outObjectIds || max <= 0) return 0;
    const int32_t n = std::min<int32_t>(max, static_cast<int32_t>(s.removed.size()));
    for (int32_t i = 0; i < n; ++i) outObjectIds[i] = s.removed[static_cast<size_t>(i)];
    s.removed.erase(s.removed.begin(), s.removed.begin() + n);
    return n;
}

int32_t aver_fw_rtmesh_view(int32_t id, AverFwRtMeshView* out) {
    RtMesh* m = meshOf(id);
    if (!m || !out) return 0;
    if (m->multi != 0) {
        Multi* mm = multiOf(m->multi);
        if (mm && mm->dirty) bake(m->multi, *mm);
    }
    if (!ensureBuilt(*m) || m->built.indices.empty()) return 0;
    const OcMeshData& d = m->built;
    m->subStart.clear(); m->subCount.clear(); m->subMatStr.clear(); m->subMat.clear();
    for (const aver::fmt::OcMeshSubmesh& sm : d.submeshes) {
        m->subStart.push_back(static_cast<int32_t>(sm.indexStart));
        m->subCount.push_back(static_cast<int32_t>(sm.indexCount));
        m->subMatStr.push_back(sm.materialSlot < d.materialSlots.size() ? d.materialSlots[sm.materialSlot] : std::string("M_Default"));
    }
    for (const std::string& str : m->subMatStr) m->subMat.push_back(str.c_str());
    out->vertexCount = static_cast<int32_t>(d.vertexCount());
    out->indexCount = static_cast<int32_t>(d.indices.size());
    out->submeshCount = static_cast<int32_t>(d.submeshes.size());
    out->pos = d.positions.data();
    out->nrm = d.normals.data();
    out->uv = d.uvs.data();
    out->col = d.hasColor() ? d.colors.data() : nullptr;
    out->uv1 = d.hasUv1() ? d.uvs1.data() : nullptr;
    out->tan = d.hasTangent() ? d.tangents.data() : nullptr;
    out->idx = d.indices.data();
    out->subStart = m->subStart.data();
    out->subCount = m->subCount.data();
    out->subMaterial = m->subMat.data();
    out->objectId = objectIdOf(m->key);
    return 1;
}

int32_t aver_fw_rtmesh_count(void) { return static_cast<int32_t>(store().meshes.size()); }

void aver_fw_rtmesh_reset(void) {
    Store& s = store();
    for (const auto& [id, m] : s.meshes) s.removed.push_back(objectIdOf(m.key));
    s.meshes.clear();
    s.byKey.clear();
    s.multis.clear();
    s.multiByKey.clear();
    s.refusedLogged = false;
}

}  // extern "C"
