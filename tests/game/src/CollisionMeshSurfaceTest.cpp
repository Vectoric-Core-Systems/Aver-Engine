// CollisionMeshSurfaceTest -- GameContent::collisionMeshFor with physical surfaces (docs/plans/wave11/SHAPES_MATERIALS.md).
//
// A material slot whose .ocmat names a PHYSSURFACE gives the collision triangles built from it that surface: the mesh carries a
// palette of surface ids and one palette index per triangle. The cases: the palette and each triangle's index match the slot
// that triangle came from; the seam between two surface groups is welded (no crack, the same vertices on both sides); a mesh
// that names no surface carries none; more than 32 surfaces fall back to "none" for the rest; the disk cache round-trips the
// palette, a version-1 entry is a miss, and editing a slot's PHYSSURFACE makes a cached entry stale.
//
// A temp project, no device, no window, no physics world.
#include "aver/core/Log.hpp"
#include "aver/core/SurfaceId.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/game/GameContent.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace aver;
using namespace aver::game;
namespace fs = std::filesystem;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

#if AVER_MODULE_SCENE && AVER_MODULE_PBR

static void writeText(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

static void writeMaterial(const fs::path& content, const std::string& name, const std::string& surface) {
    std::string text = "OCMAT 1\nNAME " + name + "\n";
    if (!surface.empty()) text += "PHYSSURFACE " + surface + "\n";
    writeText(content / "Materials" / (name + ".ocmat"), text);
}

// A 5 x 5 vertex grid on z = 0, 100 cm apart (32 triangles). Quads in columns 0 and 1 (x < 200) are submesh 0, columns 2 and 3
// submesh 1: two flat halves that meet along x = 200.
static fmt::OcMeshData gridMesh(const std::string& slotA, const std::string& slotB) {
    fmt::OcMeshData m;
    for (int y = 0; y < 5; ++y)
        for (int x = 0; x < 5; ++x) {
            m.positions.insert(m.positions.end(), {100.0f * x, 100.0f * y, 0.0f});
            m.normals.insert(m.normals.end(), {0.0f, 0.0f, 1.0f});
            m.uvs.insert(m.uvs.end(), {0.25f * x, 0.25f * y});
        }
    const auto quadsOfColumns = [&](int c0, int c1) {
        for (int y = 0; y < 4; ++y)
            for (int x = c0; x < c1; ++x) {
                const u32 a = y * 5 + x, b = a + 1, c = a + 5, d = c + 1;
                m.indices.insert(m.indices.end(), {a, b, d, a, d, c});
            }
    };
    quadsOfColumns(0, 2);
    const u32 firstCount = static_cast<u32>(m.indices.size());
    quadsOfColumns(2, 4);
    m.materialSlots = {slotA, slotB};
    fmt::OcMeshSubmesh s0, s1;
    s0.name = "left";  s0.materialSlot = 0; s0.indexStart = 0;          s0.indexCount = firstCount;
    s1.name = "right"; s1.materialSlot = 1; s1.indexStart = firstCount; s1.indexCount = static_cast<u32>(m.indices.size()) - firstCount;
    m.submeshes = {s0, s1};
    return m;
}

static fmt::ProjectDesc projectAt(const fs::path& dir) {
    fmt::ProjectDesc p;
    p.name = "CollisionSurfaces";
    p.dir = dir.string();
    return p;
}

static u64 meshId(const GameContent& c, const std::string& fileName) {
    for (const auto& [id, path] : c.index())
        if (path.size() >= fileName.size() && path.compare(path.size() - fileName.size(), fileName.size(), fileName) == 0) return id;
    return 0;
}

// The centroid x of triangle t.
static float centroidX(const GameContent::CollisionMesh& cm, usize t) {
    float x = 0.0f;
    for (int k = 0; k < 3; ++k) x += cm.positions[usize(cm.indices[t * 3 + k]) * 3];
    return x / 3.0f;
}

int main() {
    const fs::path root = fs::temp_directory_path() / "aver_collision_surface_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path content = root / "Content";
    std::string why;
    fs::create_directories(content / "Meshes", ec);

    // ---- two slots, one naming a surface ---------------------------------------------------------------------------------
    writeMaterial(content, "IceSlot", "TestIce");
    writeMaterial(content, "PlainSlot", "");
    check(fmt::saveOcMesh((content / "Meshes" / "floor.ocmesh").string(), gridMesh("IceSlot", "PlainSlot"), &why), "wrote the two-slot floor (" + why + ")");

    const i32 iceId = surfaceIdFromName("TestIce");
    GameContent first;
    first.adopt(projectAt(root));
    const u64 id = meshId(first, "floor.ocmesh");
    check(id != 0, "the project indexed the mesh");

    AVER_INFO("=== the palette follows the slots ===");
    const GameContent::CollisionMesh* cm = first.collisionMeshFor(id);
    check(cm != nullptr && cm->indices.size() >= 3, "collisionMeshFor builds a mesh");
    if (!cm) return 1;
    check(cm->palette.size() == 2 && cm->palette[0] == iceId && cm->palette[1] == 0, "the palette is {TestIce, none} in the order the slots appear");
    check(cm->triPalette.size() == cm->indices.size() / 3, "one palette index per triangle");
    bool sidesMatch = !cm->triPalette.empty();
    for (usize t = 0; t < cm->triPalette.size(); ++t) {
        const bool left = centroidX(*cm, t) < 200.0f;
        sidesMatch = sidesMatch && cm->triPalette[t] == (left ? 0 : 1);
    }
    check(sidesMatch, "every triangle on the left of x = 200 is TestIce and every one on the right is none");
    check(cm->slotSig.size() == 2, "the build recorded what it saw of the two slots");

    AVER_INFO("=== no crack along the seam ===");
    // Both groups are welded and simplified on their own with their borders locked, then welded together: the vertices on x = 200
    // are the same vertices on both sides.
    std::set<u32> leftSeam, rightSeam;
    for (usize t = 0; t < cm->triPalette.size(); ++t)
        for (int k = 0; k < 3; ++k) {
            const u32 v = cm->indices[t * 3 + k];
            if (std::fabs(cm->positions[usize(v) * 3] - 200.0f) < 1e-3f) (cm->triPalette[t] == 0 ? leftSeam : rightSeam).insert(v);
        }
    check(leftSeam.size() == 5 && leftSeam == rightSeam, "the five seam vertices are shared by both surfaces (" + std::to_string(leftSeam.size()) + " left, " + std::to_string(rightSeam.size()) + " right)");

    AVER_INFO("=== the disk cache ===");
    check(first.collisionCacheHits() == 0, "the first build was not a cache hit");
    GameContent second;
    second.adopt(projectAt(root));
    const GameContent::CollisionMesh* cached = second.collisionMeshFor(id);
    check(second.collisionCacheHits() == 1 && cached != nullptr, "a second GameContent reads the entry from the disk cache");
    if (cached)
        check(cached->palette == cm->palette && cached->triPalette == cm->triPalette && cached->indices == cm->indices && cached->positions == cm->positions,
              "and gets the same palette, per-triangle indices and geometry");

    // Editing the slot's PHYSSURFACE changes what the cached entry describes: it must be rebuilt, not trusted.
    writeMaterial(content, "IceSlot", "");
    GameContent third;
    third.adopt(projectAt(root));
    const GameContent::CollisionMesh* rebuilt = third.collisionMeshFor(id);
    check(third.collisionCacheHits() == 0 && rebuilt != nullptr, "after the .ocmat lost its PHYSSURFACE the cached entry is stale and the mesh is rebuilt");
    check(rebuilt && rebuilt->palette.empty() && rebuilt->triPalette.empty(), "a mesh that names no surface carries no palette");

    // The rebuild rewrote the entry; a version-1 file is a miss.
    GameContent fourth;
    fourth.adopt(projectAt(root));
    fourth.collisionMeshFor(id);
    check(fourth.collisionCacheHits() == 1, "the rewritten entry is a hit again");
    const fs::path cacheDir = root / "Saved" / "DerivedDataCache" / "Collision";
    int patched = 0;
    for (const fs::directory_entry& e : fs::directory_iterator(cacheDir, ec)) {
        if (e.path().extension() != ".occol") continue;
        std::fstream f(e.path(), std::ios::in | std::ios::out | std::ios::binary);
        const std::uint32_t one = 1;
        f.seekp(4);   // magic, then the version
        f.write(reinterpret_cast<const char*>(&one), sizeof one);
        ++patched;
    }
    GameContent fifth;
    fifth.adopt(projectAt(root));
    const GameContent::CollisionMesh* afterV1 = fifth.collisionMeshFor(id);
    check(patched >= 1 && fifth.collisionCacheHits() == 0 && afterV1 != nullptr, "an entry marked version 1 is a miss and the mesh is rebuilt");

    AVER_INFO("=== a mesh that names no surface ===");
    writeMaterial(content, "IceSlot", "");
    writeMaterial(content, "PlainSlot", "");
    check(fmt::saveOcMesh((content / "Meshes" / "plain.ocmesh").string(), gridMesh("IceSlot", "PlainSlot"), &why), "wrote a plain floor (" + why + ")");
    GameContent plain;
    plain.adopt(projectAt(root));
    const GameContent::CollisionMesh* pm = plain.collisionMeshFor(meshId(plain, "plain.ocmesh"));
    check(pm != nullptr && pm->palette.empty() && pm->triPalette.empty() && pm->indices.size() >= 3 && pm->indices.size() % 3 == 0,
          "no PHYSSURFACE anywhere: geometry only, no palette");

    AVER_INFO("=== more surfaces than a mesh can hold ===");
    {
        fmt::OcMeshData big;
        big.materialSlots.clear();
        for (int q = 0; q < 40; ++q) {
            const float x = 1000.0f * q;
            const u32 base = static_cast<u32>(big.positions.size() / 3);
            for (const auto& c : {std::pair{0.0f, 0.0f}, std::pair{100.0f, 0.0f}, std::pair{100.0f, 100.0f}, std::pair{0.0f, 100.0f}}) {
                big.positions.insert(big.positions.end(), {x + c.first, c.second, 0.0f});
                big.normals.insert(big.normals.end(), {0.0f, 0.0f, 1.0f});
                big.uvs.insert(big.uvs.end(), {0.0f, 0.0f});
            }
            const u32 start = static_cast<u32>(big.indices.size());
            big.indices.insert(big.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
            const std::string slot = "Many" + std::to_string(q);
            writeMaterial(content, slot, "ManySurface" + std::to_string(q));
            big.materialSlots.push_back(slot);
            fmt::OcMeshSubmesh s;
            s.name = slot; s.materialSlot = static_cast<u32>(q); s.indexStart = start; s.indexCount = 6;
            big.submeshes.push_back(s);
        }
        check(fmt::saveOcMesh((content / "Meshes" / "many.ocmesh").string(), big, &why), "wrote a mesh with 40 slots, each its own surface (" + why + ")");
        GameContent many;
        many.adopt(projectAt(root));
        const GameContent::CollisionMesh* mm = many.collisionMeshFor(meshId(many, "many.ocmesh"));
        check(mm != nullptr && mm->palette.size() == 32, "the palette is capped at 32");
        if (mm && mm->palette.size() == 32) {
            check(mm->palette[0] == surfaceIdFromName("ManySurface0") && mm->palette[30] == surfaceIdFromName("ManySurface30"), "the first 31 surfaces keep their entries");
            check(mm->palette[31] == 0, "the rest share entry 31, which is no surface");
            bool inRange = mm->triPalette.size() == mm->indices.size() / 3;
            for (const u8 p : mm->triPalette) inRange = inRange && p < 32;
            check(inRange && mm->indices.size() / 3 == 80, "every triangle (80 of them) has a valid palette index");
        }
    }

    fs::remove_all(root, ec);
    AVER_INFO("=== {} ===", g_failures == 0 ? "ok" : "FAILED");
    return g_failures;
}

#else

int main() {
    AVER_INFO("=== SKIPPED: built without the scene and material modules ===");
    (void)check;
    return 0;
}

#endif
