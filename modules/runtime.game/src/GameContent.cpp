#include "aver/game/GameContent.hpp"

#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>

// UNCONDITIONAL, and it was inside the scene guard below. AssetType/assetTypeFromPath live in
// modules/assets -- a leaf with no module switch at all, always linked through Aver.Formats -- and
// they have TWO callers here: loadProjectMeshes(), which is scene-guarded, and
// loadProjectMaterials(), which is PBR-guarded and has nothing to do with the scene. Scoping the
// include to one of the two guards left the other branch without the type.
#include "aver/assets/AssetId.hpp"

#if AVER_MODULE_PBR
#  include "aver/assets/TextureUpload.hpp"
#  include "aver/formats/OcMat.hpp"
#endif

#if AVER_MODULE_SCENE
#  include "aver/anim/AnimSystem.hpp"
#  include "aver/formats/OcMesh.hpp"
#  include "aver/scene/scene_abi.h"
#  include "GameMath.hpp"
#endif

namespace aver::game {

void GameContent::adopt(const fmt::ProjectDesc& project) {
    project_ = project;
    contentIndex_.clear();

    const std::string content = project_.contentDir();
    if (content.empty()) return;

    std::error_code ec;
    if (!std::filesystem::exists(content, ec)) {
        AVER_WARN("[Content] the project's content root does not exist: {}", content);
        return;
    }

    // The error_code overload of increment, so an unreadable subdirectory ends the walk instead of
    // throwing out of it.
    for (std::filesystem::recursive_directory_iterator it(content, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::string rel = std::filesystem::relative(it->path(), content, ec).string();
        if (ec || rel.empty()) continue;
        // FROZEN: the id hashes the forward-slash spelling, matching C# Assets.ObjectIdOf. This is
        // the property that makes packaging nearly free -- stage-game.ps1 copies Content/ under a
        // new root and every id in every .ocworld and .ocmat is unchanged, because the ids were
        // never a function of where the project lives.
        for (char& c : rel) if (c == '\\') c = '/';
        contentIndex_[fnv1a64(std::string_view(rel))] = it->path().string();
    }
    AVER_INFO("[Content] indexed {} asset(s) under {}", contentIndex_.size(), content);

#if AVER_MODULE_SCENE
    // The anim system does its own file discovery through this and caches by id, so a re-index has
    // to drop what it cached or a moved asset keeps resolving to its old path. Safe because adopt()
    // is a project-adoption call and never a per-frame one.
    //
    // Guarded on SCENE rather than PBR, which is where SandboxApp has it: the animation system has
    // nothing to do with physically based rendering, and the original guard is the cross-
    // contamination this lift exists to stop copying forward.
    anim::animSystem().clear();
    anim::animSystem().setResolver(&GameContent::resolveAnimAsset, this);
#endif
}

std::string GameContent::pathFor(u64 id) const {
    const auto it = contentIndex_.find(id);
    return it == contentIndex_.end() ? std::string() : it->second;
}

std::vector<std::string> GameContent::pathsWithExtension(std::string_view ext) const {
    std::vector<std::string> out;
    for (const auto& [id, path] : contentIndex_) {
        if (path.size() < ext.size()) continue;
        // Case-insensitive suffix compare, ASCII only -- matches isOcproject's own reasoning in
        // GameApp.cpp (a project's asset extensions are all plain ASCII, and Windows paths are
        // case-insensitive on disk but not in a plain string compare).
        bool match = true;
        for (usize i = 0; i < ext.size(); ++i) {
            char a = path[path.size() - ext.size() + i];
            char b = ext[i];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
            if (a != b) { match = false; break; }
        }
        if (match) out.push_back(path);
    }
    // contentIndex_ is an unordered_map: iteration order is not the walk order, and is not even
    // stable between two runs of the SAME binary over the SAME content. A caller that assigns
    // anything by position (GameApp's synthetic entity ids, notably) would otherwise get a
    // reproducibility gap that looks like a bug in whatever the ids are used for.
    std::sort(out.begin(), out.end());
    return out;
}

std::string GameContent::resolveAnimAsset(u64 id, void* user) {
    auto* self = static_cast<GameContent*>(user);
    return self ? self->pathFor(id) : std::string();
}

#if AVER_MODULE_SCENE

void GameContent::registerBuiltins(rhi::IDevice& device) {
    // FROZEN: the unit cube stays half-extent 1. A .ocworld PLACEG scale is a half-extent in
    // centimetres applied to this mesh, so changing it silently resizes every placed box in every
    // level ever authored. The same constant is frozen in SandboxApp.cpp with the same note.
    // BOUNDS ARE RECORDED FOR THE BUILT-INS, which SandboxApp does not do. Both are generated at
    // radius/half-extent 1, so the box is exactly known and costs nothing to write down.
    //
    // This is not tidiness. A CMeshRenderer whose bounds were never filled in presents a DEGENERATE
    // box, and the draw walk deliberately draws a degenerate box rather than culling it -- an entity
    // whose bounds are unknown must not vanish. The consequence in the editor is that every entity
    // using a built-in primitive is exempt from frustum culling entirely, including ones directly
    // behind the camera. Measured here: a five-placement level reported "5 drawn, 0 culled" from
    // every camera angle until these two lines existed.
    const std::pair<Vec3, Vec3> unitBounds{Vec3{-1.0f, -1.0f, -1.0f}, Vec3{1.0f, 1.0f, 1.0f}};
    {
        std::vector<rhi::MeshVertex> v; std::vector<u32> i;
        appendSphere(v, i, 1.0f, 24, 48);
        const u64 id = fnv1a64(std::string_view("Meshes/sphere.ocmesh"));
        sceneMeshes_[id] = device.createMesh(v.data(), (u32)v.size(), i.data(), (u32)i.size());
        meshBounds_[id]  = unitBounds;
    }
    {
        std::vector<rhi::MeshVertex> v; std::vector<u32> i;
        appendBox(v, i, 0, 0, 0, 1.0f);
        const u64 id = fnv1a64(std::string_view("Meshes/cube.ocmesh"));
        sceneMeshes_[id] = device.createMesh(v.data(), (u32)v.size(), i.data(), (u32)i.size());
        meshBounds_[id]  = unitBounds;
    }

    // The named surfaces gameplay can ask for, with the editor's exact values.
    auto look = [this](const char* name, f32 r, f32 g, f32 b, f32 metal, f32 rough) {
        surfaceLooks_[aver_scene_material(0, name)] = SurfaceLook{{r, g, b}, metal, rough};
    };
    look("M_Floor",  0.22f, 0.23f, 0.26f, 0.02f, 0.85f);
    look("M_Wall",   0.48f, 0.50f, 0.55f, 0.03f, 0.72f);
    look("M_Trim",   0.30f, 0.33f, 0.38f, 0.35f, 0.45f);
    look("M_Crate",  0.62f, 0.44f, 0.22f, 0.02f, 0.78f);
    look("M_Target", 0.86f, 0.20f, 0.16f, 0.05f, 0.40f);
    look("M_Metal",  0.55f, 0.57f, 0.60f, 0.85f, 0.28f);
    look("M_Accent", 0.95f, 0.66f, 0.15f, 0.30f, 0.35f);

    AVER_INFO("[Mesh] {} built-in primitive(s), {} named surface(s)", sceneMeshes_.size(), surfaceLooks_.size());
}

void GameContent::loadProjectMeshes(rhi::IDevice& device) {
    const std::string dir = project_.contentDir();
    if (dir.empty()) return;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return;

    u32 loaded = 0, failed = 0;
    for (std::filesystem::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string full = it->path().string();
        if (assetTypeFromPath(full) != AssetType::Mesh) continue;

        std::string rel = std::filesystem::relative(it->path(), dir, ec).string();
        if (ec) continue;
        for (char& c : rel) if (c == '\\') c = '/';

        fmt::OcMeshData md;
        std::string why;
        if (!fmt::loadOcMesh(full, md, &why)) { AVER_WARN("[Mesh] {}", why); ++failed; continue; }

        // Position, normal and uv ONLY. rhi::MeshVertex is 32 bytes and has nowhere to put joints
        // or weights, so a skinned asset arrives here as static geometry -- correct, because the
        // skinning path uploads its own target mesh and resolves through resolveSceneMesh.
        std::vector<rhi::MeshVertex> verts(md.vertexCount());
        for (u32 i = 0; i < md.vertexCount(); ++i) {
            rhi::MeshVertex& v = verts[i];
            v.px = md.positions[usize(i)*3+0]; v.py = md.positions[usize(i)*3+1]; v.pz = md.positions[usize(i)*3+2];
            v.nx = md.normals[usize(i)*3+0];   v.ny = md.normals[usize(i)*3+1];   v.nz = md.normals[usize(i)*3+2];
            v.u  = md.uvs[usize(i)*2+0];       v.v  = md.uvs[usize(i)*2+1];
        }
        const rhi::MeshHandle h = device.createMesh(verts.data(), (u32)verts.size(),
                                                   md.indices.data(), (u32)md.indices.size());
        if (!h) { AVER_WARN("[Mesh] the device refused '{}'", rel); ++failed; continue; }

        const u64 id = fnv1a64(std::string_view(rel));
        sceneMeshes_[id] = h;
        meshBounds_[id] = {md.boundsMin, md.boundsMax};
        projectMeshIds_.push_back(id);
        ++loaded;
    }
    if (loaded || failed)
        AVER_INFO("[Mesh] {} project mesh(es) loaded from {}{}", loaded, dir,
                  failed ? (", " + std::to_string(failed) + " failed") : "");
}

void GameContent::releaseProjectMeshes() {
    for (const u64 id : projectMeshIds_) { sceneMeshes_.erase(id); meshBounds_.erase(id); }
    projectMeshIds_.clear();
}

rhi::MeshHandle GameContent::meshFor(u64 id) const {
    const auto it = sceneMeshes_.find(id);
    return it == sceneMeshes_.end() ? 0 : it->second;
}

const std::pair<Vec3, Vec3>* GameContent::boundsFor(u64 id) const {
    const auto it = meshBounds_.find(id);
    return it == meshBounds_.end() ? nullptr : &it->second;
}

rhi::MeshHandle GameContent::resolveSceneMesh(u64 id, void* user) {
    auto* self = static_cast<GameContent*>(user);
    return self ? self->meshFor(id) : 0;
}

const GameContent::SurfaceLook* GameContent::lookFor(i32 material) const {
    const auto it = surfaceLooks_.find(material);
    return it == surfaceLooks_.end() ? nullptr : &it->second;
}

#endif // AVER_MODULE_SCENE

#if AVER_MODULE_PBR

std::string GameContent::resolveAssetPath(const pbr::TextureRef& ref) const {
    if (!ref.path.empty()) {
        const std::string& p = ref.path;
        const bool absolute = p.size() > 1 && (p[1] == ':' || p[0] == '\\' || p[0] == '/');
        if (absolute) return p;
        const std::string content = project_.contentDir();
        if (!content.empty()) {
            const std::string full = content + "\\" + p;
            std::error_code ec;
            if (std::filesystem::exists(full, ec)) return full;
        }
        return p;
    }
    if (ref.id) return pathFor(ref.id);
    return {};
}

rhi::TextureHandle GameContent::resolveMaterialTexture(const pbr::TextureRef& ref, pbr::TextureSlot slot,
                                                       void* user) {
    auto* self = static_cast<GameContent*>(user);
    if (!self || !self->textureFactory_) return 0;

    const std::string path = self->resolveAssetPath(ref);
    if (path.empty()) {
        AVER_WARN("[Material] texture id 0x{:016X} is not in the content index; slot '{}' keeps its fallback",
                  ref.id, pbr::MaterialLibrary::textureSlotName(slot));
        return 0;
    }

    // THE SLOT DECIDES THE COLOUR SPACE, NEVER THE FILENAME. A normal map read as sRGB is a subtly
    // wrong lighting response that looks like a shading bug rather than a decode bug.
    assets::TextureUsage usage = assets::TextureUsage::Data;
    switch (slot) {
        case pbr::TextureSlot::BaseColor:
        case pbr::TextureSlot::Emissive:  usage = assets::TextureUsage::Colour;    break;
        case pbr::TextureSlot::Normal:    usage = assets::TextureUsage::NormalMap; break;
        default:                          usage = assets::TextureUsage::Data;      break;
    }

    std::string err;
    assets::TextureUploadInfo info;
    const rhi::TextureHandle h = assets::uploadTexture(*self->textureFactory_, path, usage, &err, &info);
    if (!h) {
        AVER_WARN("[Material] {} - slot '{}' keeps its fallback", err,
                  pbr::MaterialLibrary::textureSlotName(slot));
        return 0;
    }
    AVER_INFO("[Material] {} -> {}x{}, {} mips ({} KB) for slot '{}'", path, info.width, info.height,
              info.mips, info.bytes / 1024, pbr::MaterialLibrary::textureSlotName(slot));
    return h;
}

pbr::MaterialHandle GameContent::materialForSurface(const std::string& name) {
    if (name.empty()) return 0;
    const auto cached = materialAssets_.find(name);
    if (cached != materialAssets_.end()) return cached->second;

    pbr::MaterialHandle h = 0;
    const std::string content = project_.contentDir();
    if (!content.empty()) {
        // ORDER MATTERS: a BUILT .ocmat under Binaries wins over a hand-authored one under Content,
        // because the built one is what avermatc produced from the C# source and is therefore the
        // one the ids in the level refer to.
        const std::string candidates[3] = {
            project_.binariesDir() + "\\Materials\\" + name + ".ocmat",
            content + "\\Materials\\" + name + ".ocmat",
            content + "\\" + name,
        };
        for (const std::string& path : candidates) {
            std::error_code ec;
            if (!std::filesystem::exists(path, ec)) continue;
            pbr::MaterialDesc d;
            fmt::OcMatExtras extras;
            std::string err;
            // A parse failure BREAKS rather than falling through to the next candidate: a corrupt
            // built material must not be silently replaced by a stale hand-authored one.
            if (!fmt::loadOcmat(path, d, &extras, &err)) { AVER_WARN("[Material] {}", err); break; }
            h = pbr::MaterialLibrary::get().create(d);
            if (h) AVER_INFO("[Material] '{}' loaded from {}", d.name, path);
            break;
        }
    }
    // Caches 0 as a negative result and never retries. Deliberate: a project with fifty unauthored
    // surfaces would otherwise stat three paths per surface per level load.
    materialAssets_.emplace(name, h);
    return h;
}

void GameContent::loadProjectMaterials() {
    const std::string dir = project_.contentDir();
    if (dir.empty()) return;
    const std::string matDir = dir + "\\Materials";
    std::error_code ec;
    if (!std::filesystem::exists(matDir, ec)) return;

    u32 n = 0;
    // NON-RECURSIVE, matching the editor: Content\Materials only, not every .ocmat in the tree.
    for (std::filesystem::directory_iterator it(matDir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        if (assetTypeFromPath(it->path().string()) != AssetType::Material) continue;
        if (materialForSurface(it->path().stem().string())) ++n;
    }
    if (n) AVER_INFO("[Material] {} project material(s) loaded from {}", n, matDir);
}

void GameContent::releaseProjectMaterials() {
    materialAssets_.clear();
#if AVER_MODULE_SCENE
    surfaceMaterials_.clear();
#endif
}

#endif // AVER_MODULE_PBR

#if AVER_MODULE_PBR && AVER_MODULE_SCENE
pbr::MaterialHandle GameContent::authoredFor(i32 token) const {
    const auto it = surfaceMaterials_.find(token);
    return it == surfaceMaterials_.end() ? 0 : it->second;
}
#endif

} // namespace aver::game
