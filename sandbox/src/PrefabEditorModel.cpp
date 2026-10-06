#include "PrefabEditorModel.hpp"

#if AVER_MODULE_SCENE
#include "aver/core/Log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>

namespace aver::editor {
using scene::Entity;
using scene::kInvalidEntity;
namespace fs = std::filesystem;

namespace {

std::string g(f32 v) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", static_cast<double>(v));
    return b;
}

std::string valueText(const fmt::OcSaveField& f) {
    switch (f.kind) {
        case fmt::kOcPrefabKindF32:
        case fmt::kOcPrefabKindVec3:
        case fmt::kOcPrefabKindQuat: {
            std::string s = f.f.size() > 1 ? "(" : "";
            for (usize i = 0; i < f.f.size(); ++i) { if (i) s += ", "; s += g(f.f[i]); }
            if (f.f.size() > 1) s += ")";
            return s;
        }
        case fmt::kOcPrefabKindMat4: return "matrix";
        case fmt::kOcPrefabKindBool: return f.i ? "true" : "false";
        case fmt::kOcPrefabKindString: return "\"" + f.s + "\"";
        case fmt::kOcPrefabKindEntity: return f.i < 0 ? std::string("none") : "-> " + (f.s.empty() ? std::string("root") : f.s);
        default: return std::to_string(f.i);
    }
}

} // namespace

PrefabEditorModel::PrefabEditorModel(scene::World& world, prefab::PrefabLibrary& library, prefab::PrefabSystem& system)
    : world_(world), lib_(library), sys_(system) {}

void PrefabEditorModel::setContentDir(const std::string& dir) {
    contentDir_ = dir;
    lib_.setLoader([this](const std::string& path, fmt::OcPrefabData& out, std::string* why) {
        return fmt::loadOcPrefab(fullPathFor(path), out, why);
    });
    lib_.setSaver([this](const std::string& path, const fmt::OcPrefabData& data, std::string* why) {
        return fmt::saveOcPrefab(fullPathFor(path), data, why);
    });
}

std::string PrefabEditorModel::assetRefFor(const std::string& contentDir, const std::string& fullPath) {
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(fs::path(contentDir), ec);
    const fs::path full = fs::weakly_canonical(fs::path(fullPath), ec);
    const fs::path rel = full.lexically_relative(root);
    const std::string s = rel.generic_string();
    if (s.empty() || s == "." || s.rfind("..", 0) == 0) return {};
    return s;
}

std::string PrefabEditorModel::fullPathFor(const std::string& assetRef) const {
    return (fs::path(contentDir_) / fs::path(assetRef)).make_preferred().string();
}

bool PrefabEditorModel::isPrefabPath(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".ocprefab";
}

template <class F>
bool PrefabEditorModel::runOnInstance(const char* label, Entity e, bool touchesAsset, PrefabEdit& edit, F&& op) {
    const Entity root = sys_.instanceRoot(e);
    if (root == kInvalidEntity) return false;
    const u32 id = sys_.instanceIdOf(root);
    std::vector<std::string> libs;
    if (touchesAsset) libs.push_back(sys_.prefabPathOf(root));
    PrefabEdit tmp;
    tmp.label = label;
    tmp.before = sys_.captureState(libs, {id}, {});
    if (!op(root)) return false;
    tmp.after = sys_.captureState(libs, {id}, {});
    if (touchesAsset) dirty_.insert(libs.front());
    edit = std::move(tmp);
    return true;
}

// ---- create / place -------------------------------------------------------------------------------------

