// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
#pragma once
#include "aver/rhi/RHIResources.hpp"

// Every pipeline handle VoxiRenderer owns, grouped by the build that makes it (docs/rendering/ASYNC_SHADERS.md).
// A group is built together in one pipeline batch and replaces its previous handles together, at a frame
// boundary, so a frame never binds a mix of old and new.
//
//   Base   independent of the render targets: shadow maps, voxelisation, volume passes, air visibility.
//   Scene  bakes the sample count and target formats, the material graphs and the shader text: the lit
//          variants, ray-driven primary visibility and the staged ray-driven passes.
//   Rc     the radiance-cache (NeuRaC) twins of the GI passes. Built on first use.
//   Pt     the Path Tracing twins. Built on first use.
//   Nrd2   Stage B's NRD2 variant and the half-rate fill (the denoiser's own pipelines live in Nrd2).
//          Built on first use.
//
// Each X(name) names a VoxiRenderer member; PsoLocal below mirrors them as batch-local handles.
#define AVER_VOXI_PSO_BASE(X) \
    X(shadowPso_) X(shadowInstancedPso_) X(giShadowPso_) X(giShadowInstancedPso_) X(voxelPso_) X(voxelMsPso_) \
    X(clearPso_) X(resolvePso_) X(mipPso_) X(airVisPso_)

#define AVER_VOXI_PSO_SCENE(X) \
    X(debugPso_) X(scenePso_) X(sceneMsPso_) X(sceneRtPso_) X(sceneMsRtPso_) \
    X(sceneBlendedPso_) X(sceneMsBlendedPso_) X(sceneRtBlendedPso_) X(sceneMsRtBlendedPso_) \
    X(depthPrepassPso_) X(scenePsoPrepassed_) X(sceneRtPsoPrepassed_) X(rayDrivenPso_) \
    X(sceneGbufPso_) X(sceneMsGbufPso_) X(sceneRtGbufPso_) X(sceneMsRtGbufPso_) \
    X(scenePsoPrepassedGbuf_) X(sceneRtPsoPrepassedGbuf_) X(rayDrivenGbufPso_) \
    X(rayDrivenTexPso_) X(rayDrivenTexGbufPso_) X(sceneRtBlendedTexPso_) \
    X(rdVisCsPso_) X(rdShadowCsPso_) X(rdGiCsPso_) X(rdGiCbCsPso_) X(rdSkyOccCsPso_) X(rdReflCsPso_) \
    X(rdShadowProbeCsPso_) X(rdShadowTiledCsPso_) X(rdTailVisCsPso_) X(rdTailFilterCsPso_) \
    X(rdGiTraceCsPso_) X(rdGiTraceCbCsPso_) X(rdGiSplitCsPso_) X(rdGiSplitCbCsPso_) \
    X(rdReflSplitCsPso_) X(rdReflFilterCsPso_) X(rayDrivenSplitTexPso_) X(rayDrivenSplitTexGbufPso_)

#define AVER_VOXI_PSO_RC(X) \
    X(rdGiCacheCsPso_) X(rdGiCacheCbCsPso_) X(rdGiTraceCacheCsPso_) X(rdGiTraceCacheCbCsPso_)

#define AVER_VOXI_PSO_PT(X) \
    X(rdGiPtCsPso_) X(rdGiPtCbCsPso_) X(rdGiTracePtCsPso_) X(rdGiTracePtCbCsPso_) \
    X(rdGiPtRcCsPso_) X(rdGiTracePtRcCsPso_) X(rdGiPtRcCbCsPso_) X(rdGiTracePtRcCbCsPso_) \
    X(rdPtRefCsPso_) X(rdReflPtRcCsPso_) X(rdReflSplitPtRcCsPso_) X(rdReflPtCsPso_) X(rdReflSplitPtCsPso_)

#define AVER_VOXI_PSO_NRD2(X) X(rayDrivenSplitNrd2Pso_) X(rdHalfFillCsPso_)

namespace aver::voxi {

// Which pipeline groups a build covers (bit mask).
enum PsoGroup : unsigned {
    kPsoBase  = 1u << 0,
    kPsoScene = 1u << 1,
    kPsoRc    = 1u << 2,
    kPsoPt    = 1u << 3,
    kPsoNrd2  = 1u << 4,
};

// Batch-local handles a build records: nonzero means "requested", resolved to a factory handle at adoption.
struct PsoLocal {
#define AVER_VOXI_DECL(n) rhi::PipelineHandle n = 0;
    AVER_VOXI_PSO_BASE(AVER_VOXI_DECL)
    AVER_VOXI_PSO_SCENE(AVER_VOXI_DECL)
    AVER_VOXI_PSO_RC(AVER_VOXI_DECL)
    AVER_VOXI_PSO_PT(AVER_VOXI_DECL)
    AVER_VOXI_PSO_NRD2(AVER_VOXI_DECL)
#undef AVER_VOXI_DECL
};

}  // namespace aver::voxi
