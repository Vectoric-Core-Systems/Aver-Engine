// Mesh LOD / cluster selection and mesh-shader path: toggles, stats and per-mesh LOD ladders and cluster tables.
// Included from SandboxApp.hpp (inside namespace aver) after the engine headers; not a standalone header.
#pragma once

struct LodState {
#if AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR
    // Per-mesh LOD ladder (virtualized-geometry selection). Indexed by LEVEL.
    struct MeshLodLadder {
        std::vector<rhi::MeshHandle> handles;   // [level] -> MeshHandle.
        std::vector<u32> triCounts;              // [level] -> triangle count.
        std::vector<f32> errorCm;                // [level] -> world error.
        // [level] -> meshlets (bounds + cone, resident for telemetry).
        std::vector<std::vector<trifactor::ClusterView>> clusters;
    };
    std::unordered_map<u64, MeshLodLadder> meshLods_;
#endif
    // LOD knobs (persisted via RENDER.LODSELECT/LODTHRESHOLDPX in project settings).
    bool lodSelectEnabled_ = true;      // ON by default (--lod-select / editor toggle).
    f32  lodErrorThresholdPx_ = 1.0f;   // Projected screen error tolerance (--lod-error-px).
#if AVER_MODULE_SCENE && AVER_MODULE_TRIFACTOR
    // LOD cluster stats (informational per-meshlet telemetry, CPU cost). OFF by default.
    bool lodClusterStatsEnabled_ = false;
    // This frame's selection counters (logged under "[LOD-SELECT]").
    struct LodSelectStats {
        u32 instancesTested = 0;
        u32 levelCollapsed = 0;        // Instances drew level > 0 this frame.
        u32 clustersTested = 0;        // Sum of chosen-level meshlet counts.
        u32 frustumCulled = 0;         // Informational cull count.
        u32 coneCulled = 0;            // Informational cull count.
        u64 trianglesBeforeLod0 = 0;   // Sum of LOD-0 triangle counts.
        u64 trianglesAfterLevel = 0;   // Sum of chosen level's triangle counts (what draws).
    };
    LodSelectStats lodStats_{};
    LodSelectStats lastLoggedLodStats_{};
    // Per-cluster LOD selection (virtualized geometry). Switchable via --lod-per-cluster.
    // Every meshlet across all LOD levels, mesh-local space, plus expanded global index list.
    struct MeshClusterData {
        std::vector<trifactor::MeshClusterView> clusters;   // Mesh-local; all levels.
        std::vector<std::vector<u32>> clusterIndices;        // [clusterId] -> expanded indices.
        std::vector<rhi::MeshVertex> verts;                  // Vertex copy (used by all clusters).

        // Instance-level shortcut precomputed inputs (built once at load).
        std::vector<trifactor::MeshClusterLevelBounds> levelBounds;
        f32 maxSphereRadius = 0.0f;   // Largest cluster sphere radius (mesh-local).
    };
    std::unordered_map<u64, MeshClusterData> meshClusterData_;
    // Per-instance cut-assembled MeshHandle (per instance, not per mesh). Rebuilt on cluster selection change.
    struct ClusterCutCache {
        std::vector<u32> selectedIds;   // Sorted; last frame's cut.
        rhi::MeshHandle handle = 0;     // 0 = none or selection was empty.
        u64 lastUsedFrame = 0;
        u64 rebuildCount = 0;           // How many times this instance's handle was rebuilt.
    };
    std::unordered_map<scene::Entity, ClusterCutCache> clusterCutCache_;
    u64 lodClusterFrame_ = 0;           // Incremented per onRender (drives sweep).
    bool lodPerClusterEnabled_ = false; // --lod-per-cluster. OFF = per-level path runs.
    // CPU-assembly upload cost measurement. Real std::chrono timing per createMesh call.
    struct LodClusterStats {
        u32 instancesTested = 0;
        u32 clustersTested = 0;
        u32 frustumCulled = 0;
        u32 coneCulled = 0;
        u32 lodRejected = 0;
        u32 clustersDrawn = 0;
        u64 trianglesBeforeLod0 = 0;
        u64 trianglesDrawn = 0;
        u32 instancesMixedLevels = 0;   // Instances with cut spanning > 1 level.
        u32 maxDistinctLevelsSeen = 0;  // Highest distinct-level count on any instance.
        u32 rebuilds = 0;               // Cache misses (cut changed) this frame.
        u32 cacheHits = 0;              // Cache hits (cut unchanged) this frame.
        u64 rebuildIndices = 0;         // Sum of assembled index counts on rebuilds.
        f64 rebuildMs = 0.0;            // Wall time in createMesh on rebuilds this frame.
        u32 instancesShortcut = 0;      // provablySingleLevelCut shortcuts this instance.
    };
    LodClusterStats lodClusterStats_{};
    LodClusterStats lastLoggedLodClusterStats_{};
    // Per-mesh GPU cluster buffers (built once at load, only with --lod-mesh-shader).
    struct MeshClusterGpu {
        rhi::BufferHandle bounds = 0;   // ClusterBounds[] (t0).
        rhi::BufferHandle desc   = 0;   // ClusterMeshletDesc[] (t1).
        rhi::BufferHandle verts  = 0;   // Global vertex indices (t2).
        rhi::BufferHandle tris   = 0;   // Packed local triangles (t3).
        rhi::BindingSetHandle bindingSet = 0;
        u32 clusterCount = 0;           // All levels' clusters concatenated.
    };
    std::unordered_map<u64, MeshClusterGpu> meshClusterGpu_;
    bool lodMeshShaderEnabled_ = false;   // Resolved in onInit; see lodMeshShaderRequest_.
    // -1 = decide from DeviceCaps, 0 = --no-lod-mesh-shader, 1 = --lod-mesh-shader.
    int lodMeshShaderRequest_ = -1;
    // Pipeline creation: lazy, tried once. Tier-0 devices degrade to CPU path.
    bool lodMeshPipelineTried_ = false;
    bool lodMeshPipelineReady_ = false;
    // MaterialGraphRegistry revision at last compile (for latch reopening).
    u64  lodMeshPipelineGraphRev_ = ~0ull;
    rhi::ShaderHandle lodMeshAsShader_ = 0, lodMeshMsShader_ = 0, lodMeshPsShader_ = 0;
    rhi::PipelineHandle lodMeshPipeline_ = 0;
    rhi::PipelineLayout lodMeshLayout_{};   // srvCount=4 (the four cluster buffers).
    // Per-instance constant block for ASMain/MSClusterMain (ClusterFrameCB, b4 root CBV).
    struct ClusterFrameCB {
        f32 budgetPx = 0.0f;
        f32 projScale = 0.0f;
        f32 worldScale = 1.0f;
        u32 _pad = 0;
        f32 planes[6][4] = {};
    };
    // Informational counters for --lod-mesh-shader ("[LOD-MESH-SHADER]"). CPU-mirrored, not GPU readback.
    struct LodMeshShaderStats {
        u32 instancesTested = 0;
        u32 clustersDispatched = 0;      // Sum of MeshClusterGpu::clusterCount.
        u32 survivors = 0;               // Sum of survivor counts (CPU-mirrored).
        u64 trianglesDrawn = 0;
        u32 instancesMixedLevels = 0;
        u32 maxDistinctLevelsSeen = 0;
        u32 instancesShortcut = 0;       // Used provablySingleLevelCut shortcut.
    };
    LodMeshShaderStats lodMeshShaderStats_{};
    LodMeshShaderStats lastLoggedLodMeshShaderStats_{};
#endif
};
