#include "GameAppInternal.hpp"

namespace aver::game {

void GameApp::attachPcgTest(Engine& e) {
    rhi::IDevice* dev = e.device();
    if (!dev) return;
    if (!pcgVolume_.init(*dev)) {
        AVER_ERROR("[PCG] the volume shader would not compile - the HLSL and the C++ mirror cannot "
                   "be compared, so this run proves nothing");
        return;
    }
    dev->addRenderFeature(&pcgVolume_);
    pcgAttached_ = true;

    // Match PcgMirrorTest spec for C++/F#/HLSL parity
    pcgSpec_.resX = pcgSpec_.resY = pcgSpec_.resZ = 32;
    pcgSpec_.seed = 20260802;
    pcgSpec_.layerCount = 2;
    pcgSpec_.layers[0] = { 4.0f,  1.0f, 4, 2.0f, 0.5f, 7919 };
    pcgSpec_.layers[1] = { 11.0f, 0.5f, 3, 2.0f, 0.5f, 104729 };
    pcgSpec_.coverageFloor = 0.35f;
    pcgSpec_.coverageBias  = 1.7f;
    if (!pcgVolume_.request(pcgSpec_)) AVER_ERROR("[PCG] could not queue the volume build");
}

bool GameApp::checkPcgVolume() {
    if (!pcgAttached_ || pcgChecked_ || !pcgVolume_.done()) return false;
    pcgChecked_ = true;

    const usize n = usize(pcgSpec_.resX) * pcgSpec_.resY * pcgSpec_.resZ;
    std::vector<f32> gpu(n, 0.0f);
    if (!pcgVolume_.read(gpu.data(), n)) { AVER_ERROR("[PCG] could not read the volume back"); return true; }

    // Compare all voxels against CPU reference. Tolerance 1e-5 (above contraction, below logic errors).
    constexpr f32 kTolerance = 1e-5f;
    usize differing = 0;
    usize exact = 0;
    f32 worst = 0.0f;
    usize worstAt = 0;
    for (u32 z = 0; z < pcgSpec_.resZ; ++z)
        for (u32 y = 0; y < pcgSpec_.resY; ++y)
            for (u32 x = 0; x < pcgSpec_.resX; ++x) {
                const usize i = usize(z) * pcgSpec_.resY * pcgSpec_.resX + usize(y) * pcgSpec_.resX + x;
                const f32 cpu = pcg::sampleDensity(pcgSpec_, x, y, z);
                const f32 d = std::fabs(cpu - gpu[i]);
                if (d == 0.0f) ++exact;
                if (d > kTolerance) { ++differing; }
                if (d > worst) { worst = d; worstAt = i; }
            }

    if (differing == 0) {
        AVER_INFO("[PCG] volume mirror PASS: {} of {} voxels agree with the C++ reference to within "
                  "{}, of which {} are bit-identical; worst difference {} at index {}",
                  n, n, kTolerance, exact, worst, worstAt);
    } else {
        AVER_ERROR("[PCG] volume mirror FAIL: {} of {} voxels differ by more than {}, worst {} at "
                   "index {} - that is far above float contraction and means the HLSL and the C++ "
                   "reference have genuinely diverged", differing, n, kTolerance, worst, worstAt);
    }
    return true;
}

#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
void GameApp::spawnParticleTestContent(rhi::IDevice& device) {
    // Standalone particle test (no project required)
    const u64 cubeId = fnv1a64(std::string_view("Meshes/cube.ocmesh"));
    if (!content_.meshFor(cubeId)) content_.registerBuiltins(device);

    scene::World& world = scene::World::instance();

    // Match SandboxApp.cpp --particle-test for parity
    const Vec3 cubePos{600.0f, 0.0f, 50.0f};
    {
        Transform xf;
        xf.position = cubePos;
        xf.scale = Vec3{80.0f, 80.0f, 80.0f};
        const scene::Entity occ = world.create("ParticleTest.Occluder", scene::kInvalidEntity, xf);
        if (auto* mr = static_cast<scene::CMeshRenderer*>(
                world.addComponent(occ, scene::kComponentMeshRenderer))) {
            mr->mesh = cubeId;
            mr->flags |= scene::kMeshRendererVisible;
            mr->aabbMin[0] = mr->aabbMin[1] = mr->aabbMin[2] = -1.0f;
            mr->aabbMax[0] = mr->aabbMax[1] = mr->aabbMax[2] =  1.0f;
            // Crate-brown material
            mr->material = aver_scene_material(0, "M_Crate");
        }
    }

    particles::ParticleEffect fx;
    fx.shape = particles::EmitterShape::Box;
    fx.shapeSize = Vec3{350.0f, 60.0f, 90.0f};
    fx.emissionRate = 150.0f;
    fx.burstCount = 0;
    fx.maxParticles = 400;
    fx.lifetimeMin = 3.0f;
    fx.lifetimeMax = 5.0f;
    fx.direction = Vec3{0.0f, 0.0f, 1.0f};
    fx.spreadDeg = 60.0f;
    fx.speedMin = 5.0f;
    fx.speedMax = 15.0f;
    fx.gravity = Vec3{0.0f, 0.0f, -5.0f};
    fx.damping = 0.3f;
    fx.sizeStart = 25.0f;
    fx.sizeEnd = 45.0f;
    fx.colorStart[0] = 0.75f; fx.colorStart[1] = 0.70f; fx.colorStart[2] = 0.62f; fx.colorStart[3] = 0.35f;
    fx.colorEnd[0]   = 0.75f; fx.colorEnd[1]   = 0.70f; fx.colorEnd[2]   = 0.62f; fx.colorEnd[3]   = 0.0f;
    fx.blend = rhi::BlendMode::PremultipliedAlpha;
    constexpr u64 kDustCloudEffectId = 0x50415254'44550001ull;   // arbitrary, non-zero
    particles::particleEffects().set(kDustCloudEffectId, fx);

    {
        Transform xf;
        xf.position = cubePos;
        const scene::Entity emitter = world.create("ParticleTest.DustCloud", scene::kInvalidEntity, xf);
        if (auto* c = static_cast<scene::CParticleEmitter*>(
                world.addComponent(emitter, scene::kComponentParticleEmitter))) {
            c->effect = kDustCloudEffectId;
        }
        AVER_INFO("[Game] --particle-test: dust cloud entity {} around effect 0x{:016X}",
                  emitter, kDustCloudEffectId);
    }

    // Additive embers, no GI, separate from dust cloud for visual clarity
    particles::ParticleEffect emberFx;
    emberFx.shape = particles::EmitterShape::Sphere;
    emberFx.shapeSize = Vec3{10.0f, 0.0f, 0.0f};
    emberFx.emissionRate = 80.0f;
    emberFx.burstCount = 0;
    emberFx.maxParticles = 200;
    emberFx.lifetimeMin = 1.0f;
    emberFx.lifetimeMax = 1.6f;
    emberFx.direction = Vec3{0.0f, 0.0f, 1.0f};
    emberFx.spreadDeg = 35.0f;
    emberFx.speedMin = 40.0f;
    emberFx.speedMax = 80.0f;
    emberFx.gravity = Vec3{0.0f, 0.0f, -25.0f};
    emberFx.damping = 0.1f;
    emberFx.sizeStart = 22.0f;
    emberFx.sizeEnd = 6.0f;
    emberFx.colorStart[0] = 1.0f; emberFx.colorStart[1] = 0.55f; emberFx.colorStart[2] = 0.12f; emberFx.colorStart[3] = 1.0f;
    emberFx.colorEnd[0]   = 1.0f; emberFx.colorEnd[1]   = 0.15f; emberFx.colorEnd[2]   = 0.02f; emberFx.colorEnd[3] = 0.0f;
    emberFx.blend = rhi::BlendMode::Additive;
    emberFx.receivesGI = false;   // ember is its own light source
    constexpr u64 kEmberEffectId = 0x50415254'45420001ull;   // arbitrary, non-zero
    particles::particleEffects().set(kEmberEffectId, emberFx);

    {
        Transform xf;
        xf.position = cubePos + Vec3{0.0f, 0.0f, 220.0f};
        const scene::Entity emberEmitter = world.create("ParticleTest.Embers", scene::kInvalidEntity, xf);
        if (auto* c = static_cast<scene::CParticleEmitter*>(
                world.addComponent(emberEmitter, scene::kComponentParticleEmitter))) {
            c->effect = kEmberEffectId;
        }
        AVER_INFO("[Game] --particle-test: ember entity {} around effect 0x{:016X} (receivesGI=false)",
                  emberEmitter, kEmberEffectId);
    }

    // Match SandboxApp camera position for parity
    camPos_ = Vec3{0.0f, 0.0f, 50.0f};
    yaw_ = 0.0f;
    pitch_ = 0.0f;
    AVER_INFO("[Game] --particle-test: camera set to (0,0,50) looking down +X");
}
#endif

} // namespace aver::game
