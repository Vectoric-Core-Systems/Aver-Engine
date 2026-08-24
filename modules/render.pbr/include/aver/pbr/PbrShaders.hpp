#pragma once

// The material system's HLSL, owned by Aver.Render.PBR.Materials. A renderer compiles
// rhi::sharedShaderPrelude() + pbr::materialShaderPrelude() + its own source, in that order.
#include "aver/core/Types.hpp"

#include <string>

namespace aver::pbr {

// The material HLSL, with static storage duration: the pointer is good for the whole process.
//
// DEFINE AVER_MATERIAL_GRAPH TO OMIT THE STOCK averEvalMaterial. The prelude then declares
// everything a material needs -- the AverVertex/AverSurface contract, the map sampling, the BRDF,
// and the AverAuthored/averBuildSurface pair that derives a surface from what a material authored
// -- but no averEvalMaterial at all, and the caller MUST append one of its own with the identical
// signature before any source that calls it, or the compile fails with an undeclared identifier
// rather than silently shading wrong. That appended function is what a .ocgraph material graph is
// compiled into; see materialGraphHlsl() in MaterialGraphHlsl.hpp.
const char* materialShaderPrelude();

// The semicolon-separated -D list that turns the prelude's material textures on and pins them to
// the registers the root signature declared. `tableBaseRegister` is rhi::PipelineLayout::srvCount.
std::string materialShaderDefines(u32 tableBaseRegister, u32 samplerRegister);

} // namespace aver::pbr
