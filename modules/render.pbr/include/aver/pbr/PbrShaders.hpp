#pragma once

// The material system's HLSL, owned by Aver.Render.PBR.Materials. A renderer compiles
// rhi::sharedShaderPrelude() + pbr::materialShaderPrelude() + its own source as one translation
// unit, in that order.
#include "aver/core/Types.hpp"

#include <string>

namespace aver::pbr {

// The material HLSL. Immutable with static storage duration, so a caller may hold the pointer for
// the life of the process — ShaderDesc::prelude is a borrowed const char* read at pipeline creation.
const char* materialShaderPrelude();

// The semicolon-separated -D list that turns the prelude's material textures on and pins them to
// the registers the root signature declared. `tableBaseRegister` is rhi::PipelineLayout::srvCount;
// `samplerRegister` is the index the consumer put the material's wrapping sampler at. A pipeline
// that declares no second table must NOT pass this.
std::string materialShaderDefines(u32 tableBaseRegister, u32 samplerRegister);

} // namespace aver::pbr
