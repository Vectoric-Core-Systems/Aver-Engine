#include "GameAppInternal.hpp"

namespace aver::game {

namespace {

#if AVER_MODULE_VOXI
// AverSR level names for logging
const char* averSrLevelName(u32 level) {
    switch (level) {
        case voxi::ladder::kAverSrOff:         return "Off";
        case voxi::ladder::kAverSrQuality:     return "Quality";
        case voxi::ladder::kAverSrBalanced:    return "Balanced";
        case voxi::ladder::kAverSrPerformance: return "Performance";
        default:                               return "Off";
    }
}

// AverSR source names for logging
const char* averSrSourceName(voxi::AverSrSource source) {
    switch (source) {
        case voxi::AverSrSource::Auto:      return "Auto";
        case voxi::AverSrSource::Manifest:  return "project default";
        case voxi::AverSrSource::User:      return "your Display preference";
        case voxi::AverSrSource::Cli:       return "--aversr";
        case voxi::AverSrSource::ForcedOff: return "forced Off after a failed launch";
    }
    return "Auto";
}
#endif

} // namespace

void GameApp::attachSkinning(Engine& e) {
#if AVER_MODULE_SCENE
    rhi::IDevice* dev = e.device();
    if (!dev) return;

    auto scene = std::make_unique<render::SkinnedScene>();
    // HLSL compiled at runtime; null on fail (entities draw at rest pose)
    if (!scene->init(*dev)) {
        AVER_WARN("[Game] skinning unavailable; skinned entities will draw at rest");
        return;
    }
    // Use same resolvers as the draw walk
    scene->setResolvers(&GameContent::resolveAnimAsset, &GameContent::resolveSceneMesh, &content_);
    dev->addRenderFeature(scene.get());
    skinnedScene_ = std::move(scene);
    AVER_INFO("[Game] skinning attached");
#if AVER_MODULE_RENDER_SOFTBODY
    // After skinning in the feature list; draw-only, reads softbody::softBodySystem().pieces().
    {
#if AVER_MODULE_RENDER_HAIR && AVER_MODULE_HAIR_SCENE
        {
            // The compute host must be installed BEFORE any groom is built; the scene feature is registered after skinning like SoftBodyScene.
            auto hs = std::make_unique<render::HairScene>();
            if (hs->init(*e.device())) {
                hair::hairSystem().installComputeHost(&hs->hostTable());
                hair::hairSystem().setPathResolver(&GameContent::resolveAnimAsset, &content_);
                hs->setMaterialBinder([](i32 token, pbr::MaterialHandle h, void* u) { static_cast<GameContent*>(u)->bindSurfaceMaterial(token, h); }, &content_);
                e.device()->addRenderFeature(hs.get());
                hairScene_ = std::move(hs);
            }
        }
#endif
        auto sb = std::make_unique<render::SoftBodyScene>();
        if (sb->init(*dev)) {
            sb->setResolvers(&GameContent::resolveSceneMesh, &GameContent::resolveAnimAsset, &content_);
            dev->addRenderFeature(sb.get());
            softBodyScene_ = std::move(sb);
            AVER_INFO("[Game] soft-body drawing attached");
        }
    }
#endif
#else
    (void)e;
#endif
}

void GameApp::attachVoxi(Engine& e) {
#if AVER_MODULE_VOXI
    rhi::IDevice* dev = e.device();
    if (!dev) return;

    // Hand the GPU's real capabilities to Voxi so its settings reflect this hardware.
    const rhi::DeviceCaps caps = dev->caps();
    voxi::DeviceInfo di;
    di.msaaMask = caps.msaaMask; di.maxMsaaSamples = caps.maxMsaaSamples;
    di.rayTracingTier = caps.rayTracingTier; di.computeShaders = caps.computeShaders;
    di.typedUavLoads = caps.typedUavLoads; di.conservativeRaster = caps.conservativeRaster;
    di.shaderModel = caps.shaderModel; di.meshShaderTier = caps.meshShaderTier;
    di.dxcAvailable = caps.dxcAvailable;
    // Denoiser needs G-buffer (D3D12 only)
    di.denoiserSupported = dev->caps().gBuffer;
    voxi::Renderer::get().setDeviceInfo(di);

    // Seed render settings before init() to set voxel volume size and layered BSDF
    int manifestMsaa = -1;
    {
        std::string manifestPath = cfg_.projectPath;
        if (manifestPath.empty()) {
            const std::string beside = executableDir() + AVER_PATH_SEP "Game.ocproject";
            if (fileExists(beside)) manifestPath = beside;
        }
        if (!manifestPath.empty()) {
            fmt::ProjectDesc seed;
            std::string seedErr;
            if (fmt::loadOcproject(manifestPath, seed, &seedErr) && seed.hasRenderSettings()) {
                manifestMsaa = seed.msaa;
                voxi::Renderer& vx = voxi::Renderer::get();
                voxi::Settings seeded = vx.settings();
                voxi::applyManifestTwoPhase(seed, seeded, [&]() {
                    vx.setSettings(seeded);
                    seeded = vx.settings();
                });
            }
        }
    }

    voxi::Settings s = voxi::Renderer::get().settings();
    // Device MSAA fallback only when manifest unstated
    if (manifestMsaa < 0) s.msaa = static_cast<voxi::Msaa>(dev->sampleCount());
    voxi::Renderer::get().setSettings(s);
    voxiRenderer_.setSettings(s);
    voxiRenderer_.setNrd2WeightsPaths(
        aver::userDataDir().empty() ? std::string() : aver::userDataDir() + AVER_PATH_SEP + render::denoise::kNrd2WeightsFileName,
        aver::executableDir() + AVER_PATH_SEP "data" AVER_PATH_SEP + render::denoise::kNrd2WeightsFileName);

    if (voxiRenderer_.init(*dev)) {
        dev->addRenderFeature(&voxiRenderer_);
        voxiAttached_ = true;
#if AVER_MODULE_PBR
        // Keep adjacent: factory and resolver installed together
        content_.setTextureFactory(dev->resources());
        content_.setMaterialSystem(&voxiRenderer_.materials());
        // Phased residency: no frame waits on a decode. A bounded run (--frames) keeps the blocking loads so its frame N is the finished picture.
        if (GameContent::asyncTexturesWanted(cfg_.maxFrames != 0)) {
            content_.enableAsyncTextures(true);
            voxiRenderer_.materials().setAsyncTextureResolver(&GameContent::resolveMaterialTextureAsync, &content_);
        } else {
            voxiRenderer_.materials().setTextureResolver(&GameContent::resolveMaterialTexture, &content_);
        }
        voxiRenderer_.materials().setTerrainResolver(&GameContent::resolveTerrainAtlas, &content_);   // terrain layers
#endif
        // Depth proxy for LOD-based shadow/voxel optimization
        voxiRenderer_.setDepthProxy(&GameApp::depthProxyLookup, this);

        AVER_INFO("[Game] Voxi attached: MSAA {}x, RT tier {}, SM {}, mesh tier {}",
                  caps.maxMsaaSamples, caps.rayTracingTier, caps.shaderModel, caps.meshShaderTier);
    } else {
        AVER_WARN("[Game] Voxi failed to initialise; the world draws untextured");
    }
#else
    (void)e;
#endif
}

#if AVER_WITH_UI_ABI
// HUD render feature (overlay pass)
void GameApp::attachGameUi(Engine& e) {
    rhi::IDevice* dev = e.device();
    if (!dev) return;

    gameUi_ = render::ui::UiRenderer::create(*dev);
    if (gameUi_) {
        dev->addRenderFeature(gameUi_);
        game::uiImagesBind(dev, gameUi_);   // UI images by content path (the Content folder is given with the project)
        game::scene2dBind([this](u64 id) { return game::scene2dRelativePath(content_.pathFor(id), project_.contentDir()); });   // 2D sprites
        AVER_INFO("[Game] game UI attached");
    } else {
        AVER_WARN("[Game] game UI render feature failed to create -- the HUD will not draw");
    }
}

// Load font for game UI (non-fatal on all failures)
void GameApp::loadGameUiFont(Engine& e) {
    rhi::IDevice* dev = e.device();
    rhi::IResourceFactory* res = dev ? dev->resources() : nullptr;
    if (!res) return;   // headless, or a device that never came up -- nothing to upload into

    const std::string fontPath = executableDir() + AVER_PATH_SEP "Roboto-Regular.ocfont";
    std::string text;
    if (!readFileText(fontPath, text)) {
        // Legal to ship without HUD font
        AVER_INFO("[Game] no game-UI font at {} -- the HUD draws without text", fontPath);
        return;
    }
    std::string why;
    if (!ui::parseOcfont(text, uiFont_, &why)) {
        AVER_WARN("[Game] '{}': {}", fontPath, why);
        return;
    }

    // Extract atlas filename from content-relative path
    std::string atlas = uiFont_.atlasPath;
    const usize slash = atlas.find_last_of("/\\");
    if (slash != std::string::npos) atlas = atlas.substr(slash + 1);
    const std::string atlasPath = executableDir() + AVER_PATH_SEP + atlas;

    ImageData img;
    if (!decodeImage(atlasPath, img, &why)) {
        AVER_WARN("[Game] the game-UI font atlas '{}' could not be read ({})", atlasPath, why);
        uiFont_ = ui::UiFont{};   // glyphs with no atlas would sample nothing; worse than no font
        return;
    }
    rhi::TextureDesc td;
    td.width = img.width;
    td.height = img.height;
    td.format = rhi::Format::RGBA8Unorm;
    td.bind = rhi::ResourceBind::ShaderResource;
    td.initialState = rhi::ResourceState::ShaderResource;
    td.debugName = "GameUiFontAtlas";
    const void* levels[1] = {img.pixels.data()};
    td.initialData = levels;
    td.initialDataCount = 1;
    td.initialRowPitch = img.rowPitch();
    uiFontTexture_ = res->createTexture(td);
    if (!uiFontTexture_) { AVER_WARN("[Game] the game-UI font atlas could not be uploaded"); return; }
    // UiDrawCmd::texture is cast back to rhi::TextureHandle
    uiFont_.atlasTexture = static_cast<u64>(uiFontTexture_);
    AVER_INFO("[Game] game-UI font '{}' loaded: {} glyph(s), atlas {}x{}",
              uiFont_.name, uiFont_.glyphs.size(), img.width, img.height);
}
#endif

void GameApp::attachParticles(Engine& e) {
#if AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
    rhi::IDevice* dev = e.device();
    if (!dev) return;

    particles::particleSystem().setEffectLibrary(&particles::particleEffects());
    {
        const int pm = cfg_.particleGpuArg >= 0 ? cfg_.particleGpuArg : (project_.particleGpu >= 0 ? project_.particleGpu : 1);
        particles::particleSystem().setGpuMode(pm);
        if (project_.particleScale >= 0) {
            particles::particleEffects().setScalabilityTier(static_cast<particles::ScalabilityTier>(std::min(project_.particleScale, 3)));
        }
#if AVER_MODULE_VOXI
        else {   // no RENDER.PARTICLESCALE: the particle tier follows the Overall Quality rung the project's render settings read as
            particles::ScalabilityTier t;
            const voxi::Renderer& vx = voxi::Renderer::get();
            if (particles::tierForQualityRung(static_cast<u32>(voxi::overallFromSettings(vx.settings(), vx.deviceInfo())), t))
                particles::particleEffects().setScalabilityTier(t);
        }
#endif
        particleRenderer_.setGpuEnabled(pm != 0);
        particleRenderer_.setTextureResolver(&GameContent::resolveParticleTexture, &content_);
        particleRenderer_.setMeshResolver(&GameContent::resolveParticleMesh, &content_);
    }
    if (particleRenderer_.init(*dev)) {
        particleRenderer_.setSystem(&particles::particleSystem());
        dev->addRenderFeature(&particleRenderer_);
        particlesAttached_ = true;
#if AVER_MODULE_VOXI
        // Install GI seam if Voxi attached and not disabled
        if (voxiAttached_ && !cfg_.noParticleGi) {
            particles::ParticleRenderer::GiSeam seam;
            seam.prepare = &GameApp::particleGiPrepare;
            seam.bind = &GameApp::particleGiBind;
            seam.user = this;
            particleRenderer_.setGiSeam(seam);
        }
#endif
        AVER_INFO("[Game] particles attached");
    } else {
        AVER_WARN("[Game] particle renderer unavailable on this device; CParticleEmitter placements draw nothing");
    }
#else
    (void)e;
#endif
}


rhi::MeshHandle GameApp::depthProxyLookup(rhi::MeshHandle mesh, void* user) {
#if AVER_MODULE_SCENE
    if (!user) return 0;
    const GameApp* app = static_cast<const GameApp*>(user);
    const auto& proxyMap = app->content_.depthProxyMap();
    const auto it = proxyMap.find(mesh);
    return it == proxyMap.end() ? 0 : it->second;
#else
    (void)mesh; (void)user;
    return 0;
#endif
}

#if AVER_MODULE_VOXI && AVER_MODULE_PARTICLES && AVER_MODULE_SCENE
// ParticleRenderer GI seam (particles DECIDED 4; modules/particles does not include voxi headers)
bool GameApp::particleGiPrepare(u32 srvBase, u32 samplerBase, u32 cbRegister,
                                std::string* outPrelude, std::string* outDefines, void* user) {
    (void)user;
    if (!outPrelude || !outDefines) return false;
    *outPrelude = voxi::giShaderPrelude();
    *outDefines = voxi::giShaderDefines(srvBase, samplerBase, cbRegister);
    return true;
}

void GameApp::particleGiBind(rhi::IResourceFactory& res, rhi::BindingSetHandle set, u32 srvBase,
                             const void** outCbData, u32* outCbBytes, void* user) {
    auto* self = static_cast<GameApp*>(user);
    if (!self || !outCbData || !outCbBytes) return;
    self->voxiRenderer_.bindGiResources(res, set, srvBase);
    *outCbData = self->voxiRenderer_.giFrameConstants();
    *outCbBytes = self->voxiRenderer_.giFrameConstantBytes();
}
#endif

void GameApp::resolveAverSr(Engine& e) {
#if AVER_MODULE_VOXI
    // AverSR resolved after openProject. applyProjectRenderSettings committed project's GI/RT tiers.
    // CLI (--aversr) beats RENDER.AVERSR beats Auto. Shipped game: userLevel pinned to -1.
    // Upscaler: TAAU (the editor's choice) unless --no-taa / RENDER.TAA 0, then FSR 1. TAAU also runs at
    // native scale (Off) as the anti-aliasing; FSR 1 only when a level scales the scene.
    if (rhi::IDevice* dev = e.device()) {
        const voxi::Renderer& vx = voxi::Renderer::get();
        const voxi::AverSrDecision dec = voxi::resolveAverSrLevel(
            cfg_.averSrArg, -1, project_.averSr, voxi::autoAverSrLevel(vx.settings(), vx.deviceInfo()));
        const bool wantTaa = cfg_.taaArg >= 0 ? cfg_.taaArg == 1 : project_.taa != 0;

        // averSrInstaller_ is null when SR module is absent. Resolved level with no installer stays native 1.0.
        f32 scale = 1.0f;
        if ((dec.level != voxi::ladder::kAverSrOff || wantTaa) && averSrInstaller_ &&
            averSrInstaller_(*dev, dec.level, wantTaa, averSrUpscaler_, scale)) {
            dev->setRenderScale(scale);
            baseRenderScale_ = scale;
            dev->setUpscaler(averSrUpscaler_.get());
            temporalAa_ = wantTaa;
        }

        // Mirrors setRenderScale formula: scene renders at round(present * scale).
        const u32 presentW = e.window() ? e.window()->width()  : cfg_.width;
        const u32 presentH = e.window() ? e.window()->height() : cfg_.height;
        const u32 sceneW = static_cast<u32>(std::lround(static_cast<f64>(presentW) * scale));
        const u32 sceneH = static_cast<u32>(std::lround(static_cast<f64>(presentH) * scale));
        AVER_INFO("[AverSR] {} ({}): scene {}x{} -> present {}x{}, upscaler {}",
                  averSrLevelName(dec.level), averSrSourceName(dec.source), sceneW, sceneH, presentW, presentH,
                  !averSrUpscaler_ ? "none" : temporalAa_ ? "TAAU" : "FSR 1");
    }
#else
    (void)e;
#endif
}

} // namespace aver::game
