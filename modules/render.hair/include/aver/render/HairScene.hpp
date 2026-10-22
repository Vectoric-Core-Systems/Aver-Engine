#pragma once
// The draw side of strand hair (docs/plans/wave11/SOFTBODY_HAIR.md 2.16): replays the GPU simulation and expands the simulated strands into
// camera-facing ribbons that the ordinary mesh draw shows. The simulation, the components and the per-instance rules are Aver.Hair.Scene's
// (HairSystem); the compute host the simulation runs on is RhiComputeHost; this class owns the GPU side of the drawn instances.
//
// SkinnedScene and SoftBodyScene are the model, almost line for line: a per-ENTITY mesh whose vertex buffer a compute pass writes
// (IDevice::createSkinTargetMesh), handed to the draw by substituting the MeshHandle (drawHandle) rather than by teaching the draw path a new stream.
// Both hosts already do it for skinned characters and soft bodies; hair is one more link in that chain.
//
// PER GROOM (shared by every instance of it, keyed by the asset): the ribbon layout (strands in a seeded random rank order, so any prefix is a uniform
// thinning), the point buffer, the zero-vertex source mesh whose index buffer the instances share, and the hair material built from the groom's LOOK.
// PER INSTANCE: a skin-target mesh (the ribbon vertices), up to three posed-part meshes (the thinner levels, index prefixes of the same vertices),
// and a binding set (points, the host's render positions buffer, the vertex buffer) that is made once and never rewritten.
//
// A FRAME: update() reconciles residency with the system's views, picks the level and fills the pass constants; prePass() replays the sealed
// simulation batches and then runs one ribbon dispatch per drawn instance; the scene pass draws drawHandle(); overlayPass() returns every buffer to Common,
// which the RHI requires at the end of a frame (the rule SkinnedMeshGpu's comment states).
//
// THE DRAW CHILD starts invisible (HairSystem makes it that way) and is revealed here, once the instance has a ribbon mesh and a simulated pose, and hidden
// again when the system hides it: this class is the only thing that turns it on, so a device without compute shows no stray placeholder cube.
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

// Frames a retired GPU object waits before it is destroyed (the retire-during-recording rule: kHairHostFramesInFlight + 1).
inline constexpr u32 kHairRetireFrames = kHairHostFramesInFlight + 1;

struct HairSceneOptions {
    u32 vramBudgetMB = 96;        // drawn grooms past this are refused (they draw nothing); HairOptions::vramBudgetMB bounds the build the same way
    f32 minPixels = 1.0f;         // the narrowest a ribbon is drawn
    u32 maxBuildsPerFrame = 2;    // new instances made per update (each creates meshes and a binding set)
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

    // Makes the compute host and the ribbon pipeline. False leaves the feature INERT, not broken: hair is simulated by nobody and drawn by nobody, and the
    // composition root installs no compute host (hairSystem().installComputeHost(nullptr)).
    bool init(rhi::IDevice& dev);
    void shutdown();
    bool ready() const { return ready_; }

    // The table the composition root hands to HairSystem::installComputeHost BEFORE the first groom is built. Valid after a successful init().
    const AverPhysComputeHostV1& hostTable() const { return host_.table(); }
    RhiComputeHost& host() { return host_; }

    // The system whose views are drawn. Defaults to hair::hairSystem().
    void setSystem(hair::HairSystem* sys) { sys_ = sys; }
    hair::HairSystem& system() { return sys_ ? *sys_ : hair::hairSystem(); }

    // The host side of the hair material: after a groom's material is made, the draw child's surface token (aver_scene_material) must be bound to it,
    // which is GameContent::bindSurfaceMaterial in both hosts. Without a binder the hair draws with the host's default material.
    using MaterialBindFn = void (*)(i32 token, pbr::MaterialHandle handle, void* user);
    void setMaterialBinder(MaterialBindFn fn, void* user) { bindFn_ = fn; bindUser_ = user; }

    HairSceneOptions& options() { return opt_; }

    // Reconciles residency with the system's views, picks each instance's level and fills the pass constants. Called ONCE per frame by the host, after
    // the system's tick and World::flush, before the draw pass.
    void update(scene::World& world, rhi::IDevice& dev);

    // What the scene pass should draw for this entity (the draw child), or ZERO meaning "draw its authored (placeholder) mesh".
    rhi::MeshHandle drawHandle(scene::Entity e) const;
    // Whether the hair drawn by this DRAW entity asked to be in the ray-tracing structure (CHair kHairRayTraced, default off). The scene walk asks it
    // beside drawHandle and tells Voxi to leave every other hair draw out of the acceleration structure: a ribbon mesh that is rewritten each frame
    // costs a refit of its BLAS (about 15 MB for a full groom) and would also enter reflections and RT shadows, which hair is not authored for.
    bool inRayTracing(scene::Entity drawEntity) const;
    // Whether it casts shadow-map shadows (CHair without kHairNoShadow). True for an entity that is not hair, which is never asked.
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
        rhi::MeshHandle source = 0;             // zero vertices, the ribbon indices: instances share its index buffer
        pbr::MaterialHandle material = 0;
        u32 refs = 0;
        u64 bytes = 0;
        u32 idleFrames = 0;
    };
    struct Resident {
        u64 key = 0;
        scene::Entity owner = scene::kInvalidEntity;
        scene::Entity drawEntity = scene::kInvalidEntity;
        rhi::MeshHandle draw = 0;               // the skin target: level 0
        rhi::BufferHandle vertices = 0;
        rhi::MeshHandle part[3] = {0, 0, 0};    // levels 1..3, made when first needed; kPartFailed once a try failed
        rhi::MeshHandle current = 0;            // what drawHandle answers this frame
        rhi::BindingSetHandle set = 0;
        u64 setRenderBuffer = 0;                // the host handle the set was made for
        rhi::ResourceState state = rhi::ResourceState::Common;
        u32 level = 0;
        u32 vertexCount = 0;
        bool dispatch = false;                  // this frame
        bool rayTraced = false;                 // CHair kHairRayTraced, as of the last update
        bool noShadow = false;                  // CHair kHairNoShadow
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
    std::unordered_map<u64, u32> failed_;                  // groom keys that would not build -> not retried
    u32 frame_ = 0;
    u32 buildsLast_ = 0;
    u32 dispatchesLast_ = 0;
    u32 refused_ = 0;
};

} // namespace aver::render
