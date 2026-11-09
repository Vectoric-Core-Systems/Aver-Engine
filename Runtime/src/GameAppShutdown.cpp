#include "GameAppInternal.hpp"

namespace aver::game {

void GameApp::detachSubViews() {
#if AVER_MODULE_SCENE && AVER_MODULE_FRAMEWORK
    // The views' features were registered after everything below, and their textures are named by the UI and by materials: out first.
    subViews_.detach();
#endif
}

void GameApp::detachCrowdGpu(rhi::IDevice* dev) {
#if AVER_WITH_SYNAPSE_AI && AVER_MODULE_SYNAPSE_GPU
    if (crowdGpuAttached_) { game::removeCrowdGpu(dev, crowdGpu_); crowdGpuAttached_ = false; }
#else
    (void)dev;
#endif
}

void GameApp::detachUpscalers(rhi::IDevice* dev) {
    // Detach before destroy: device holds raw pointer into averSrUpscaler_. Both calls no-op when never installed.
    if (dev) dev->setUpscaler(nullptr);
    averSrUpscaler_.reset();
    // Frame generator: same detach-then-destroy order.
    if (dev) { dev->setFrameInterpolation(false); dev->setFrameInterpolator(nullptr); }
    frameInterpolator_.reset();
    if (dev && pcgAttached_) { dev->removeRenderFeature(&pcgVolume_); pcgAttached_ = false; }
    pcgVolume_.shutdown();
}

void GameApp::shutdownParticles(rhi::IDevice* dev) {
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    // Registered last, so removed first.
    if (dev && particlesAttached_) {
        dev->removeRenderFeature(&particleRenderer_);
        particlesAttached_ = false;
    }
    content_.releaseParticleTextures();
    particleRenderer_.shutdown();
#else
    (void)dev;
#endif
}

void GameApp::shutdownGameUi(rhi::IDevice* dev) {
#if AVER_WITH_UI_ABI
    // Font out first: ui_abi.h comment on aver_ui_set_font: a font outliving its atlas textures draws glyphs sampling dead descriptor.
    aver_ui_set_font(nullptr);
    game::scene2dUnbind();      // the sprite resolver names UI images
    game::uiImagesShutdown();   // the UI images (their textures and bindings) before the renderer and device go
    game::uiTextShutdown();   // unbind the atlas from the renderer and close the fonts before the device goes
    if (dev && uiFontTexture_) {
        // waitIdle: destroying a texture a still-in-flight command list references is GPU use-after-free.
        if (rhi::IResourceFactory* res = dev->resources()) {
            res->waitIdle();
            res->destroyTexture(uiFontTexture_);
        }
        uiFontTexture_ = 0;
    }
    // Registered after Voxi, before particles; removed after particles, before Voxi.
    if (dev && gameUi_) {
        dev->removeRenderFeature(gameUi_);
        delete gameUi_;
        gameUi_ = nullptr;
    }
#else
    (void)dev;
#endif
}

void GameApp::removeSceneFeatures(rhi::IDevice* dev) {
    (void)dev;
#if AVER_MODULE_SCENE
    // Reverse order: skinning first, last to come out. Removed before device.
#if AVER_MODULE_RENDER_SOFTBODY
#if AVER_MODULE_RENDER_HAIR
    if (dev && hairScene_) dev->removeRenderFeature(hairScene_.get());   // registered after the soft bodies, removed before them
#endif
    if (dev && softBodyScene_) dev->removeRenderFeature(softBodyScene_.get());   // registered after skinning, removed before it
#endif
    if (dev && skinnedScene_) dev->removeRenderFeature(skinnedScene_.get());
#endif
}

void GameApp::shutdownVoxi(rhi::IDevice* dev) {
    (void)dev;
#if AVER_MODULE_VOXI
    if (dev && voxiAttached_) {
        dev->removeRenderFeature(&voxiRenderer_);
        voxiAttached_ = false;
    }
    voxiRenderer_.shutdown();
#endif
}

void GameApp::releaseContentMaterials() {
#if AVER_MODULE_PBR
    // After Voxi teardown: material system lives inside VoxiRenderer. Dropping materials while registered would leave freed textures.
    content_.releaseProjectMaterials();
    content_.setTextureFactory(nullptr);
    content_.setMaterialSystem(nullptr);
#endif
}

void GameApp::shutdownSceneState(rhi::IDevice* dev) {
    (void)dev;
#if AVER_MODULE_SCENE
#if AVER_MODULE_RENDER_SOFTBODY
#if AVER_MODULE_RENDER_HAIR
    hairScene_.reset();
#endif
    softBodyScene_.reset();
#endif
    skinnedScene_.reset();
    // Streamed chunks own entities and static bodies. Released here while physics exists (unlike editor, which waits for destructors).
    streaming_.disable();
    // Before physics: unloading destroys entities and removes static bodies. Object animation stops first so nothing moves as level comes down.
    anim::animSystem().setObjectAnimationLive(false);
    playMobility_.end();
#if AVER_MODULE_PHYSICS
    game::clearVehicles();   // CVehicle vehicles: bodies, constraints, voices (before the level, like the traffic cars)
    // Cars' bodies and constraints go before the level.
    vehicles_.end();
#endif
    level_.unload();
#endif
}

void GameApp::shutdownWater(rhi::IDevice* dev) {
    (void)dev;
#if AVER_MODULE_FLUIDS
    // After level's volumes were despawned, before physics: retiring live volume calls into solver.
    water_.shutdown(dev);
#endif
}

void GameApp::shutdownPhysics() {
#if AVER_MODULE_PHYSICS
#if AVER_MODULE_RAGDOLL
    ragdoll::ragdollSystem().clear();
#endif
    aver_phys_shutdown();
#endif
}

void GameApp::shutdownAudio() {
#if AVER_WITH_AUDIO_ABI
    // Stops mixer, releases device, forgets every sound. Idempotent. 0 is not an error (no output device).
    aver_audio_shutdown();
#endif
}

void GameApp::shutdownScripting() {
#if AVER_MODULE_SCRIPTING
    // Drains graphs/behaviors and closes CLR host. Ordering against physics/scene shutdown not load-bearing.
    if (scriptsReady_) { scripts_.shutdown(); scriptsReady_ = false; }
#endif
}


} // namespace aver::game
