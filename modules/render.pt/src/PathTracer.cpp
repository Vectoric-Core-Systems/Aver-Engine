// The path-tracing compute pass: scenes in, linear radiance out.
#include "aver/pt/PathTracer.hpp"

#include <chrono>
#include <unordered_map>
#include "aver/core/Log.hpp"
#include "PtShaders.hpp"

#include <cstring>

namespace aver::pt {

namespace {

// t0 acceleration structure, t1 vertices, t2 indices, t3 instances.
constexpr u32 kSrvCount = 4;
// u0 the accumulator.
constexpr u32 kUavCount = 1;
// The shader's [numthreads(8,8,1)].
constexpr u32 kGroup = 8;

// RayQuery is SM 6.5, and DXR 1.1 is what makes it available from a compute shader with no state
// object and no shader table.
constexpr u32 kShaderModel = 65;
constexpr u32 kRayTracingTier = 11;

// RESOURCE BINDING TIER IS DELIBERATELY NOT CHECKED HERE, and this note exists so that stays a
// decision rather than becoming an oversight someone "fixes" prematurely.
//
// The engine as a whole is explicitly not bindless -- RHIResources.hpp:3, a Resource Binding Tier 1
// commitment that protects the MINIMUM tier (D3D12 FL 11_0: Kepler, GCN 1.0, Haswell). This tracer
// is gated far above that, to DXR 1.1 hardware, every generation of which reports Binding Tier 3.
// So the tracer COULD use descriptor indexing without raising the engine's floor, because it is
// already gated to hardware that has it.
//
// It does not need it yet. The integrator binds a fixed four SRVs and one UAV (kSrvCount/kUavCount
// above) -- TLAS, vertices, indices, instances -- which Tier 1 satisfies comfortably. Adding a
// resourceBindingTier >= 3 requirement TODAY would refuse the path tracer on hardware where it
// currently runs correctly, buying nothing.
//
// IT BECOMES REQUIRED THE MOMENT MATERIALS DO. Sampling an arbitrary material's textures at a ray
// hit is precisely what needs an unbounded, dynamically-indexed table; that work is what should add
// the check, next to the feature that depends on it, so the refusal names a real reason. The
// capability is already queried (DeviceCaps::resourceBindingTier) and already clampable for testing
// (CapsOverride::maxResourceBindingTier, the `tier1` token in --force-caps), so nothing has to be
// built first -- only used.
//
// Until then the honest statement is: this tracer requires DXR 1.1 and SM 6.5, and happens to run
// only on hardware that would also support bindless. That is a coincidence of GPU generations, not
// an invariant this file enforces.

// MIRRORS cbuffer PtFrame in kPathTracerHLSL, field for field. A shifted field here reads a camera
// basis as a sample count -- silently, and only in the rendered image.
struct FrameCB {
    f32 origin[4];
    f32 forward[4];
    f32 right[4];
    f32 up[4];
    u32 image[4];
    u32 sample[4];
    f32 trace[4];
};

// Seven float4/uint4 rows, matching PtShaders.hpp's cbuffer exactly. This assert used to say
// "nothing checks this across the C++/HLSL boundary" -- which was true, and was the whole problem.
// It cannot catch a field REORDERED within the same size, but it does catch the common case: a row
// added on one side and not the other. pbr::MaterialConstants (MaterialGpu.hpp:38) has guarded
// itself this way all along; this struct and pcg::VolumeCB were simply the two that never did.
static_assert(sizeof(FrameCB) == 112, "cbuffer PtFrame in PtShaders.hpp mirrors this byte for byte");
static_assert(sizeof(FrameCB) % 16 == 0, "must be a legal constant-buffer size");

} // namespace

PathTracer::~PathTracer() { shutdown(); }

bool PathTracer::init(rhi::IDevice& dev) {
    shutdown();
    dev_ = &dev;
    res_ = dev.resources();
    if (!res_) return false;

    const rhi::DeviceCaps caps = dev.caps();
    if (caps.rayTracingTier < kRayTracingTier || caps.shaderModel < kShaderModel ||
        !caps.dxcAvailable || !caps.computeShaders) {
        AVER_INFO("[PT] unavailable on this device: RT tier {}, SM {}, DXC {} (needs tier {}, SM {})",
                  caps.rayTracingTier, caps.shaderModel, caps.dxcAvailable, kRayTracingTier,
                  kShaderModel);
        shutdown();
        return false;
    }

    rhi::ShaderDesc sd;
    sd.source  = kPathTracerHLSL;
    // The engine's own declarations, which is where PI, skyColor and the averFurnace* contract come
    // from. Taking the environment from the shipped prelude rather than a private constant is what
    // makes the furnace a measurement of the engine and not of a test rig.
    sd.prelude = rhi::sharedShaderPrelude();
    sd.entry   = "CSPathTrace";
    sd.stage   = rhi::ShaderStage::Compute;
    sd.minShaderModel = kShaderModel;
    cs_ = res_->createShader(sd);
    if (!cs_) {
        // HLSL is compiled at RUNTIME by DXC, so this is the only place a shader error can surface.
        AVER_ERROR("[PT] the integrator would not compile");
        shutdown();
        return false;
    }

    rhi::ComputePipelineDesc pd;
    pd.cs = cs_;
    pd.layout.srvCount = kSrvCount;
    pd.layout.uavCount = kUavCount;
    // b4 stays a ROOT CBV (zero dwords): the block is 112 bytes, which is more than root constants
    // should carry, and setConstantBuffer suballocates it from the frame's upload ring.
    pd.layout.constantDwords[rhi::kFeatureFrameConstantRegister] = 0;
    pipeline_ = res_->createComputePipeline(pd);
    if (!pipeline_) { AVER_ERROR("[PT] integrator pipeline unavailable"); shutdown(); return false; }

    AVER_INFO("[PT] path tracer ready (RayQuery, SM {}, {}x{} threads per group)",
              kShaderModel, kGroup, kGroup);
    return true;
}

void PathTracer::shutdown() {
    if (res_) {
        if (verts_)       res_->destroyBuffer(verts_);
        if (indices_)     res_->destroyBuffer(indices_);
        if (instanceBuf_) res_->destroyBuffer(instanceBuf_);
        // BLAS handles ARE released, and THIS is now the only place that does it: they are owned by
        // blasCache_, which deliberately survives resetScene() (see its own comment). Iterating the
        // CACHE rather than blas_ is what makes that correct -- blas_ holds only the meshes in the
        // last snapshot, so freeing that instead would leak every mesh that had left the view.
        //
        // This function used not to free them at all, which was inert while the only caller was
        // process exit and became a per-toggle leak once a live editor could shut down and re-init a
        // PathTracer repeatedly.
        for (const auto& [mesh, b] : blasCache_) if (b) res_->destroyBlas(b);
        // THE TLASES STILL CANNOT BE RELEASED: the RHI has no destroyTlas at all (see
        // IResourceFactory -- BLAS has one, TLAS does not), so what this object allocated outlives
        // it, for the life of the DEVICE. What changed is the COUNT: addScene reuses a handle PER
        // SCENE SLOT and grows it by doubling, so a session leaks O(log n) per slot rather than one
        // per re-arm. Dropping the vectors here forgets the handles rather than freeing them, which
        // is all that can be done; removing the last of it needs a real destroyTlas added to the
        // RHI, a wider change than this module.
        tlasPool_.clear();
        tlasPoolCap_.clear();
        if (pipeline_)    res_->destroyPipeline(pipeline_);
        if (cs_)          res_->destroyShader(cs_);
    }
    blasCache_.clear();
    // CLEARED WITH THE BUFFERS IT DESCRIBES. Leaving it populated would let the next prepare() after
    // a re-init match its mesh set against a table whose verts_/indices_ were just destroyed, and
    // reuse two freed handles -- the exact hazard the geoMeshes_/verts_ pair has to be kept in step
    // to avoid, which is why resetScene() clears NEITHER and this clears BOTH.
    geoMeshes_.clear();
    geoVerts_ = geoIndices_ = 0;
    geometryReused_ = false;
    verts_ = indices_ = instanceBuf_ = 0;
    pipeline_ = 0;
    cs_ = 0;
    surfaces_.clear();
    instances_.clear();
    scenes_.clear();
    blas_.clear();
    totalVerts_ = totalIndices_ = 0;
    prepared_ = false;
    built_ = false;
    dev_ = nullptr;
    res_ = nullptr;
}

u32 PathTracer::addSurface(const PtSurface& s) {
    surfaces_.push_back(s);
    return static_cast<u32>(surfaces_.size() - 1);
}

u32 PathTracer::addScene(const u32* surfaceIds, u32 count) {
    Scene s;
    s.surfaces.assign(surfaceIds, surfaceIds + count);
    if (res_) {
        // REUSED PER SLOT, NOT RECREATED, AND NOT SHARED BETWEEN SCENES -- see tlasPool_'s
        // declaration for both halves of that. Reuse is what stops a re-arm leaking a handle; the
        // per-slot part is what stops five simultaneous scenes all tracing the same geometry.
        //
        // Grows to the next power of two so a snapshot that gains a single instance does not
        // allocate again; shrinks never, because a smaller TLAS buys nothing and every allocation
        // here is permanent until the device goes.
        const usize slot = scenes_.size();
        if (tlasPool_.size() <= slot) { tlasPool_.resize(slot + 1, 0); tlasPoolCap_.resize(slot + 1, 0); }

        const u32 need = count ? count : 1;
        if (!tlasPool_[slot] || tlasPoolCap_[slot] < need) {
            u32 cap = tlasPoolCap_[slot] ? tlasPoolCap_[slot] : 1;
            while (cap < need) cap <<= 1;
            const rhi::TlasHandle grown = res_->createTlas(cap);
            // A failed grow keeps the old handle rather than dropping to zero: too small is a
            // degraded scene, but zero is no scene at all, and prepare() would then build nothing
            // while reporting success.
            if (grown) { tlasPool_[slot] = grown; tlasPoolCap_[slot] = cap; }
        }
        s.tlas = tlasPool_[slot];
    }
    scenes_.push_back(std::move(s));
    return static_cast<u32>(scenes_.size() - 1);
}

bool PathTracer::prepare() {
    if (!res_ || !dev_ || surfaces_.empty()) return false;
    if (prepared_) return true;
    const auto tPrep0 = std::chrono::steady_clock::now();

    // Where each surface's geometry lands in the flat table. Per SURFACE and not per mesh: two
    // surfaces may share a mesh (the same quad with two albedos is exactly that case), and giving
    // them separate rows costs a few vertices and removes a deduplication that could go wrong.
    instances_.resize(surfaces_.size());
    surfaceRow_.assign(surfaces_.size(), 0);
    meshRows_.clear();
    totalVerts_ = totalIndices_ = 0;
    // Mesh handle -> row. Local to this call: the rows are rebuilt from scratch on every prepare(),
    // so this is a deduplication within one snapshot, never a cache across snapshots.
    std::unordered_map<rhi::MeshHandle, u32> rowOf;
    rowOf.reserve(surfaces_.size());
    for (usize i = 0; i < surfaces_.size(); ++i) {
        const rhi::MeshHandle mesh = surfaces_[i].mesh;
        auto found = rowOf.find(mesh);
        if (found == rowOf.end()) {
            rhi::BufferHandle vb = 0, ib = 0;
            u32 vc = 0, ic = 0;
            if (!dev_->meshGeometry(mesh, &vb, &ib, &vc, &ic)) {
                AVER_ERROR("[PT] surface {} has no readable geometry; the backend cannot express it", i);
                return false;
            }
            MeshRow row;
            row.mesh        = mesh;
            row.firstVertex = totalVerts_;
            row.firstIndex  = totalIndices_;
            row.vertexCount = vc;
            row.indexCount  = ic;
            totalVerts_   += vc;
            totalIndices_ += ic;
            found = rowOf.emplace(mesh, static_cast<u32>(meshRows_.size())).first;
            meshRows_.push_back(row);
        }
        const MeshRow& row = meshRows_[found->second];
        surfaceRow_[i] = found->second;
        Instance& inst = instances_[i];
        std::memcpy(inst.objectToWorld, surfaces_[i].world, sizeof(inst.objectToWorld));
        std::memcpy(inst.albedo, surfaces_[i].albedo, sizeof(inst.albedo));
        inst.ior = surfaces_[i].ior;
        // The SHARED rows: every surface on this mesh reads the same geometry. What stays per
        // surface is objectToWorld, albedo and ior, right here.
        inst.firstVertex = row.firstVertex;
        inst.firstIndex  = row.firstIndex;
    }
    if (totalVerts_ == 0 || totalIndices_ == 0) return false;

    // THE SAME MESHES IN THE SAME ORDER means the table this call just laid out is byte-for-byte the
    // one verts_/indices_ already hold -- every firstVertex/firstIndex above was recomputed from the
    // same walk -- so both the allocation and buildScenes' copy pass can be skipped entirely. See
    // geoMeshes_ for why comparing handles is sufficient to know the geometry itself is unchanged.
    std::vector<rhi::MeshHandle> meshOrder;
    meshOrder.reserve(meshRows_.size());
    for (const MeshRow& row : meshRows_) meshOrder.push_back(row.mesh);
    // BOTH the mesh set AND the totals it implies. The set alone is what the argument above rests
    // on -- same meshes in the same order means the same offsets -- but the totals are what the SRV
    // is actually declared with (setSrvBuffer, in createTarget), so they are checked independently
    // rather than assumed to follow. A mismatch here would mean declaring more vertices than the
    // buffer holds, which is a GPU out-of-bounds read rather than a wrong picture; falling back to a
    // rebuild costs 4ms and cannot do that.
    geometryReused_ = verts_ && indices_ && meshOrder == geoMeshes_ &&
                      totalVerts_ == geoVerts_ && totalIndices_ == geoIndices_;

    if (!geometryReused_) {
        // A DIFFERENT mesh set: the offsets have moved, so whatever the old buffers hold is wrong.
        // Released here rather than in resetScene(), which now deliberately keeps them -- this is
        // the one place that knows whether they are still correct.
        if (verts_)   res_->destroyBuffer(verts_);
        if (indices_) res_->destroyBuffer(indices_);
        verts_ = indices_ = 0;

        rhi::BufferDesc vd;
        vd.bytes = static_cast<u64>(totalVerts_) * sizeof(rhi::MeshVertex);
        vd.kind  = rhi::BufferKind::Default;
        vd.debugName = "pt vertices";
        verts_ = res_->createBuffer(vd);

        rhi::BufferDesc id;
        id.bytes = static_cast<u64>(totalIndices_) * sizeof(u32);
        id.kind  = rhi::BufferKind::Default;
        id.debugName = "pt indices";
        indices_ = res_->createBuffer(id);

        geoMeshes_  = std::move(meshOrder);
        geoVerts_   = totalVerts_;
        geoIndices_ = totalIndices_;
    }

    // Upload rather than Default: it is written once from the CPU and never by the GPU, so there is
    // no copy to schedule and no state to walk.
    rhi::BufferDesc nd;
    nd.bytes = sizeof(Instance) * instances_.size();
    nd.kind  = rhi::BufferKind::Upload;
    nd.debugName = "pt instances";
    instanceBuf_ = res_->createBuffer(nd);

    if (!verts_ || !indices_ || !instanceBuf_) {
        AVER_ERROR("[PT] could not allocate the flat geometry table");
        return false;
    }
    res_->writeBuffer(instanceBuf_, instances_.data(), nd.bytes);

    // One bottom-level structure per DISTINCT MESH. A BLAS describes geometry in object space and
    // nothing else -- the instance transform lives in the TLAS -- so two surfaces on one mesh have
    // always been able to share one, and building it twice was pure waste. See MeshRow for the
    // measurement that made that waste the dominant cost of the whole feature.
    const auto tBuf = std::chrono::steady_clock::now();
    blas_.assign(meshRows_.size(), 0);
    blasFresh_.assign(meshRows_.size(), 0);
    u32 reused = 0;
    for (usize i = 0; i < meshRows_.size(); ++i) {
        const rhi::MeshHandle mesh = meshRows_[i].mesh;
        if (const auto hit = blasCache_.find(mesh); hit != blasCache_.end()) {
            // Reached only for a mesh whose meshGeometry() succeeded above, which is the liveness
            // test the whole cache rests on -- see blasCache_ for why that is sufficient.
            blas_[i] = hit->second;
            ++reused;
            continue;
        }
        blas_[i] = res_->createBlas(mesh);
        if (!blas_[i]) { AVER_ERROR("[PT] no bottom-level structure for mesh {}", mesh); return false; }
        blasFresh_[i] = 1;
        blasCache_.emplace(mesh, blas_[i]);
    }
    // Logged with its TIMING because this function was, measured, the entire cost of the feature:
    // 2.2 SECONDS per re-arm before geometry was deduplicated by mesh and structures were cached,
    // and a re-arm fires whenever the visible set changes. Anyone who makes this slow again should
    // find out from the log rather than from the frame rate.
    const auto tBlas = std::chrono::steady_clock::now();
    AVER_INFO("[PT] flat table: {} surface(s) over {} distinct mesh(es), {} vertices, {} indices"
              " | geometry {}, buffers {:.1f}ms, createBlas {:.1f}ms ({} built, {} reused)",
              static_cast<u32>(surfaces_.size()), static_cast<u32>(meshRows_.size()),
              totalVerts_, totalIndices_,
              geometryReused_ ? "REUSED" : "rebuilt",
              std::chrono::duration<f64, std::milli>(tBuf - tPrep0).count(),
              std::chrono::duration<f64, std::milli>(tBlas - tBuf).count(),
              static_cast<u32>(meshRows_.size()) - reused, reused);
    for (const Scene& s : scenes_) if (!s.tlas) {
        AVER_ERROR("[PT] a scene has no top-level structure");
        return false;
    }

    prepared_ = true;
    return true;
}

bool PathTracer::buildScenes(rhi::IRenderContext& ctx) {
    if (!prepared_ || built_) return built_;

    ctx.pushMarker("Aver.PathTracer build");
    // ONLY THE NEW ONES. A built BLAS stays built until it is destroyed, and re-tracing the same
    // geometry on every re-arm is GPU work whose result is bit-identical to what is already there.
    for (usize i = 0; i < blas_.size(); ++i) if (blasFresh_[i]) ctx.buildBlas(blas_[i]);

    // BOTH transitions are explicit. D3D12 would promote a Common buffer to CopyDest by itself, but
    // the RHI tracks buffer state to catch exactly this class of mistake and does not model
    // promotion, so an implicit promotion followed by an explicit walk-back is a barrier claiming a
    // state the tracker never saw it enter.
    // SKIPPED ENTIRELY when prepare() kept the previous table: the buffers already hold this exact
    // geometry at these exact offsets, so the copy would write identical bytes over themselves. The
    // BARRIERS are skipped with it, deliberately -- transitioning a buffer nothing is about to write
    // would be a barrier claiming a state the RHI's tracker never saw it enter, which is the same
    // mistake the explicit-transition comment below exists to prevent.
    if (!geometryReused_) {
        // BOTH transitions are explicit. D3D12 would promote a Common buffer to CopyDest by itself,
        // but the RHI tracks buffer state to catch exactly this class of mistake and does not model
        // promotion, so an implicit promotion followed by an explicit walk-back is a barrier
        // claiming a state the tracker never saw it enter.
        ctx.bufferBarrier(verts_,   rhi::ResourceState::Common, rhi::ResourceState::CopyDest);
        ctx.bufferBarrier(indices_, rhi::ResourceState::Common, rhi::ResourceState::CopyDest);
        // Per DISTINCT MESH, matching the table prepare() laid out. Copying per surface duplicated
        // the same blob once per instance -- 31.2M vertices where 2M would do.
        for (const MeshRow& row : meshRows_) {
            rhi::BufferHandle vb = 0, ib = 0;
            u32 vc = 0, ic = 0;
            if (!dev_->meshGeometry(row.mesh, &vb, &ib, &vc, &ic)) { ctx.popMarker(); return false; }
            ctx.copyBuffer(verts_, vb, static_cast<u64>(vc) * sizeof(rhi::MeshVertex),
                           static_cast<u64>(row.firstVertex) * sizeof(rhi::MeshVertex), 0);
            ctx.copyBuffer(indices_, ib, static_cast<u64>(ic) * sizeof(u32),
                           static_cast<u64>(row.firstIndex) * sizeof(u32), 0);
        }
        ctx.bufferBarrier(verts_,   rhi::ResourceState::CopyDest, rhi::ResourceState::Common);
        ctx.bufferBarrier(indices_, rhi::ResourceState::CopyDest, rhi::ResourceState::Common);
    }

    for (const Scene& s : scenes_) {
        std::vector<rhi::TlasInstance> inst;
        inst.reserve(s.surfaces.size());
        for (u32 id : s.surfaces) {
            if (id >= surfaces_.size()) continue;
            const u32 row = surfaceRow_[id];
            if (row >= blas_.size() || !blas_[row]) continue;
            rhi::TlasInstance i;
            std::memcpy(i.world, surfaces_[id].world, sizeof(i.world));
            i.mask = 0xFF;
            i.blas = blas_[row];
            // The id a hit reads back. It indexes the SHARED instance table, so a scene holding a
            // subset of the surfaces still resolves each hit to the right geometry and albedo --
            // which CommittedInstanceIndex, a position inside this one structure, could not do.
            i.instanceId = id & rhi::kMaxTlasInstanceId;
            inst.push_back(i);
        }
        if (inst.empty()) { ctx.popMarker(); return false; }
        ctx.buildTlas(s.tlas, inst.data(), static_cast<u32>(inst.size()));
    }
    ctx.popMarker();

    built_ = true;
    AVER_INFO("[PT] {} surfaces, {} scenes, {} vertices and {} indices in the flat table",
              surfaces_.size(), scenes_.size(), totalVerts_, totalIndices_);
    return true;
}

bool PathTracer::createTarget(u32 scene, u32 width, u32 height, PtTarget& out) {
    out = {};
    if (!res_ || !prepared_ || scene >= scenes_.size() || width == 0 || height == 0) return false;

    rhi::BufferDesc ad;
    ad.bytes = static_cast<u64>(width) * height * kPtAccumElementsPerPixel * kPtAccumStride;
    ad.kind  = rhi::BufferKind::Default;
    ad.allowUnorderedAccess = true;
    ad.debugName = "pt accumulator";
    out.accum = res_->createBuffer(ad);

    rhi::BindingSetDesc bd;
    bd.srvCount = kSrvCount;
    bd.uavCount = kUavCount;
    bd.srvKinds[0] = rhi::SlotKind::AccelerationStructure;
    bd.srvKinds[1] = bd.srvKinds[2] = bd.srvKinds[3] = rhi::SlotKind::StructuredBuffer;
    bd.uavKinds[0] = rhi::SlotKind::StructuredBuffer;
    out.set = res_->createBindingSet(bd);

    if (!out.accum || !out.set) {
        AVER_ERROR("[PT] could not allocate an accumulation target");
        destroyTarget(out);
        return false;
    }

    // ONE BINDING SET PER TARGET, and never one shared set rebound between dispatches: a descriptor
    // table is read when the command EXECUTES, so rewriting a shared set between two recorded
    // dispatches would give both of them the last scene written.
    res_->setSrvTlas(out.set, 0, scenes_[scene].tlas);
    res_->setSrvBuffer(out.set, 1, verts_, sizeof(rhi::MeshVertex), totalVerts_, 0);
    res_->setSrvBuffer(out.set, 2, indices_, sizeof(u32), totalIndices_, 0);
    res_->setSrvBuffer(out.set, 3, instanceBuf_, sizeof(Instance),
                       static_cast<u32>(instances_.size()), 0);
    res_->setUavBuffer(out.set, 0, out.accum, kPtAccumStride,
                       width * height * kPtAccumElementsPerPixel, 0);

    out.width = width;
    out.height = height;
    out.scene = scene;
    return true;
}

void PathTracer::resetScene() {
    if (res_) {
        // ONLY the per-surface buffer. verts_/indices_ are NOT released here any more: they belong
        // to the geometry table keyed by geoMeshes_, which outlives a snapshot for the same reason
        // blasCache_ does (see both of their comments). prepare() is the one place that can tell
        // whether they are still correct, and it releases them itself when the mesh set has moved;
        // shutdown() frees whatever is left. instanceBuf_ genuinely IS per snapshot -- it is one
        // record per SURFACE, and the surface set is exactly what changes.
        if (instanceBuf_) res_->destroyBuffer(instanceBuf_);
        // BLAS handles are NOT released here either, for the same reason.
        // THE TLAS POOL IS KEPT, DELIBERATELY, and that is the point of the change: clearing
        // scenes_ below drops the Scenes that referenced them, but tlasPool_ itself survives, so the
        // next round of addScene() calls reuses slot 0, slot 1, ... in the same order instead of
        // allocating structures that can never be freed. See tlasPool_'s own declaration for what
        // that used to cost, and for why the pool is per SLOT rather than a single shared handle.
    }
    instanceBuf_ = 0;
    surfaces_.clear();
    instances_.clear();
    scenes_.clear();
    blas_.clear();
    meshRows_.clear();
    surfaceRow_.clear();
    totalVerts_ = totalIndices_ = 0;
    prepared_ = false;
    built_ = false;
}

void PathTracer::destroyTarget(PtTarget& t) {
    if (res_) {
        if (t.accum) res_->destroyBuffer(t.accum);
        if (t.set)   res_->destroyBindingSet(t.set);
    }
    t = {};
}

void PathTracer::accumulate(rhi::IRenderContext& ctx, const PtTarget& t, const PtCamera& cam,
                            const PtDispatch& d) {
    if (!pipeline_ || !built_ || !t.valid()) return;

    FrameCB cb{};
    for (u32 i = 0; i < 3; ++i) {
        cb.origin[i]  = cam.origin[i];
        cb.forward[i] = cam.forward[i];
        cb.right[i]   = cam.right[i];
        cb.up[i]      = cam.up[i];
    }
    cb.origin[3]  = cam.tanHalfFov;
    cb.forward[3] = cam.aspect;
    cb.image[0] = t.width;
    cb.image[1] = t.height;
    cb.image[2] = d.maxBounces;
    cb.image[3] = static_cast<u32>(d.defect);
    cb.sample[0] = d.firstSample;
    cb.sample[1] = d.samples;
    cb.sample[2] = d.reset ? 1u : 0u;
    cb.trace[0] = d.rayBias;
    cb.trace[1] = d.tMax;

    ctx.pushMarker("Aver.PathTracer");
    ctx.bufferBarrier(t.accum, rhi::ResourceState::Common, rhi::ResourceState::UnorderedAccess);
    ctx.setPipeline(pipeline_);
    ctx.setBindingSet(t.set);
    ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb, sizeof(cb));
    ctx.dispatch((t.width + kGroup - 1) / kGroup, (t.height + kGroup - 1) / kGroup, 1);
    // Back to Common before the frame ends: a buffer's state does not survive the command list.
    ctx.bufferBarrier(t.accum, rhi::ResourceState::UnorderedAccess, rhi::ResourceState::Common);
    ctx.popMarker();
}

void PathTracer::copyForReadback(rhi::IRenderContext& ctx, const PtTarget& t,
                                 rhi::BufferHandle readback, u64 dstOffset) {
    if (!t.valid() || !readback) return;
    ctx.bufferBarrier(t.accum, rhi::ResourceState::Common, rhi::ResourceState::CopySource);
    ctx.copyBuffer(readback, t.accum, t.bytes(), dstOffset, 0);
    ctx.bufferBarrier(t.accum, rhi::ResourceState::CopySource, rhi::ResourceState::Common);
}

} // namespace aver::pt
