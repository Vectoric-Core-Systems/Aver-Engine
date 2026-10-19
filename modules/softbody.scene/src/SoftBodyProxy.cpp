// Proxy simulation cage: a mesh over the particle cap simulates a cooked, simplified copy of itself (.ocsoft "proxy"), and every
// render vertex of the authored mesh follows it through a ProxyBind (closest-point barycentric coordinates plus a normal
// offset, evaluated against whichever proxy triangle a piece owns). The runtime never simplifies: the proxy .ocmesh is made in
// the editor (sandbox/src/SoftProxyTool). See docs/SOFTBODY.md 2.7.
#include "aver/softbody/SoftBodySystem.hpp"

#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace aver::softbody {
namespace {

struct CellKey {
    i64 x, y, z;
    bool operator==(const CellKey& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct CellHash {
    usize operator()(const CellKey& k) const {
        return static_cast<usize>(static_cast<u64>(k.x) * 73856093ull ^ static_cast<u64>(k.y) * 19349663ull ^ static_cast<u64>(k.z) * 83492791ull);
    }
};

}  // namespace

bool SoftBodySystem::buildProxy(scene::World&, Entry& en, const SimMeshInput& authored, SimBuildSpec& spec, bool& notReady) {
    notReady = false;
    const SoftAsset& a = en.asset;
    const fmt::OcMeshData* pm = meshData(fnv1a64(a.proxy.mesh));
    if (!pm) {
        // The proxy may simply not have loaded yet; give it two seconds of ticks before falling back to the authored mesh.
        u32& waited = proxyWait_[en.entity];
        if (++waited < 120) { notReady = true; return false; }
        if (waited == 120) AVER_WARN("[SoftBody] entity {}: proxy mesh {} did not resolve; simulating the authored mesh", en.entity, a.proxy.mesh);
        return false;
    }
    proxyWait_.erase(en.entity);
    if (pm->positions.empty() || pm->indices.empty()) {
        AVER_WARN("[SoftBody] entity {}: proxy mesh {} has no triangles; simulating the authored mesh", en.entity, a.proxy.mesh);
        return false;
    }

    SimMeshInput pin;
    pin.positions = pm->positions.data();
    pin.uvs = pm->uvs.empty() ? nullptr : pm->uvs.data();
    pin.indices = pm->indices.data();
    pin.vertexCount = pm->vertexCount();
    pin.indexCount = static_cast<u32>(pm->indices.size());
    SimBuildSpec ps = spec;
    if (a.proxy.maxParticles) ps.maxParticles = a.proxy.maxParticles;
    if (!buildSimMesh(pin, ps, en.sim)) {
        AVER_WARN("[SoftBody] entity {}: proxy mesh {}: {}; simulating the authored mesh", en.entity, a.proxy.mesh, en.sim.error);
        return false;
    }

    ProxyBind bind;
    std::string why;
    if (!bindRenderToProxy(authored, en.sim.cage, en.sim.bindLocal, 0.0f, bind, &why)) {
        AVER_WARN("[SoftBody] entity {}: the mesh does not follow its proxy ({}); simulating the authored mesh", en.entity, why);
        en.sim = SimMesh{};
        return false;
    }
    bind.offsetScale = en.uniformScale;
    en.sim.binding = RenderBinding::forProxy(en.sim.cage, bind, std::vector<u32>(authored.indices, authored.indices + authored.indexCount));
    en.proxyBind = std::move(bind);
    en.proxyMode = true;
    en.proxyNormalsTick = 0xFFFFFFFFu;

    // The welded authored positions: the surface-point identity the caps need, since the dominant corner particle alone
    // would merge distinct points that share it.
    en.renderWeld.assign(authored.vertexCount, 0);
    std::unordered_map<CellKey, u32, CellHash> ids;
    const f32 cell = std::max(a.sim.weldCm, 0.01f);
    for (u32 v = 0; v < authored.vertexCount; ++v) {
        const f32* p = authored.positions + static_cast<usize>(v) * 3;
        const CellKey k{static_cast<i64>(std::llround(p[0] / cell)), static_cast<i64>(std::llround(p[1] / cell)), static_cast<i64>(std::llround(p[2] / cell))};
        const auto it = ids.find(k);
        if (it != ids.end()) en.renderWeld[v] = it->second;
        else { const u32 id = static_cast<u32>(ids.size()); ids.emplace(k, id); en.renderWeld[v] = id; }
    }

    // The saved-damage key covers both meshes: the cage that was replayed and the one drawn.
    const u64 drawKey = simMeshKey(authored, a.sim.weldCm);
    u64 h = en.sim.meshKey;
    h ^= drawKey + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    en.saveMeshKey = h;
    return true;
}

// Draw positions of the front vertices of a piece: each authored vertex evaluated against the proxy triangle chosen for it at the epoch.
void SoftBodySystem::proxyPositions(Entry& en, Piece& p) {
    const Cage& c = en.sim.cage;
    if (en.proxyNormalsTick != tickCount_) {
        smoothParticleNormals(c, en.proxyNormals);
        en.proxyNormalsTick = tickCount_;
    }
    for (usize i = 0; i < p.renderVerts.size() && i < p.sourceVertex.size(); ++i) {
        const u32 tri = i < p.drawEvalTri.size() ? p.drawEvalTri[i] : 0u;
        p.drawWorld[i] = proxyEvaluate(en.proxyBind, p.sourceVertex[i], tri, c, en.proxyNormals.data());
    }
}

}  // namespace aver::softbody