PrefabEditorModel::CreateResult PrefabEditorModel::createPrefab(Entity root, const std::string& assetRef,
                                                                bool replaceWithInstance) {
    CreateResult r;
    if (assetRef.empty() || !isPrefabPath(assetRef)) { r.error = "a prefab is saved as <name>.ocprefab"; return r; }
    if (!world_.valid(root) || world_.destroyPending(root)) { r.error = "nothing is selected"; return r; }
    std::error_code ec;
    if (fs::exists(fullPathFor(assetRef), ec)) { r.error = assetRef + " already exists"; return r; }

    fmt::OcPrefabData data;
    std::string why;
    if (!sys_.captureAsPrefab(root, fs::path(assetRef).stem().string(), data, &why)) { r.error = why; return r; }
    lib_.set(assetRef, data);
    if (!lib_.save(assetRef, &why)) {
        lib_.drop(assetRef);
        r.error = why;
        return r;
    }
    r.assetRef = prefab::PrefabLibrary::normalize(assetRef);
    r.ok = true;
    if (!replaceWithInstance) return r;

    const u32 group = sys_.registerPlain(root);
    PrefabEdit edit;
    edit.label = "Create Prefab";
    edit.before = sys_.captureState({}, {}, {group});
    const Entity inst = sys_.convertToInstance(root, assetRef, &why);
    if (inst == kInvalidEntity) { r.error = "the prefab was saved but the selection could not be replaced: " + why; return r; }
    const u32 id = sys_.instanceIdOf(inst);
    edit.after = sys_.captureState({}, {id}, {group});
    edit.before.instances.push_back(prefab::InstanceSnapshot{id, false, {}, {}, {}, kInvalidEntity, {}});
    r.instance = inst;
    r.hasEdit = true;
    r.edit = std::move(edit);
    return r;
}

bool PrefabEditorModel::place(const std::string& assetRef, const Transform& xf, Entity parent, Entity& outRoot,
                              PrefabEdit& edit, std::string* why) {
    const Entity root = sys_.instantiate(assetRef, xf, parent, {}, {}, why);
    if (root == kInvalidEntity) return false;
    const u32 id = sys_.instanceIdOf(root);
    PrefabEdit tmp;
    tmp.label = "Place Prefab";
    tmp.before.instances.push_back(prefab::InstanceSnapshot{id, false, {}, {}, {}, kInvalidEntity, {}});
    tmp.after = sys_.captureState({}, {id}, {});
    edit = std::move(tmp);
    outRoot = root;
    return true;
}

// ---- instance operations ----------------------------------------------------------------------------------

bool PrefabEditorModel::applyAll(Entity e, PrefabEdit& edit, std::string* why) {
    return runOnInstance("Apply to Prefab", e, true, edit, [&](Entity root) { return sys_.applyAll(root, why); });
}

bool PrefabEditorModel::applyOverride(Entity e, const fmt::OcPrefabOverride& ov, PrefabEdit& edit, std::string* why) {
    return runOnInstance("Apply Override to Prefab", e, true, edit,
                         [&](Entity root) { return sys_.applyOverride(root, ov, why); });
}

bool PrefabEditorModel::revertAll(Entity e, PrefabEdit& edit) {
    return runOnInstance("Revert Prefab Overrides", e, false, edit, [&](Entity root) { return sys_.revertAll(root); });
}

bool PrefabEditorModel::revertOverride(Entity e, const fmt::OcPrefabOverride& ov, PrefabEdit& edit) {
    return runOnInstance("Revert Override", e, false, edit, [&](Entity root) { return sys_.revertOverride(root, ov); });
}

bool PrefabEditorModel::revertNode(Entity e, PrefabEdit& edit) {
    return runOnInstance("Revert Node", e, false, edit, [&](Entity) { return sys_.revertNode(e); });
}

bool PrefabEditorModel::destroyInstance(Entity e, PrefabEdit& edit) {
    return runOnInstance("Delete Prefab Instance", e, false, edit, [&](Entity root) { return sys_.destroyInstance(root); });
}

