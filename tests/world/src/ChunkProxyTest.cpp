// buildChunkProxy: an HLOD proxy for a generated chunk from its payload, off any scene -- chunk-local vertices, parent composition,
// layer exclusion, the time/item cap, determinism, and the budget handed to the simplifier. CPU only.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/world/ChunkProxy.hpp"
#include "aver/world/ChunkPayloadExtras.hpp"

#include <string>
#include <vector>

using namespace aver;
using namespace aver::world;

static int g_checks = 0, g_failures = 0;
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static fmt::OcMeshData cube() {
    fmt::OcMeshData m;
    const f32 c[8][3] = {{0, 0, 0}, {100, 0, 0}, {100, 100, 0}, {0, 100, 0}, {0, 0, 100}, {100, 0, 100}, {100, 100, 100}, {0, 100, 100}};
    for (const auto& v : c) {
        m.positions.insert(m.positions.end(), {v[0], v[1], v[2]});
        m.normals.insert(m.normals.end(), {0, 0, 1});
        m.uvs.insert(m.uvs.end(), {0, 0});
    }
    m.indices = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4, 1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7};
    m.materialSlots = {"stone"};
    fmt::OcMeshSubmesh sm; sm.name = "stone"; sm.indexCount = 36; sm.vertexCount = 8;
    m.submeshes = {sm};
    m.computeBounds();
    return m;
}

static PayloadEntity entity(f32 x, f32 y, f32 z, i32 parent = -1) {
    PayloadEntity e;
    e.name = "Cube";
    e.parent = parent;
    e.local.position = Vec3{x, y, z};
    e.hasMesh = true;
    e.mesh = 77;
    e.meshFlags = 1;
    return e;
}

int main() {
    AVER_INFO("=== ChunkProxyTest ===");
    const fmt::OcMeshData unit = cube();
    const auto meshOf = [&](u64 id) -> const fmt::OcMeshData* { return id == 77 ? &unit : nullptr; };

    ChunkPayload p;
    p.coord = ChunkCoord{40, -3, 0};   // far from the origin: 64000 cm, -4800 cm
    p.chunkSizeCm = 1600;
    p.entities = {entity(100, 100, 0), entity(400, 400, 0), entity(50, 0, 0, 1)};   // the last hangs off the second

    ChunkProxyOptions opt;
    opt.hlodCm = 20000;
    ChunkProxyResult r;
    std::string why;
    check(buildChunkProxy(p, meshOf, opt, r, &why), "a proxy is built: " + why);
    check(r.items == 3 && r.skipped == 0 && !r.truncated && r.trianglesIn == 36, "three cubes merged, nothing skipped");
    check(r.mesh.boundsMin.x == 100.0f && r.mesh.boundsMin.y == 100.0f && r.mesh.boundsMax.x == 550.0f && r.mesh.boundsMax.y == 500.0f,
          "vertices are relative to the chunk's corner, with the child composed onto its parent");
    fmt::OcMeshData again;
    ChunkProxyResult r2;
    buildChunkProxy(p, meshOf, opt, r2);
    check(r2.mesh.positions == r.mesh.positions && r2.mesh.indices == r.mesh.indices, "deterministic");

    {   // skips: no mesh, hidden, unknown mesh id, layered out
        ChunkPayload q = p;
        q.entities[0].hasMesh = false;
        q.entities[1].meshFlags = 0;
        q.entities.push_back(entity(900, 900, 0));
        q.entities.back().mesh = 5;   // the loader has none
        ChunkProxyResult s;
        check(buildChunkProxy(q, meshOf, opt, s, &why) && s.items == 1 && s.skipped == 3, "entities without a mesh, hidden, or with an unknown mesh are skipped");

        std::vector<fmt::OcDataLayer> defs = {{"Always", fmt::OcDataLayerState::Activated, false, ""}, {"Gameplay", fmt::OcDataLayerState::Activated, true, ""}};
        ChunkPayload l = p;
        PlacementExtras x;
        x.layers = {"Gameplay"};
        setPlacementExtras(l.entities[0], x);
        x.layers = {"Always"};
        setPlacementExtras(l.entities[1], x);
        ChunkProxyOptions lo = opt;
        lo.layers = &defs;
        ChunkProxyResult t;
        check(buildChunkProxy(l, meshOf, lo, t, &why) && t.items == 2 && t.skipped == 1, "a runtime-layer entity stays out of the proxy; an always-on layer stays in");
    }
    {   // caps
        ChunkPayload many = p;
        many.entities.clear();
        for (int i = 0; i < 50; ++i) many.entities.push_back(entity(10.0f * i, 0, 0));
        ChunkProxyOptions co = opt;
        co.maxItems = 7;
        ChunkProxyResult c;
        check(buildChunkProxy(many, meshOf, co, c, &why) && c.items == 7 && c.truncated, "the item cap truncates and says so");
        co = opt;
        co.timeBudgetMs = -1.0;   // already over budget: one item is still taken
        check(buildChunkProxy(many, meshOf, co, c, &why) && c.items == 1 && c.truncated, "an exhausted time budget still takes one item");
    }
    {   // budget reaches the simplifier
        ChunkPayload many = p;
        many.entities.clear();
        for (int i = 0; i < 20; ++i) many.entities.push_back(entity(60.0f * i, 0, 0));
        ChunkProxyOptions so = opt;
        so.hlod.minTriangles = 1; so.hlod.maxTriangles = 40;
        u32 asked = 0;
        so.hlod.simplify = [&](fmt::OcMeshData& m, u32 budget) { asked = budget; m.indices.resize(budget * 3); m.submeshes.resize(1); m.submeshes[0].indexCount = budget * 3; return true; };
        ChunkProxyResult s;
        check(buildChunkProxy(many, meshOf, so, s, &why) && asked > 0 && asked <= 40 && s.mesh.indices.size() == asked * 3, "the clamped budget is passed to the simplifier and honoured");
    }
    {
        ChunkPayload empty = p;
        empty.entities.clear();
        ChunkProxyResult e;
        check(!buildChunkProxy(empty, meshOf, opt, e, &why) && e.items == 0, "an empty chunk has no proxy");
    }

    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
