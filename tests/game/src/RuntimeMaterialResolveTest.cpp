// RuntimeMaterialResolveTest -- GameContent::authoredOrResolve, the lookup behind a surface nothing bound.
//
// Level-scoped residency binds only the materials the opened level places, so a mesh spawned at run time
// (Entity.SetMesh) named a surface with no authored handle and drew the flat fallback. authoredOrResolve finds the
// project's .ocmat by name on first sight and binds it; a token with no file is remembered, so it costs one lookup.
// The cases: an unbound token resolves and stays bound; a bound token is returned untouched; a missing token stays
// missing after the file appears (the miss was cached) until the project's materials are released; token 0 resolves
// to nothing.
//
// A temp project, no device, no window.
#include "aver/core/Log.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/game/GameContent.hpp"
#include "aver/scene/scene_abi.h"

#include <filesystem>
#include <fstream>
#include <string>

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

static void writeMaterial(const fs::path& dir, const std::string& name, const std::string& extra = "") {
    fs::create_directories(dir);
    std::ofstream out(dir / (name + ".ocmat"), std::ios::binary | std::ios::trunc);
    out << "OCMAT 1\nNAME " << name << "\n" << extra;
}

int main() {
    const fs::path root = fs::temp_directory_path() / "aver_runtime_material_resolve_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path content = root / "Content";
    writeMaterial(content / "Materials", "SpawnedRed");
    writeMaterial(content / "Materials", "Pending");
    writeMaterial(content / "Materials", "Doubled");

    fmt::ProjectDesc p;
    p.name = "RuntimeMaterials";
    p.dir = root.string();
    GameContent gc;
    gc.adopt(p);

    const i32 red = aver_scene_material(0, "SpawnedRed");
    const i32 none = aver_scene_material(0, "NoSuchMaterial");
    const i32 pending = aver_scene_material(0, "Pending");

    AVER_INFO("=== an unbound token resolves from the project ===");
    check(gc.authoredFor(red) == 0, "nothing bound the token before");
    const pbr::MaterialHandle h = gc.authoredOrResolve(red);
    check(h != 0, "the .ocmat under Content/Materials is found by the token's name");
    check(gc.authoredFor(red) == h, "and bound, so the plain lookup sees it afterwards");
    check(gc.authoredOrResolve(red) == h, "a second ask returns the same handle");

    AVER_INFO("=== a bound token is left alone ===");
    const pbr::MaterialHandle other = gc.materialForSurface("Doubled");
    gc.bindSurfaceMaterial(aver_scene_material(0, "Doubled"), other);
    check(gc.authoredOrResolve(aver_scene_material(0, "Doubled")) == other, "an already-bound token returns its handle");

    AVER_INFO("=== a miss is remembered ===");
    check(gc.authoredOrResolve(none) == 0, "a surface with no .ocmat resolves to nothing");
    check(gc.authoredOrResolve(0) == 0, "token 0 (named no material) resolves to nothing");
    check(gc.authoredFor(none) == 0, "and nothing is bound for it");
    // Remove the file the way a miss would see it, ask (a miss), then put it back: the cached miss must hold.
    fs::remove(content / "Materials" / "Pending.ocmat", ec);
    check(gc.authoredOrResolve(pending) == 0, "a token whose file is missing resolves to nothing");
    writeMaterial(content / "Materials", "Pending");
    check(gc.authoredOrResolve(pending) == 0, "the miss was cached: the file appearing later is not looked for per call");
    gc.releaseProjectMaterials();
    check(gc.authoredOrResolve(pending) != 0, "releasing the project's materials forgets the miss, so it resolves now");

    return g_failures ? 1 : 0;
}

#else

int main() {
    AVER_INFO("=== SKIPPED: built without the scene and material modules ===");
    (void)check;
    return 0;
}

#endif
