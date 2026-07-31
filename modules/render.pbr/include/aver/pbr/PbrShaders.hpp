#pragma once

// The material system's HLSL, owned by Aver.Render.PBR.Materials. A renderer compiles
// rhi::sharedShaderPrelude() + pbr::materialShaderPrelude() + its own source, in that order.
#include "aver/core/Types.hpp"

#include <string>

namespace aver::pbr {

// The material HLSL, with static storage duration: the pointer is good for the whole process.
const char* materialShaderPrelude();

// The semicolon-separated -D list that turns the prelude's material textures on and pins them to
// the registers the root signature declared. `tableBaseRegister` is rhi::PipelineLayout::srvCount.
std::string materialShaderDefines(u32 tableBaseRegister, u32 samplerRegister);

} // namespace aver::pbr
