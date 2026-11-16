// LevelPartsTest -- GameContent::buildLevelParts: a coarser LOD level of a mesh that names several materials is cut into the
// same parts as LOD 0, so a level the draw walk substitutes still draws each slot with its own material.
//
//   MESHLETS      a level with material-pure meshlets is cut by each meshlet's slot
//   OWNERSHIP     without them, by which part's LOD-0 slice owns each vertex
//   REFUSAL       a triangle using two parts' vertices refuses the level and registers nothing
//   LOOKUP        levelPartsFor answers only for the mesh id the level was cut from; a single-material mesh has no parts to cut
// A recording device stands in for the GPU: it hands out handles and keeps each part's index list.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/rhi/RHI.hpp"

#include <map>
#include <string>
#include <vector>

using namespace aver;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

#if AVER_MODULE_SCENE

#  include "aver/game/GameContent.hpp"

namespace {

struct RecordingDev final : public rhi::IDevice {
    rhi::Backend backend() const override { return rhi::Backend::Null; }
    const char* adapterName() const override { return "no gpu"; }
    rhi::ISwapchain* createSwapchain(const rhi::SwapchainDesc&) override { return nullptr; }
    void beginFrame() override {}
    void endFrame() override {}
    rhi::MeshHandle createMesh(const rhi::MeshVertex*, u32, const u32* indices, u32 indexCount) override {
        const rhi::MeshHandle h = next++;
        made[h].assign(indices, indices + indexCount);
        return h;
    }
    rhi::MeshHandle createMeshSharingVertices(rhi::MeshHandle source, const u32* indices, u32 indexCount) override {
        if (!made.count(source)) return 0;
        const rhi::MeshHandle h = next++;
        made[h].assign(indices, indices + indexCount);
        return h;
    }
    bool destroyMesh(rhi::MeshHandle mesh) override { return made.erase(mesh) > 0; }
    rhi::MeshHandle next = 1;
    std::map<rhi::MeshHandle, std::vector<u32>> made;
};

// Two quads, one per material slot: slot 0 owns vertices 0-3, slot 1 owns 4-7. LOD 0 is two triangles each; the one coarser
// level keeps one triangle of each quad.
fmt::OcMeshData twoMaterialMesh() {
    fmt::OcMeshData md;
    for (int q = 0; q < 2; ++q)
        for (int v = 0; v < 4; ++v) {
            md.positions.insert(md.positions.end(), {static_cast<f32>(v & 1) + 3.0f * q, static_cast<f32>(v >> 1), 0.0f});
            md.normals.insert(md.normals.end(), {0.0f, 0.0f, 1.0f});
            md.uvs.insert(md.uvs.end(), {0.0f, 0.0f});
        }
    md.indices = {0, 1, 2, 2, 1, 3,   4, 5, 6, 6, 5, 7};
    md.materialSlots = {"", ""};
    fmt::OcMeshSubmesh a; a.name = "a"; a.materialSlot = 0; a.indexStart = 0; a.indexCount = 6;
    fmt::OcMeshSubmesh b; b.name = "b"; b.materialSlot = 1; b.indexStart = 6; b.indexCount = 6;
    md.submeshes = {a, b};
    fmt::OcMeshLod lod;
    lod.indices = {0, 1, 3,   4, 5, 7};
    md.coarserLods.push_back(lod);
    return md;
}

}  // namespace

