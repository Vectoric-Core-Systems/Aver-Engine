#include "PrefabFields.hpp"

#include "aver/formats/OcPrefab.hpp"
#include "aver/scene/scene_abi.h"

#include <cstring>

namespace aver::prefab::detail {

using scene::FieldKind;

bool excludedComponent(u32 type, u32 linkType) {
    return type == 0 || type == linkType || type == scene::kComponentWorld ||
           type == scene::kComponentHierarchy || type == scene::kComponentName;
}

bool ignoredField(const std::string& component, const std::string& field) {
    return component == "CParticleEmitter" && (field == "seed" || field == "age");
}

bool isMaterialField(const scene::FieldDesc& d) {
    return d.component == scene::kComponentMeshRenderer && d.kind == FieldKind::I32 &&
           std::strcmp(d.name, "material") == 0;
}

namespace {

const u8* addrOf(const scene::World& w, scene::Entity e, const scene::FieldDesc& d) {
    const void* b = w.getComponent(e, d.component);
    return b ? static_cast<const u8*>(b) + d.offset : nullptr;
}

} // namespace

fmt::OcSaveField zeroField(const scene::FieldDesc& d) {
    fmt::OcSaveField f;
    f.name = d.name;
    f.kind = isMaterialField(d) ? fmt::kOcPrefabKindString : static_cast<u32>(d.kind);
    if (f.kind != fmt::kOcPrefabKindString && f.kind != fmt::kOcPrefabKindEntity) {
        const u32 n = fmt::ocSaveFloatCount(f.kind);
        f.f.assign(n, 0.0f);
    }
    if (f.kind == fmt::kOcPrefabKindEntity) f.i = -1;
    return f;
}

bool readField(const scene::World& w, scene::Entity e, u32 fieldId, const PathLookup& lookup,
               fmt::OcSaveField& out) {
    const scene::FieldDesc* d = w.field(fieldId);
    if (!d) return false;
    const u8* a = addrOf(w, e, *d);
    if (!a) return false;

    out = fmt::OcSaveField{};
    out.name = d->name;
    out.kind = static_cast<u32>(d->kind);

    if (isMaterialField(*d)) {
        i32 token = 0;
        std::memcpy(&token, a, 4);
        out.kind = fmt::kOcPrefabKindString;
        out.s = token > 0 ? aver_scene_material_name(token) : "";
        return true;
    }

    switch (d->kind) {
        case FieldKind::F32:
        case FieldKind::Vec3:
        case FieldKind::Quat:
        case FieldKind::Mat4: {
            const u32 n = fmt::ocSaveFloatCount(out.kind);
            out.f.resize(n);
            std::memcpy(out.f.data(), a, n * sizeof(f32));
            return true;
        }
        case FieldKind::I32: { i32 v; std::memcpy(&v, a, 4); out.i = v; return true; }
        case FieldKind::Bool: { i32 v; std::memcpy(&v, a, 4); out.i = v != 0 ? 1 : 0; return true; }
        case FieldKind::I64: { i64 v; std::memcpy(&v, a, 8); out.i = v; return true; }
        case FieldKind::Entity: {
            u32 raw; std::memcpy(&raw, a, 4);
            const scene::Entity target = static_cast<scene::Entity>(raw);
            if (target == scene::kInvalidEntity || !w.valid(target)) { out.i = -1; return true; }
            std::string path;
            if (!lookup || !lookup(target, path)) return false;
            out.i = 0;
            out.s = std::move(path);
            return true;
        }
        case FieldKind::String: {
            const char* s = aver_scene_get_str(static_cast<i32>(e), static_cast<i32>(fieldId));
            out.s = s ? s : "";
            return true;
        }
    }
    return false;
}

Write writeField(scene::World& w, scene::Entity e, u32 fieldId, const fmt::OcSaveField& f,
                 const EntityLookup& lookup) {
    const scene::FieldDesc* d = w.field(fieldId);
    if (!d || d->readOnly) return Write::Failed;
    void* bytes = w.getComponent(e, d->component);
    if (!bytes) return Write::Failed;
    u8* a = static_cast<u8*>(bytes) + d->offset;

    const auto finish = [&](bool changed) {
        if (!changed) return Write::Same;
        if (d->component == scene::kComponentLocal) w.touchLocal(e);
        else if (d->component == scene::kComponentMeshRenderer)
            static_cast<scene::CMeshRenderer*>(bytes)->dirty = 1;
        return Write::Changed;
    };

    if (isMaterialField(*d)) {
        if (f.kind != fmt::kOcPrefabKindString) return Write::Failed;
        const i32 want = f.s.empty() ? 0 : aver_scene_material(0, f.s.c_str());
        i32 cur; std::memcpy(&cur, a, 4);
        if (cur == want) return Write::Same;
        std::memcpy(a, &want, 4);
        return finish(true);
    }

    // A kind change between builds is a dropped field, not a reinterpretation of the bytes.
    if (static_cast<u32>(d->kind) != f.kind) return Write::Failed;

    switch (d->kind) {
        case FieldKind::F32:
        case FieldKind::Vec3:
        case FieldKind::Quat:
        case FieldKind::Mat4: {
            const u32 n = fmt::ocSaveFloatCount(f.kind);
            if (f.f.size() != n) return Write::Failed;
            if (std::memcmp(a, f.f.data(), n * sizeof(f32)) == 0) return Write::Same;
            std::memcpy(a, f.f.data(), n * sizeof(f32));
            return finish(true);
        }
        case FieldKind::I32:
        case FieldKind::Bool: {
            const i32 want = d->kind == FieldKind::Bool ? (f.i != 0 ? 1 : 0) : static_cast<i32>(f.i);
            i32 cur; std::memcpy(&cur, a, 4);
            if (cur == want) return Write::Same;
            std::memcpy(a, &want, 4);
            return finish(true);
        }
        case FieldKind::I64: {
            const i64 want = f.i;
            i64 cur; std::memcpy(&cur, a, 8);
            if (cur == want) return Write::Same;
            std::memcpy(a, &want, 8);
            return finish(true);
        }
        case FieldKind::Entity: {
            scene::Entity target = scene::kInvalidEntity;
            if (f.i >= 0 && lookup) target = lookup(f.s);
            const u32 want = static_cast<u32>(target);
            u32 cur; std::memcpy(&cur, a, 4);
            if (cur == want) return Write::Same;
            std::memcpy(a, &want, 4);
            return finish(true);
        }
        case FieldKind::String: {
            const char* cur = aver_scene_get_str(static_cast<i32>(e), static_cast<i32>(fieldId));
            if (f.s == (cur ? cur : "")) return Write::Same;
            if (aver_scene_set_str(static_cast<i32>(e), static_cast<i32>(fieldId), f.s.c_str()) == 0)
                return Write::Failed;
            return Write::Changed;
        }
    }
    return Write::Failed;
}

bool readComponent(const scene::World& w, scene::Entity e, u32 type, const PathLookup& lookup,
                   fmt::OcSaveComponent& out) {
    if (!w.hasComponent(e, type)) return false;
    out = fmt::OcSaveComponent{};
    out.type = w.componentName(type);
    const u32 n = w.fieldCount(type);
    for (u32 i = 0; i < n; ++i) {
        const u32 id = w.fieldAt(type, i);
        const scene::FieldDesc* d = w.field(id);
        if (!d || d->readOnly || ignoredField(out.type, d->name)) continue;
        fmt::OcSaveField f;
        if (readField(w, e, id, lookup, f)) out.fields.push_back(std::move(f));
    }
    return true;
}

} // namespace aver::prefab::detail
