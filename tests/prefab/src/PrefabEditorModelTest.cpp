// The editor's prefab model (sandbox/src/PrefabEditorModel.cpp), driven with no UI: create a prefab
// from a selection and undo it, place, apply and save, the override rows, unpack.
#include "PrefabEditorModel.hpp"

#include "aver/core/Log.hpp"
#include "aver/scene/Components.hpp"

#include <cmath>
#include <filesystem>

using namespace aver;
using aver::editor::PrefabEdit;
using aver::editor::PrefabEditorModel;
using scene::Entity;
namespace fs = std::filesystem;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static void clearWorld(scene::World& w) {
    std::vector<Entity> all;
    for (u32 i = 0; i < w.count(); ++i) all.push_back(w.at(i));
    for (const Entity e : all) if (w.valid(e) && !w.destroyPending(e)) w.destroy(e);
    w.flush();
}

static Transform at(f32 x, f32 y, f32 z) { Transform t; t.position = Vec3{x, y, z}; return t; }
// The CTags.bits value the asset in memory has for its second node (the Lid).
static i64 assetTags(prefab::PrefabLibrary& lib) {
    const fmt::OcPrefabData* d = lib.peek("Prefabs/Crate.ocprefab");
    if (!d || d->nodes.size() < 2) return -1;
    for (const auto& c : d->nodes[1].components)
        if (c.type == "CTags") for (const auto& f : c.fields) if (f.name == "bits") return f.i;
    return -1;
}
static i32 tagsOf(scene::World& w, Entity e) {
    const auto* t = w.component<scene::CTags>(e, scene::kComponentTags);
    return t ? static_cast<i32>(t->bits) : -1;
}

