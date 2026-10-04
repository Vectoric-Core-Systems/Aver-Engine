#pragma once
// The pixel shader for the GPU per-cluster mesh-shader path, with materials, and (Stage 3, when
// AVER_MODULE_VOXI is compiled in) real shadows and real GI.
//
// WHY THIS IS NOT IN THE SHARED PRELUDE, where its predecessor lived. rhi::sharedShaderPrelude()
// is compiled FIRST, before pbr::materialShaderPrelude(), so anything written inside it cannot call
// averEvalMaterial -- the material functions do not exist yet at that point in the translation
// unit. The old PSClusterMain was a one-liner into plainShadeSurface for exactly that reason, and
// that is why every plant on this path drew as a black silhouette: plainShadeSurface shades from
// gBaseColor/gMaterial, per-object constants, and never samples a texture.
//
// So this source lives with the feature that owns the pipeline and is appended AFTER both preludes,
// which is the composition order PbrShaders.hpp documents and the one Voxi already uses for
// VoxiShaders.hpp.
//
// THE HALF STAGE 3 DID NOT BUY, NOW CLOSED -- BUT NOT FROM THIS FILE. This shader SAMPLES the cascade
// map and the voxel volume, and it does. The geometry it draws USED TO be absent FROM both, because
// IRenderFeature::submitDraw is called from exactly one place -- D3D12Device::drawMesh -- and this
// path dispatches clusters itself and skips that call by design. A cluster-drawn plant RECEIVED
// shadow and GI and CAST neither. That was a scene-submission gap, not a shading one, so nothing in
// this file could fix it -- and nothing in this file did: SandboxApp.cpp's cluster-dispatch branch now
// calls VoxiRenderer::submit() itself, right after dispatchMeshClusters (search "DEFECT 2's FIX" in
// SandboxApp.cpp), handing Voxi the same (mesh, world, material) shape any ordinary drawMesh() instance
// already does. That gets a cheap depth-only proxy of this geometry into the shadow and voxelise
// passes without this pixel shader, or PSClusterMain's own lit-pass shading, changing at all.
//
// WHAT IT DELIBERATELY DOES NOT DO, STILL, EVEN AFTER STAGE 3. No ray tracing, ever -- shadowFactor()
// and coneTracedIndirect() (borrowed from Voxi via VoxiGiShaders.hpp's giShaderPrelude(), merged
// into this pipeline's own table 0 rather than needing a third -- see D3D12Device.cpp's nullFill for
// why that merge is D3D12 only) are Voxi's OWN non-ray-traced fallback path, the same one PSMainVoxi
// itself runs when ray tracing is off or its acceleration structure is not built. On a project that
// DOES have ray tracing on, ordinary draws get ray-traced shadows/reflections and cluster-path draws
// still get the cascade map and the voxel cone -- a real, visible difference, not merely an
// unmeasured one. And without AVER_MODULE_VOXI compiled in at all, this file still has no shadow
// lookup and no GI cone trace to fall back on: sun.visibility is 1.0 and the indirect diffuse term
// is zero, exactly Stage 2's neutral stand-in. Either way, the result is textured, sun-lit, sky-
// ambient foliage that is closer to the ordinary path than Stage 2 left it, and still NOT full
// parity with it -- see the header comment on lodMeshShaderEnabled_.
#include <string_view>

namespace aver::sandbox {

// Compiled as: rhi::sharedShaderPrelude() + pbr::materialShaderPrelude() +
// (voxi::giShaderPrelude(), only when AVER_MODULE_VOXI) + this. Every symbol used below comes from
// one of those, and none of them sit behind a feature macro of their own: averSunRadiance/
// averSkyIrradiance/skyColor/averApplyFog and VSOut are unguarded in the shared prelude, the aver*
// material entry points are unguarded in the material prelude, and shadowFactor()/
// coneTracedIndirect() are unguarded in Voxi's borrowed one -- AVER_CLUSTER_VOXI below is what
// SandboxApp.cpp defines only when that third prelude was actually appended, so this file is the
// one place that decides whether to call them at all.
// The cluster PS now lives in sandbox/shaders/cluster_material.hlsl, loaded through
// rhi::shaderFile() at its one call site in SandboxApp.cpp.

} // namespace aver::sandbox
