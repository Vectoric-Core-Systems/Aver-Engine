#pragma once
// The draw side of strand hair (docs/plans/wave11/SOFTBODY_HAIR.md 2.16): replays the GPU simulation and expands strands into
// camera-facing ribbons. Modelled on SkinnedScene and SoftBodyScene: a compute pass writes the mesh, drawHandle() swaps it in.
// Per groom: layout, points, shared source mesh and material. Per instance: skin-target mesh, thinner part meshes, binding set.
// update() reconciles residency; prePass() replays sealed batches and dispatches ribbons; overlayPass() returns buffers to Common.
// The draw child starts invisible and is revealed here once the instance has a ribbon mesh and a pose.
#include "aver/core/Math.hpp"
#include "aver/hair/HairSystem.hpp"
#include "aver/pbr/Material.hpp"
#include "aver/render/RhiComputeHost.hpp"
#include "aver/render/RibbonPack.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/scene/World.hpp"

#include <memory>
#include <unordered_map>
#include <vector>

namespace aver::render {

// Frames a retired GPU object waits before it is destroyed (kHairHostFramesInFlight + 1).
inline constexpr u32 kHairRetireFrames = kHairHostFramesInFlight + 1;

struct HairSceneOptions {
    u32 vramBudgetMB = 96;        // drawn grooms past this are refused (they draw nothing)
    f32 minPixels = 1.0f;         // the narrowest a ribbon is drawn
    u32 maxBuildsPerFrame = 2;
};

struct HairSceneStats {
    u32 grooms = 0, instances = 0, drawn = 0, dispatches = 0, buildsLastFrame = 0, refused = 0;
    u64 bytes = 0;
    RhiComputeHostStats host;
};

class HairScene final : public rhi::IRenderFeature {
public:
    HairScene();
    ~HairScene() override;
    HairScene(const HairScene&) = delete;
    HairScene& operator=(const HairScene&) = delete;

    // False leaves the feature inert: no compute host is installed.
    bool init(rhi::IDevice& dev);
    void shutdown();
    bool ready() const { return ready_; }

    // Hand to HairSystem::installComputeHost BEFORE the first groom is built. Valid after a successful init().
    const AverPhysComputeHostV1& hostTable() const { return host_.table(); }
    RhiComputeHost& host() { return host_; }

    // The system whose views are drawn. Defaults to hair::hairSystem().
    void setSystem(hair::HairSystem* sys) { sys_ = sys; }
    hair::HairSystem& system() { return sys_ ? *sys_ : hair::hairSystem(); }

    // Binds the draw child's surface token to the groom's material. Without a binder hair draws the host's default material.
    using MaterialBindFn = void (*)(i32 token, pbr::MaterialHandle handle, void* user);
    void setMaterialBinder(MaterialBindFn fn, void* user) { bindFn_ = fn; bindUser_ = user; }

    HairSceneOptions& options() { return opt_; }

    // Called ONCE per frame by the host, after the system's tick and World::flush, before the draw pass.
    void update(scene::World& world, rhi::IDevice& dev);

    // The mesh the scene pass draws for this entity, or ZERO to draw its authored placeholder.
    rhi::MeshHandle drawHandle(scene::Entity e) const;
    // Hair is kept out of the ray-tracing structure unless CHair kHairRayTraced is set.
    bool inRayTracing(scene::Entity drawEntity) const;
    // Whether it casts shadow-map shadows (CHair without kHairNoShadow).
    bool castsShadow(scene::Entity drawEntity) const;

    const char* name() const override { return "Aver.Render.Hair"; }
    void prePass(rhi::IRenderContext& ctx) override;
    void overlayPass(rhi::IRenderContext& ctx, u32 width, u32 height) override;

    HairSceneStats stats() const;

private:
    struct Groom {
        u64 key = 0;
        RibbonLayout layout;
        rhi::BufferHandle points = 0;           // StructuredBuffer<RibbonPoint>, an Upload buffer written once
        rhi::MeshHandle source = 0;             // zero vertices; instances share its index buffer
        pbr::MaterialHandle material = 0;
        u32 refs = 0;
        u64 bytes = 0;
        u32 idleFrames = 0;
    };
    struct Resident {
        u64 key = 0;
        scene::Entity owner = scene::kInvalidEntity;
        scene::Entity drawEntity = scene::kInvalidEntity;
        rhi::MeshHandle draw = 0;               // level 0 (the skin target)
        rhi::BufferHandle vertices = 0;
        rhi::MeshHandle part[3] = {0, 0, 0};    // levels 1..3; kPartFailed once a try failed
        rhi::MeshHandle current = 0;
        rhi::BindingSetHandle set = 0;
        u64 setRenderBuffer = 0;
        rhi::ResourceState state = rhi::ResourceState::Common;
        u32 level = 0;
        u32 vertexCount = 0;
        bool dispatch = false;
        bool rayTraced = false;
        bool noShadow = false;
        bool shown = false;
        RibbonConstants constants;
        u32 groups = 0;
        u64 renderHandle = 0;
        u64 bytes = 0;
    };
    struct Retired {
        rhi::MeshHandle meshes[5] = {0, 0, 0, 0, 0};   // parts first, then the draw mesh, then (for a groom) its source
        rhi::BindingSetHandle set = 0;
        rhi::BufferHandle buffer = 0;
        pbr::MaterialHandle material = 0;
        u32 framesLeft = 0;
    };

    static constexpr rhi::MeshHandle kPartFailed = 0xFFFFFFFFu;

    bool buildGroom(Groom& g, const hair::HairView& v, rhi::IDevice& dev);
    bool buildResident(Resident& r, Groom& g, const hair::HairView& v, rhi::IDevice& dev);
    void ensureSet(Resident& r, const Groom& g, const hair::HairView& v);
    rhi::MeshHandle levelMesh(Resident& r, const Groom& g, u32 level, rhi::IDevice& dev);
    void retire(Resident& r);
    void retireGroom(Groom& g);
    void age();
    void destroyNow(Retired& t);
    static void showDraw(scene::World& world, scene::Entity draw, bool visible);

    rhi::IDevice* dev_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;
    bool ready_ = false;
    hair::HairSystem* sys_ = nullptr;
    HairSceneOptions opt_;
    RhiComputeHost host_;
    rhi::ShaderHandle cs_ = 0;
    rhi::PipelineHandle pipeline_ = 0;
    MaterialBindFn bindFn_ = nullptr;
    void* bindUser_ = nullptr;

    std::unordered_map<u64, Groom> grooms_;
    std::unordered_map<scene::Entity, Resident> live_;     // keyed by the DRAW entity
    std::vector<Retired> retired_;
    std::unordered_map<u64, u32> failed_;                  // groom keys that would not build; not retried
    u32 frame_ = 0;
    u32 buildsLast_ = 0;
    u32 dispatchesLast_ = 0;
    u32 refused_ = 0;
};

} // namespace aver::render