int main() {
    AVER_INFO("PrefabEditorModelTest");
    scene::World& w = scene::World::instance();
    clearWorld(w);

    const fs::path base = fs::temp_directory_path() / "aver_prefab_editor_test";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base / "Content" / "Prefabs", ec);
    const std::string content = (base / "Content").string();

    prefab::PrefabLibrary lib;
    prefab::PrefabSystem sys(w, lib);
    PrefabEditorModel model(w, lib, sys);
    model.setContentDir(content);

    AVER_INFO("asset references");
    {
        check(PrefabEditorModel::assetRefFor(content, (base / "Content" / "Prefabs" / "A.ocprefab").string()) == "Prefabs/A.ocprefab",
              "a file under Content becomes a forward-slash reference");
        check(PrefabEditorModel::assetRefFor(content, (base / "Other" / "A.ocprefab").string()).empty(), "a file outside Content has none");
        check(PrefabEditorModel::isPrefabPath("x/Y.OCPREFAB") && !PrefabEditorModel::isPrefabPath("x/y.ocmesh"), "extension test");
    }

    AVER_INFO("create a prefab from a selection, replace it with an instance, undo and redo");
    Entity instRoot = scene::kInvalidEntity;
    {
        const Entity root = w.create("Crate", scene::kInvalidEntity, at(10, 20, 30));
        const Entity lid = w.create("Lid", root, at(0, 0, 50));
        if (auto* t = static_cast<scene::CTags*>(w.addComponent(lid, scene::kComponentTags))) t->bits = 3;
        w.flush();

        auto bad = model.createPrefab(root, "Prefabs/Crate.txt", true);
        check(!bad.ok && !bad.error.empty(), "a name without .ocprefab is refused");

        auto r = model.createPrefab(root, "Prefabs/Crate.ocprefab", true);
        check(r.ok, "created: " + r.error);
        check(fs::exists(fs::path(model.fullPathFor("Prefabs/Crate.ocprefab"))), "the asset is on disk");
        check(r.instance != scene::kInvalidEntity && r.hasEdit, "the selection became an instance with one undo entry");
        w.flush();
        check(!w.valid(root) && sys.isInstanceRoot(r.instance), "the original is gone");
        instRoot = r.instance;

        auto again = model.createPrefab(r.instance, "Prefabs/Crate.ocprefab", false);
        check(!again.ok, "an existing asset is never overwritten");

        const u32 id = sys.instanceIdOf(r.instance);
        check(model.undo(r.edit), "undo");
        w.flush();
        const Entity plain = w.find("Crate");
        check(plain != scene::kInvalidEntity && !sys.isLinked(plain), "the plain entities are back");
        check(sys.rootOfInstance(id) == scene::kInvalidEntity, "the instance is gone");
        check(std::fabs(w.localTransform(plain).position.x - 10.0f) < 1e-3f, "where they were");
        check(w.childCount(plain) == 1, "with their child");

        check(model.redo(r.edit), "redo");
        w.flush();
        instRoot = sys.rootOfInstance(id);
        check(instRoot != scene::kInvalidEntity && w.find("Crate") == instRoot, "the instance is back and the plain copy is not");
    }

    AVER_INFO("override rows, apply, and saving the asset");
    {
        PrefabEdit edit;
        const Entity lid = sys.entityAtPath(instRoot, std::to_string(lib.peek("Prefabs/Crate.ocprefab")->nodes[1].uid));
        check(lid != scene::kInvalidEntity, "the child is addressable");
        check(model.rows(lid, true).empty(), "a fresh instance has no rows");
        if (auto* t = w.component<scene::CTags>(lid, scene::kComponentTags)) t->bits = 9;
        const auto rows = model.rows(lid, false);
        check(rows.size() == 1 && rows[0].label == "CTags.bits = 9" && rows[0].nodeName == "Lid", "one row, described: " + (rows.empty() ? std::string() : rows[0].label));
        check(model.rows(instRoot, false).empty() && model.overrideCount(instRoot) == 1, "the root has none of its own, the instance has one");

        std::string why;
        check(model.applyAll(lid, edit, &why), "apply: " + why);
        check(tagsOf(w, lid) == 9 && model.overrideCount(lid) == 0, "nothing left to override");
        check(model.dirtyAssets().count("Prefabs/Crate.ocprefab") == 1, "the asset is marked dirty");
        check(model.saveDirtyAssets(&why) && model.dirtyAssets().empty(), "and saved: " + why);
        fmt::OcPrefabData onDisk;
        check(fmt::loadOcPrefab(model.fullPathFor("Prefabs/Crate.ocprefab"), onDisk, &why), "the file loads back: " + why);
        bool nine = false;
        for (const auto& n : onDisk.nodes) for (const auto& c : n.components) if (c.type == "CTags") for (const auto& f : c.fields) if (f.i == 9) nine = true;
        check(nine, "with the applied value in it");

        check(model.undo(edit), "undo the apply");
        check(tagsOf(w, lid) == 9 && model.overrideCount(lid) == 1, "the instance has its override again");
        check(assetTags(lib) == 3, "and the asset in memory is back to 3");
        check(model.redo(edit) && model.overrideCount(lid) == 0 && assetTags(lib) == 9, "redo");
    }

    AVER_INFO("place, revert and unpack");
    {
        PrefabEdit placeEdit;
        Entity second = scene::kInvalidEntity;
        std::string why;
        check(model.place("Prefabs/Crate.ocprefab", at(100, 0, 0), scene::kInvalidEntity, second, placeEdit, &why), "place: " + why);
        const u32 id2 = sys.instanceIdOf(second);
        check(tagsOf(w, sys.entityAtPath(second, std::to_string(lib.peek("Prefabs/Crate.ocprefab")->nodes[1].uid))) == 9, "a new instance follows the saved asset");

        PrefabEdit unpackEdit;
        check(model.unpack(second, unpackEdit, &why), "unpack: " + why);
        w.flush();
        check(sys.rootOfInstance(id2) == scene::kInvalidEntity, "the instance is gone");
        bool plainCrate = false;
        for (u32 i = 0; i < w.count(); ++i) {
            const Entity e = w.at(i);
            if (std::string(w.name(e)) == "Crate" && !sys.isLinked(e) && std::fabs(w.localTransform(e).position.x - 100.0f) < 1e-3f) plainCrate = true;
        }
        check(plainCrate, "ordinary entities took its place");
        check(model.undo(unpackEdit), "undo the unpack");
        w.flush();
        const Entity back = sys.rootOfInstance(id2);
        check(back != scene::kInvalidEntity, "the instance is back");
        check(model.undo(placeEdit), "undo the placement");
        w.flush();
        check(sys.rootOfInstance(id2) == scene::kInvalidEntity, "and gone again");
    }

    clearWorld(w);
    fs::remove_all(base, ec);
    if (g_failures) { AVER_ERROR("PrefabEditorModelTest: {} failure(s)", g_failures); return 1; }
    AVER_INFO("PrefabEditorModelTest: all passed");
    return 0;
}
