#include "aver/game/GameWater.hpp"

#if AVER_MODULE_FLUIDS

#include "aver/game/GameContent.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <cmath>

#if AVER_MODULE_PHYSICS
#  include "aver/physics/physics_abi.h"
#endif
#if AVER_FLUIDS_SIMULATED
#  if AVER_MODULE_FRAMEWORK
#    include "aver/framework/framework_abi.h"
#  endif
#  if AVER_MODULE_SCENE
#    include "aver/scene/World.hpp"
#  endif
#endif
#if AVER_MODULE_PBR
#  include "aver/pbr/MaterialSystem.hpp"
#  include <unordered_set>
#endif

namespace aver::game {

void GameWater::init(rhi::IDevice& device) {
#if AVER_FLUIDS_SIMULATED
    // Registered unconditionally, spawned only if a level or a script asks for one -- an idle
    // feature costs nothing.
    if (fluidScene_.init(device)) device.addRenderFeature(&fluidScene_);

#if AVER_MODULE_FRAMEWORK
    // A graph-authored `COMP ... Fluid` and a plain script SpawnFluidVolume call both reach
    // fluidScene_ through this one relay, installed beside fluidScene_.init() for the same reason
    // the editor does: the providers answer a query that means nothing before this registration.
    aver_fw_set_fluid_spawn_provider(&GameWater::fluidSpawnProvider, this);
    aver_fw_set_fluid_spawn_material_provider(&GameWater::fluidSpawnMaterialProvider, this);
#endif
#else
    (void)device;
#endif
}

void GameWater::applyLevel(rhi::IDevice& device, const fmt::OcWorldData& w) {
    if (w.waters.empty()) return;
    const fmt::OcWaterPlacement& wp = w.waters.front();

    // ONE SURFACE DRAWN: WaterRenderer holds a single level and wave set, so a second WATER record
    // has nowhere to go until the renderer can hold more than one. Placed BEFORE the simulate
    // branch below (which returns), so a simulated record's level still gets this warning about
    // its second record.
    if (w.waters.size() > 1)
        AVER_WARN("[Water] the level declares {} WATER records; only '{}' is rendered",
                  w.waters.size(), wp.name.empty() ? "unnamed" : wp.name);

#if AVER_FLUIDS_SIMULATED
    // A simulated record is not a Gerstner surface, and taking both paths would draw two waters in
    // the same hole. Returns rather than falling through: the soft body IS the water for this
    // record.
    if (wp.simulate) {
        if (wp.infinite) {
            // A simulated volume is a closed shell needing a size; an endless ocean has none to
            // give it. Refused here, not in the parser -- the format's job is to carry what was
            // written.
            AVER_WARN("[Water] '{}' asks to be simulated but declares no bounds; a simulated "
                      "volume needs a size, so it is left analytic",
                      wp.name.empty() ? "unnamed" : wp.name);
        } else {
            fluids::FluidVolumeDesc fd;
            // The shell fills the authored footprint, hanging BELOW the surface line: the body's
            // centre sits half its depth under `level`. Depth is the shallower of a sensible pool
            // depth and the footprint, so a puddle never gets a shell deeper than it is wide.
            const f32 halfX = static_cast<f32>(wp.boundsMax[0] - wp.boundsMin[0]) * 0.5f;
            const f32 halfY = static_cast<f32>(wp.boundsMax[1] - wp.boundsMin[1]) * 0.5f;
            const f32 halfZ = std::min(60.0f, std::min(halfX, halfY));
            fd.centreCm[0] = static_cast<f32>(wp.boundsMin[0] + wp.boundsMax[0]) * 0.5f;
            fd.centreCm[1] = static_cast<f32>(wp.boundsMin[1] + wp.boundsMax[1]) * 0.5f;
            fd.centreCm[2] = static_cast<f32>(wp.levelCm) - halfZ;
            fd.halfExtentCm[0] = halfX;
            fd.halfExtentCm[1] = halfY;
            fd.halfExtentCm[2] = halfZ;
            // Overrides FluidVolumeDesc's own {8,8,4} default: 14x14 horizontal is the finer top
            // face a player-visible pool wants; Z stays at the struct's own 4 (vertical detail is
            // rarely camera-visible on a shallow pool).
            fd.subdivisions[0] = 14;
            fd.subdivisions[1] = 14;

            // The four solver knobs, applied only when this record actually named one -- wp's own
            // fields default to -1 ("not authored"), so an old .ocmap keeps FluidVolumeDesc's own
            // defaults exactly as before these tokens existed.
            if (wp.compliance >= 0.0) fd.compliance = static_cast<f32>(wp.compliance);
            if (wp.damping    >= 0.0) fd.damping    = static_cast<f32>(wp.damping);
            if (wp.iterations >= 0)   fd.iterations = static_cast<u32>(wp.iterations);
            if (wp.pressure   >= 0.0) fd.pressure   = static_cast<f32>(wp.pressure);

            // The solver's material layer, same "applied only when actually named" rule: a
            // non-empty preset wins outright; otherwise density/viscosity each apply independently.
            if (!wp.preset.empty()) {
                if (auto mat = fluids::fluidPhysicsMaterialPreset(wp.preset)) {
                    fd.material = *mat;
                } else {
                    AVER_WARN("[Water] '{}' names unknown material preset '{}'; no material applied",
                              wp.name.empty() ? "unnamed" : wp.name, wp.preset);
                }
            } else if (wp.density >= 0.0 || wp.viscosity >= 0.0) {
                fluids::FluidPhysicsMaterial mat;   // struct defaults are water's own numbers
                if (wp.density   >= 0.0) mat.densityKgM3  = static_cast<f32>(wp.density);
                if (wp.viscosity >= 0.0) mat.viscosityPaS = static_cast<f32>(wp.viscosity);
                fd.material = mat;
            }

            // The SURFACE material -- how the volume LOOKS, not how it moves -- carried to the
            // draw site by name (see fluidSurfaceMaterial_).
            fluidWantSurfaceMaterial_ = wp.material;

            // Latched, not spawned: init()'s fluidScene_ may not be ready() yet when applyLevel
            // runs. update()'s drain performs the real spawn().
            fluidWantDesc_    = fd;
            fluidWantName_    = wp.name;
            fluidWantPending_ = true;
            // The buoyancy plane still comes from the authored level: the solver exposes no query
            // for the deformed surface, and a flat plane at the authored height is closer than none.
            const f32 normal[3]  = {0.0f, 0.0f, 1.0f};
            const f32 current[3] = {0.0f, 0.0f, 0.0f};
            aver_phys_set_water_plane(static_cast<f32>(wp.levelCm), normal, 1.0f, 0.5f, 0.05f, current);
            return;
        }
    }
#endif

    // The waves belonging to this surface: the ones that name it, plus the ones that name nothing
    // at all, which the format defines as meaning the FIRST declared water.
    fluids::GerstnerWave waves[fluids::kMaxGerstnerWaves];
    size_t n = 0;
    size_t skipped = 0;
    for (const fmt::OcGerstnerWave& gw : w.waves) {
        if (!gw.water.empty() && gw.water != wp.name) continue;
        if (n >= fluids::kMaxGerstnerWaves) { ++skipped; continue; }
        waves[n].dirX         = static_cast<f32>(gw.dirX);
        waves[n].dirZ         = static_cast<f32>(gw.dirZ);
        waves[n].wavelengthCm = static_cast<f32>(gw.wavelengthCm);
        waves[n].amplitudeCm  = static_cast<f32>(gw.amplitudeCm);
        waves[n].steepness    = static_cast<f32>(gw.steepness);
        ++n;
    }
    if (skipped)
        AVER_WARN("[Water] '{}' declares {} waves; the renderer takes {} and the rest are dropped",
                  wp.name.empty() ? "unnamed" : wp.name, n + skipped, fluids::kMaxGerstnerWaves);

    // Zero waves is a legal answer, not a reason to fall back on a default swell -- a level that
    // declared WATER and no WAVEs asked for still water. gerstnerHeightCm's own contract already
    // returns the flat level for an empty set.
    if (!attachSurface(device, static_cast<f32>(wp.levelCm), waves, n)) {
        AVER_ERROR("[Water] the level authored water, but the renderer is unavailable on this device");
        return;
    }

    if (wp.infinite) {
        waterRenderer_.clearWaterBounds();
    } else {
        waterRenderer_.setWaterBoundsCm(static_cast<f32>(wp.boundsMin[0]), static_cast<f32>(wp.boundsMin[1]),
                                        static_cast<f32>(wp.boundsMax[0]), static_cast<f32>(wp.boundsMax[1]));
    }

    // The surface ripple set, different from the Gerstner swell above: the swell displaces
    // vertices on an analytic ocean, this shapes the NORMAL the material graph and caustics read
    // (IDevice::setWaterWaves) -- derived from the authored waves where there are any, else a
    // default sized for a pool (non-harmonic wavelengths avoid a visible beat; each speed is a
    // multiple of 2*pi/3600 so it crosses gTime's hourly wrap without a jump).
    {
        f32 rip[3][4];
        if (n > 0) {
            for (u32 i = 0; i < 3; ++i) {
                const fluids::GerstnerWave& g = waves[i < n ? i : n - 1];
                const f32 len = std::sqrt(g.dirX * g.dirX + g.dirZ * g.dirZ);
                const f32 k = 6.2831853f / (g.wavelengthCm > 1.0f ? g.wavelengthCm : 1.0f);
                rip[i][0] = len > 1e-4f ? g.dirX / len : 1.0f;
                rip[i][1] = len > 1e-4f ? g.dirZ / len : 0.0f;
                rip[i][2] = k;
                // Deep-water dispersion, snapped to the hourly wrap: omega = sqrt(g*k).
                const f32 omega = std::sqrt(981.0f * k);
                rip[i][3] = std::round(omega / 0.001745329f) * 0.001745329f;
            }
        } else {
            const f32 kk[3] = {0.169816f, 0.273182f, 0.145670f};
            const f32 ss[3] = {3.740140f, 4.640800f, 2.879793f};
            const f32 dx[3] = {1.0f, 0.0f, 0.7071068f};
            const f32 dy[3] = {0.0f, 1.0f, 0.7071068f};
            for (u32 i = 0; i < 3; ++i) {
                rip[i][0] = dx[i]; rip[i][1] = dy[i]; rip[i][2] = kk[i]; rip[i][3] = ss[i];
            }
        }
        device.setWaterWaves(rip, 3, 0.055f);
    }

    // The same single number for both the rendered and simulated surface: two independent heights
    // would drift, reading as broken buoyancy rather than a mismatch.
#if AVER_MODULE_PHYSICS
    {
        const f32 normal[3]  = {0.0f, 0.0f, 1.0f};
        const f32 current[3] = {0.0f, 0.0f, 0.0f};
        aver_phys_set_water_plane(waterRenderer_.waterLevelCm(), normal, 1.0f, 0.5f, 0.05f, current);
    }
#endif

    AVER_INFO("[Water] level surface '{}' at z = {} cm with {} wave(s){}",
              wp.name.empty() ? "unnamed" : wp.name, wp.levelCm, n,
              wp.infinite ? "" : " (bounded)");
}

bool GameWater::attachSurface(rhi::IDevice& device, f32 levelCm, const fluids::GerstnerWave* waves,
                              size_t n) {
    if (!waterAttached_) {
        if (!waterRenderer_.init(device)) return false;
        device.addRenderFeature(&waterRenderer_);
        waterAttached_ = true;
    }
    waterRenderer_.setWaterLevelCm(levelCm);
    waterRenderer_.setWaves(waves, n);
    return true;
}

bool GameWater::respawnLevelVolume() {
#if AVER_FLUIDS_SIMULATED
    // The level's own volume only -- a graph-authored one is spawned by its own script/graph node,
    // not by the level re-loading, so it is left alone here.
    if (!fluidHandle_) return false;
    fluidSurfaceMaterial_.erase(fluidHandle_);
    fluidScene_.despawn(fluidHandle_);
    fluidHandle_ = 0;
    // Re-latches the SAME desc/name/surface material applyLevel last set: update()'s drain re-spawns
    // and re-inserts the surface material for the new handle.
    fluidWantPending_ = true;
    return true;
#else
    return false;
#endif
}

void GameWater::update(rhi::IDevice& device, f32 dt) {
#if AVER_FLUIDS_SIMULATED
    // The one place a fluid volume is ever spawned: applyLevel only latches what a level asked
    // for; this drains it. A volume spawned here still gets its prePass this frame, so its seed
    // shell is overwritten before it is ever visible at the origin.
    if (fluidWantPending_) {
        fluidWantPending_ = false;
        fluidHandle_ = fluidScene_.spawn(fluidWantDesc_, device);
        if (fluidHandle_) fluidSurfaceMaterial_[fluidHandle_] = fluidWantSurfaceMaterial_;
        const char* nm = fluidWantName_.empty() ? "unnamed" : fluidWantName_.c_str();
        if (fluidHandle_) {
            AVER_INFO("[Water] '{}' is SIMULATED: a {}x{}x{} cm soft body centred at ({}, {}, {})",
                      nm, fluidWantDesc_.halfExtentCm[0] * 2.0f, fluidWantDesc_.halfExtentCm[1] * 2.0f,
                      fluidWantDesc_.halfExtentCm[2] * 2.0f, fluidWantDesc_.centreCm[0],
                      fluidWantDesc_.centreCm[1], fluidWantDesc_.centreCm[2]);
        } else {
            AVER_ERROR("[Water] '{}' asked to be simulated but the fluid body could not be created", nm);
        }
    }

    // Graph-authored fluid volumes, drained here for the identical readiness reason
    // fluidWantPending_ is.
    for (const FluidGraphRequest& req : fluidGraphQueue_) {
        const fluids::FluidHandle h = fluidScene_.spawn(req.desc, device);
        if (h) {
            fluidGraphHandles_.push_back(h);
            AVER_INFO("[Fluid] '{}' is SIMULATED: a {}x{}x{} cm soft body centred at ({}, {}, {}), "
                      "compliance={}, damping={}, iterations={}, pressure={}",
                      req.label, req.desc.halfExtentCm[0] * 2.0f, req.desc.halfExtentCm[1] * 2.0f,
                      req.desc.halfExtentCm[2] * 2.0f, req.desc.centreCm[0], req.desc.centreCm[1],
                      req.desc.centreCm[2], req.desc.compliance, req.desc.damping,
                      req.desc.iterations, req.desc.pressure);
        } else {
            AVER_ERROR("[Fluid] '{}' asked to be simulated but the fluid body could not be created",
                       req.label);
        }
    }
    fluidGraphQueue_.clear();

    // AFTER the physics step, BEFORE any prePass: update() reads the solver's current particle
    // positions, so running before the step draws the previous frame's shape.
    fluidScene_.update();

#if AVER_MODULE_FRAMEWORK && AVER_MODULE_SCENE
    // Fallback for a gap in Jolt's own soft-body update: a fluid volume's own collision pass never
    // sees the player's capsule.
    if (fluidHandle_) {
        const i32 body = fluidScene_.physicsBody(fluidHandle_);
        const i32 pawn = aver_fw_controlled_pawn(aver_fw_player_controller(0));
        if (body && pawn) {
            const scene::Entity pe = static_cast<scene::Entity>(static_cast<u32>(pawn));
            const Mat4& wm = scene::World::instance().worldMatrix(pe);
            const Vec3 playerPos{wm.m[3][0], wm.m[3][1], wm.m[3][2]};

            // Cheap AABB-ish reject before touching the solver: most frames the player is nowhere
            // near any given pool, and the impulse ABI walks every particle to answer that the slow
            // way.
            const auto& d = fluidWantDesc_;
            const f32 marginCm = 200.0f;
            const bool nearVolume =
                std::abs(playerPos.x - d.centreCm[0]) < d.halfExtentCm[0] + marginCm &&
                std::abs(playerPos.y - d.centreCm[1]) < d.halfExtentCm[1] + marginCm &&
                std::abs(playerPos.z - d.centreCm[2]) < d.halfExtentCm[2] + marginCm;

            if (nearVolume) {
                // No native handle reaches the character's physics velocity, so it is recovered by
                // finite difference across frames.
                Vec3 vel{0.0f, 0.0f, 0.0f};
                if (fluidPrevPlayerValid_ && dt > 1.0e-5f)
                    vel = (playerPos - fluidPrevPlayerPosCm_) * (1.0f / dt);

                // Centred a half-height above the feet (the entity's origin is the character's
                // feet) and sized to bracket its default capsule without reading it back.
                const Vec3 centre = playerPos + Vec3{0.0f, 0.0f, 90.0f};
                const f32 radiusCm = 90.0f;
                aver_phys_softbody_apply_impulse(body, &centre.x, radiusCm, &vel.x, 0.5f);
            }

            fluidPrevPlayerPosCm_ = playerPos;
            fluidPrevPlayerValid_ = true;
        } else {
            fluidPrevPlayerValid_ = false;   // no pawn this frame -- next frame's diff would be bogus
        }
    }
#endif
#else
    (void)device; (void)dt;
#endif
}

rhi::SkyAtmosphere GameWater::applyUnderwaterFog(const rhi::SkyAtmosphere& sky, f32 cameraZCm) const {
    if (!waterAttached_) return sky;
    return fluids::applyUnderwaterFog(sky, cameraZCm, waterRenderer_.waterLevelCm(), waterFog_);
}

#if AVER_FLUIDS_SIMULATED
void GameWater::drawOneFluid(rhi::IDevice& device, GameContent& content, pbr::MaterialSystem* materials,
                             fluids::FluidHandle h) const {
    const rhi::MeshHandle fm = fluidScene_.drawHandle(h);
    if (!fm) return;

    // The surface material, resolved at DRAW time rather than latched at spawn: a volume can be
    // spawned before its project's materials finish loading, so a handle captured too early could
    // be a permanent zero.
    // u32, not pbr::MaterialHandle: that alias only exists with AVER_MODULE_PBR on.
    u32 authored = 0;
#if AVER_MODULE_PBR
    if (const auto nm = fluidSurfaceMaterial_.find(h);
        nm != fluidSurfaceMaterial_.end() && !nm->second.empty()) {
        authored = content.materialForSurface(nm->second);
        if (!authored) {
            // ONCE PER NAME: a WATER record naming a material nobody authored is otherwise silent.
            static std::unordered_set<std::string> s_warned;
            if (s_warned.insert(nm->second).second)
                AVER_WARN("[Water] volume {} names surface material '{}' but no .ocmat by "
                          "that name was loaded; drawing the fallback look", h, nm->second);
        }
    }
#endif

    // The fallback look, for a WATER record naming no .ocmat. Alpha is load-bearing: at 1.0 this
    // would put an opaque lid over the pool.
    f32 col[4]   = {0.35f, 0.55f, 0.62f, 0.35f};
    f32 metallic = 0.0f, roughness = 0.10f;
    bool blended = true;
#if AVER_MODULE_PBR
    if (authored) {
        col[0] = col[1] = col[2] = 1.0f;
        metallic = roughness = 1.0f;
        if (const pbr::MaterialDesc* d = pbr::MaterialLibrary::get().desc(authored))
            blended = pbr::isTranslucent(*d);
    }
    // setDrawBinding is STICKY, so it is stated before every draw rather than set once -- bound
    // even with no authored material, since bindingSet(0)/constants(0) resolve to the system's own
    // fallback rather than leaving the previous draw's binding stuck.
    if (materials && materials->ready())
        device.setDrawBinding(materials->bindingSet(authored), &materials->constants(authored),
                              sizeof(pbr::MaterialConstants));
#endif
    // Blended even with no authored material: only an authored OPAQUE material turns it off --
    // water with no .ocmat must composite over the pool floor, not draw as a solid lid.
    device.setDrawBlended(blended);

    // A real translation, not identity: the buffer is mesh-LOCAL about the volume's centre, so
    // this matrix places it AND gives the blended flush something true to sort by.
    f32 origin[3] = {0.0f, 0.0f, 0.0f};
    fluidScene_.volumeOrigin(h, origin);
    Mat4 fw = Mat4::identity();
    fw.m[3][0] = origin[0];
    fw.m[3][1] = origin[1];
    fw.m[3][2] = origin[2];
    device.drawMesh(fm, &fw.m[0][0], col, metallic, roughness);
}
#endif

void GameWater::draw(rhi::IDevice& device, GameContent& content, pbr::MaterialSystem* materials) const {
#if AVER_FLUIDS_SIMULATED
    // Every live volume, drawn in handle order and not sorted here -- the device captures blended
    // draws and replays them sorted back-to-front itself.
    if (fluidHandle_) drawOneFluid(device, content, materials, fluidHandle_);
    for (const fluids::FluidHandle h : fluidGraphHandles_) drawOneFluid(device, content, materials, h);
#else
    (void)device; (void)content; (void)materials;
#endif
}

void GameWater::unload() {
#if AVER_FLUIDS_SIMULATED
    // The only teardown site for what a level itself spawned. Not folded into a generic body
    // list: neither the WATER record's own volume nor a graph-spawned one is ever pushed to one,
    // and removing either directly here (rather than through it) would double-free once
    // FluidScene::retire removes it too.
    //
    // THE ANALYTIC SURFACE IS LEFT STANDING: a level with a WATER record followed by one with none
    // keeps showing the first surface, since applyLevel returns early and nothing else clears
    // waterRenderer_. Unreachable while the runtime loads one level per process; a known gap in
    // both hosts.
    if (fluidHandle_) {
        fluidSurfaceMaterial_.erase(fluidHandle_);
        fluidScene_.despawn(fluidHandle_);
        fluidHandle_ = 0;
    }
    // Cleared for the "simulated level -> level with no water" case: applyLevel returns
    // immediately when the new level declares no WATER record, so a latch left standing here would
    // spawn the OLD level's volume into the new world on the next update().
    fluidWantPending_ = false;
    // The same reasoning, generalised to every graph-authored volume this level's actors spawned:
    // none are pushed to a body list either, and a class outliving its own level's teardown would
    // go on sloshing in the next one.
    for (const fluids::FluidHandle h : fluidGraphHandles_) {
        fluidSurfaceMaterial_.erase(h);
        fluidScene_.despawn(h);
    }
    fluidGraphHandles_.clear();
    fluidGraphQueue_.clear();
#endif
}

void GameWater::shutdown(rhi::IDevice* device) {
#if AVER_FLUIDS_SIMULATED
    // BEFORE physics shuts down: retiring a live volume calls into the solver.
    if (device) device->removeRenderFeature(&fluidScene_);
    fluidScene_.shutdown();
#endif
    // Unregistered here so no feature outlives its registration.
    if (waterAttached_) {
        if (device) device->removeRenderFeature(&waterRenderer_);
        waterRenderer_.shutdown();
        waterAttached_ = false;
    }
}

#if AVER_FLUIDS_SIMULATED
#if AVER_MODULE_FRAMEWORK
i32 GameWater::fluidSpawnProvider(f32 cx, f32 cy, f32 cz, f32 hx, f32 hy, f32 hz,
                                  f32 compliance, f32 damping, i32 iterations, f32 pressure,
                                  const char* name, void* user) {
    auto* self = static_cast<GameWater*>(user);
    if (!self) return 0;
    fluids::FluidVolumeDesc desc;
    desc.centreCm[0] = cx; desc.centreCm[1] = cy; desc.centreCm[2] = cz;
    desc.halfExtentCm[0] = hx; desc.halfExtentCm[1] = hy; desc.halfExtentCm[2] = hz;
    desc.compliance = compliance;
    desc.damping    = damping;
    desc.iterations = static_cast<u32>(iterations);
    desc.pressure   = pressure;
    self->fluidGraphQueue_.push_back({desc, name && *name ? name : "unnamed"});
    return 1;
}

i32 GameWater::fluidSpawnMaterialProvider(f32 cx, f32 cy, f32 cz, f32 hx, f32 hy, f32 hz,
                                          f32 compliance, f32 damping, i32 iterations, f32 pressure,
                                          f32 densityKgM3, f32 viscosityPaS, const char* materialPreset,
                                          const char* name, void* user) {
    auto* self = static_cast<GameWater*>(user);
    if (!self) return 0;
    fluids::FluidVolumeDesc desc;
    desc.centreCm[0] = cx; desc.centreCm[1] = cy; desc.centreCm[2] = cz;
    desc.halfExtentCm[0] = hx; desc.halfExtentCm[1] = hy; desc.halfExtentCm[2] = hz;
    desc.compliance = compliance;
    desc.damping    = damping;
    desc.iterations = static_cast<u32>(iterations);
    desc.pressure   = pressure;

    const std::string label = name && *name ? name : "unnamed";
    if (materialPreset && *materialPreset) {
        if (auto mat = fluids::fluidPhysicsMaterialPreset(materialPreset)) {
            desc.material = *mat;
        } else {
            AVER_WARN("[Fluid] '{}' names unknown material preset '{}'; no material applied",
                      label, materialPreset);
        }
    } else if (densityKgM3 > 0.0f || viscosityPaS > 0.0f) {
        fluids::FluidPhysicsMaterial mat;   // struct defaults are water's own numbers
        if (densityKgM3  > 0.0f) mat.densityKgM3  = densityKgM3;
        if (viscosityPaS > 0.0f) mat.viscosityPaS = viscosityPaS;
        desc.material = mat;
    }

    self->fluidGraphQueue_.push_back({desc, label});
    return 1;
}
#endif // AVER_MODULE_FRAMEWORK
#endif // AVER_FLUIDS_SIMULATED

} // namespace aver::game

#endif // AVER_MODULE_FLUIDS