int main() {
    AVER_INFO("LevelPartsTest");

    {
        AVER_INFO("-- OWNERSHIP: no meshlets, so each coarse triangle goes to the part owning its vertices --");
        RecordingDev dev;
        game::GameContent content;
        const fmt::OcMeshData md = twoMaterialMesh();
        check(content.uploadBuiltMesh(dev, 7, md, "Meshes/two.ocmesh"), "the two-material mesh uploads");
        const auto* parts = content.partsFor(7);
        check(parts && parts->size() == 2, "and splits into two parts");
        const rhi::MeshHandle whole = content.meshFor(7);
        const rhi::MeshHandle level = dev.createMesh(nullptr, 0, md.coarserLods[0].indices.data(), 6);
        check(content.buildLevelParts(dev, 7, md, 1, whole, level), "level 1 is cut by vertex ownership");
        const auto* lp = content.levelPartsFor(level, 7);
        check(lp && lp->size() == 2, "into the same two parts");
        if (lp && lp->size() == 2) {
            check((*lp)[0].slot == 0 && (*lp)[1].slot == 1, "with the base parts' slots");
            check(dev.made[(*lp)[0].mesh] == std::vector<u32>{0, 1, 3}, "slot 0 gets the triangle on vertices 0-3");
            check(dev.made[(*lp)[1].mesh] == std::vector<u32>{4, 5, 7}, "slot 1 gets the triangle on vertices 4-7");
        }
        check(content.levelPartsFor(level, 8) == nullptr, "another mesh id asking about this level gets nothing");
        check(content.levelPartsFor(level + 100, 7) == nullptr, "a handle never cut gets nothing");
    }

    {
        AVER_INFO("-- MESHLETS: material-pure meshlets name each triangle's slot --");
        RecordingDev dev;
        game::GameContent content;
        fmt::OcMeshData md = twoMaterialMesh();
        md.meshletsMaterialPure = true;
        fmt::OcMeshMeshlet m1; m1.vertices = {4, 6, 7}; m1.triangles = {0, 1, 2}; m1.materialSlot = 1;
        fmt::OcMeshMeshlet m0; m0.vertices = {0, 2, 3}; m0.triangles = {0, 1, 2}; m0.materialSlot = 0;
        md.coarserLods[0].meshlets = {m1, m0};
        md.coarserLods[0].indices = {4, 6, 7, 0, 2, 3};
        check(content.uploadBuiltMesh(dev, 7, md, "Meshes/two.ocmesh"), "the mesh uploads");
        const rhi::MeshHandle level = dev.createMesh(nullptr, 0, md.coarserLods[0].indices.data(), 6);
        check(content.buildLevelParts(dev, 7, md, 1, content.meshFor(7), level), "level 1 is cut by meshlet slot");
        const auto* lp = content.levelPartsFor(level, 7);
        check(lp && lp->size() == 2, "two parts");
        if (lp && lp->size() == 2) {
            check(dev.made[(*lp)[0].mesh] == std::vector<u32>{0, 2, 3}, "slot 0's meshlet lands in slot 0's part");
            check(dev.made[(*lp)[1].mesh] == std::vector<u32>{4, 6, 7}, "slot 1's meshlet lands in slot 1's part");
        }
    }

    {
        AVER_INFO("-- REFUSAL: a triangle across two parts cannot be given one material --");
        RecordingDev dev;
        game::GameContent content;
        fmt::OcMeshData md = twoMaterialMesh();
        md.coarserLods[0].indices = {0, 1, 4};
        check(content.uploadBuiltMesh(dev, 7, md, "Meshes/two.ocmesh"), "the mesh uploads");
        const usize before = dev.made.size();
        const rhi::MeshHandle level = dev.createMesh(nullptr, 0, md.coarserLods[0].indices.data(), 3);
        check(!content.buildLevelParts(dev, 7, md, 1, content.meshFor(7), level), "the level is refused");
        check(content.levelPartsFor(level, 7) == nullptr, "nothing is registered for it");
        check(dev.made.size() == before + 1, "and no part mesh is left behind");
    }

    {
        AVER_INFO("-- one material: nothing to cut --");
        RecordingDev dev;
        game::GameContent content;
        fmt::OcMeshData md = twoMaterialMesh();
        md.submeshes.resize(1);
        md.submeshes[0].indexCount = 12;
        check(content.uploadBuiltMesh(dev, 7, md, "Meshes/one.ocmesh") && content.partsFor(7) == nullptr, "a one-submesh mesh has no parts");
        check(!content.buildLevelParts(dev, 7, md, 1, content.meshFor(7), 99), "so there is no level to cut");
    }

    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}

#else

int main() {
    AVER_INFO("=== SKIPPED: built without the scene module ===");
    return exitCode(ExitCode::Ok);
}

#endif
