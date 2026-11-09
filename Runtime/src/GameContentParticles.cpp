#include "GameContentInternal.hpp"

namespace aver::game {

#if AVER_MODULE_PARTICLES
void GameContent::loadProjectParticleEffects() {
    // Clear process-global table on reload.
    particles::particleEffects().clear();
    const std::string dir = project_.contentDir();
    if (dir.empty()) return;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return;

    u32 loaded = 0, failed = 0;
    for (std::filesystem::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string full = it->path().string();
        if (assetTypeFromPath(full) != AssetType::Particle) continue;

        std::string rel = std::filesystem::relative(it->path(), dir, ec).string();
        if (ec) continue;
        for (char& c : rel) if (c == '\\') c = '/';

        particles::ParticleSystemDef def;
        std::string err;
        if (!fmt::loadOcparticleSystem(full, def, nullptr, &err)) {
            AVER_WARN("[Particles] {}", err);
            ++failed;
            continue;
        }

        // Effect id is fnv1a64(relative path), matches CParticleEmitter::effect. A v1 file arrives as a one-emitter system.
        particles::particleEffects().setSystem(fnv1a64(std::string_view(rel)), def);
        // An emitter that names a custom particles program needs it in the registry before the first emitter instance asks the GPU tier for it.
#if AVER_MODULE_FRAMEWORK
        for (const particles::ParticleEffect& em : def.emitters) {
            if (em.customShader.empty()) continue;
            std::ifstream sf(std::filesystem::path(dir) / "Shaders" / (em.customShader + ".particles.ocgraph"), std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(sf)), std::istreambuf_iterator<char>());
            if (text.empty() || aver_fw_cenv_register(AVER_FW_CENV_PARTICLES, em.customShader.c_str(), text.c_str()) == 0)
                AVER_WARN("[Particles] effect {}: the custom particles shader '{}' could not be registered from Shaders/{}.particles.ocgraph; the emitter simulates without it", rel, em.customShader, em.customShader);
        }
#endif
        ++loaded;
    }
    if (loaded || failed)
        AVER_INFO("[Particles] {} project effect(s) loaded from {}{}", loaded, dir,
                  failed ? (", " + std::to_string(failed) + " failed") : "");
}

rhi::TextureHandle GameContent::resolveParticleTexture(u64 textureId, void* user) {
#if AVER_MODULE_PBR
    auto* self = static_cast<GameContent*>(user);
    if (!self || !self->textureFactory_ || textureId == 0) return 0;
    const auto it = self->particleTextures_.find(textureId);
    if (it != self->particleTextures_.end()) return it->second;
    rhi::TextureHandle h = 0;
    const std::string path = self->pathFor(textureId);
    if (path.empty()) {
        AVER_WARN("[Particles] texture id 0x{:016X} is not in the content index; the effect draws the procedural dot", textureId);
    } else {
        std::string err;
        h = assets::uploadTexture(*self->textureFactory_, path, assets::TextureUsage::Colour, &err, nullptr);
        if (!h) AVER_WARN("[Particles] {} - the effect draws the procedural dot", err);
    }
    self->particleTextures_[textureId] = h;   // a failure is cached too, so it is logged once
    return h;
#else
    (void)textureId; (void)user;
    return 0;
#endif
}

bool GameContent::resolveParticleMesh(u64 meshId, particles::ParticleMeshData& out, void* user) {
#if AVER_MODULE_SCENE
    auto* self = static_cast<GameContent*>(user);
    if (!self || meshId == 0) return false;
    const std::string path = self->pathFor(meshId);
    if (path.empty()) {
        // The two built-in primitives (registerBuiltins) have no file; a particle may still name them. Coarser than the scene's
        // own sphere: a mesh particle is a few hundred triangles at most (ParticleMeshCache::kMaxTriangles).
        std::vector<rhi::MeshVertex> v;
        std::vector<u32> idx;
        if (meshId == fnv1a64(std::string_view("Meshes/cube.ocmesh"))) appendBox(v, idx, 0, 0, 0, 1.0f);
        else if (meshId == fnv1a64(std::string_view("Meshes/sphere.ocmesh"))) appendSphere(v, idx, 1.0f, 12, 20);
        else return false;
        out.positions.clear(); out.normals.clear(); out.uvs.clear();
        for (const rhi::MeshVertex& mv : v) {
            out.positions.insert(out.positions.end(), {mv.px, mv.py, mv.pz});
            out.normals.insert(out.normals.end(), {mv.nx, mv.ny, mv.nz});
            out.uvs.insert(out.uvs.end(), {mv.u, mv.v});
        }
        out.indices = std::move(idx);
        return !out.indices.empty();
    }
    fmt::OcMeshData md;
    std::string why;
    if (!fmt::loadOcMesh(path, md, &why)) { AVER_WARN("[Particles] {}", why); return false; }
    out.positions = std::move(md.positions);
    out.normals = std::move(md.normals);
    out.uvs = std::move(md.uvs);
    out.indices = std::move(md.indices);
    return !out.indices.empty() && !out.positions.empty();
#else
    (void)meshId; (void)out; (void)user;
    return false;
#endif
}

void GameContent::releaseParticleTextures() {
#if AVER_MODULE_PBR
    if (textureFactory_)
        for (const auto& kv : particleTextures_)
            if (kv.second) textureFactory_->destroyTexture(kv.second);
    particleTextures_.clear();
#endif
}
#endif // AVER_MODULE_PARTICLES

} // namespace aver::game