bool PrefabEditorModel::unpack(Entity e, PrefabEdit& edit, std::string* why) {
    const Entity root = sys_.instanceRoot(e);
    if (root == kInvalidEntity) return false;
    const u32 id = sys_.instanceIdOf(root);
    const u32 group = sys_.registerPlain(kInvalidEntity);

    PrefabEdit tmp;
    tmp.label = "Unpack Prefab";
    tmp.before = sys_.captureState({}, {id}, {});
    tmp.before.plains.push_back(prefab::PlainSnapshot{group, false, {}, {}, kInvalidEntity});

    // The ordinary entities that replace it come from a flat capture of the instance as it stands,
    // overrides included, so unpacking changes nothing the author can see.
    prefab::PlainSnapshot plain;
    plain.groupId = group;
    plain.local = world_.localTransform(root);
    plain.parent = world_.parent(root);
    if (!sys_.captureAsPrefab(root, world_.name(root), plain.data, why)) return false;
    prefab::PrefabState apply;
    apply.plains.push_back(std::move(plain));
    apply.instances.push_back(prefab::InstanceSnapshot{id, false, {}, {}, {}, kInvalidEntity, {}});
    if (!sys_.restoreState(apply, why)) return false;

    tmp.after = sys_.captureState({}, {id}, {group});
    edit = std::move(tmp);
    return true;
}

bool PrefabEditorModel::updateAsset(const std::string& assetRef, fmt::OcPrefabData data, PrefabEdit& edit,
                                    std::string* why) {
    const std::string path = prefab::PrefabLibrary::normalize(assetRef);
    PrefabEdit tmp;
    tmp.label = "Edit Prefab";
    tmp.before = sys_.captureState({path}, {}, {});
    if (!sys_.updatePrefab(path, std::move(data), why)) return false;
    tmp.after = sys_.captureState({path}, {}, {});
    dirty_.insert(path);
    edit = std::move(tmp);
    return true;
}

bool PrefabEditorModel::saveAsset(const std::string& assetRef, std::string* why) {
    const std::string path = prefab::PrefabLibrary::normalize(assetRef);
    if (!lib_.save(path, why)) return false;
    dirty_.erase(path);
    return true;
}

bool PrefabEditorModel::saveDirtyAssets(std::string* why) {
    bool ok = true;
    const std::set<std::string> todo = dirty_;
    for (const std::string& p : todo) ok = saveAsset(p, why) && ok;
    return ok;
}

// ---- Details ------------------------------------------------------------------------------------------------

std::vector<PrefabOverrideRow> PrefabEditorModel::rows(Entity e, bool wholeInstance) {
    std::vector<PrefabOverrideRow> out;
    const Entity root = sys_.instanceRoot(e);
    if (root == kInvalidEntity) return out;
    std::vector<fmt::OcPrefabOverride> ov;
    if (!sys_.computeOverrides(root, ov)) return out;
    std::string mine;
    if (!wholeInstance && !sys_.nodePathOf(e, mine)) return out;
    for (const fmt::OcPrefabOverride& o : ov) {
        if (!wholeInstance && o.path != mine) continue;
        PrefabOverrideRow row;
        row.ov = o;
        row.nodePath = o.path;
        const Entity n = sys_.entityAtPath(root, o.path);
        row.nodeName = n != kInvalidEntity ? world_.name(n) : o.path;
        row.label = describe(o);
        out.push_back(std::move(row));
    }
    return out;
}

int PrefabEditorModel::overrideCount(Entity e) {
    return static_cast<int>(rows(e, true).size());
}

std::string PrefabEditorModel::describe(const fmt::OcPrefabOverride& ov) {
    switch (ov.op) {
        case fmt::OcOverrideOp::AddComponent:    return "+ " + ov.component;
        case fmt::OcOverrideOp::RemoveComponent: return "- " + ov.component;
        case fmt::OcOverrideOp::Set:
            if (ov.component == fmt::kOcPrefabNodeComponent) return "Name = " + valueText(ov.value);
            return ov.component + "." + ov.value.name + " = " + valueText(ov.value);
    }
    return {};
}

} // namespace aver::editor
#endif // AVER_MODULE_SCENE
