#pragma once

// The material system's HLSL, owned by Aver.Render.PBR.Materials.
//
// A renderer composes rhi::sharedShaderPrelude() + pbr::materialShaderPrelude() + its own source
// and compiles the three as one translation unit. The order is not negotiable: this text uses the
// constant-buffer layouts, the vertex structures and the colour-space helpers the shared prelude
// declares.
#include "aver/core/Types.hpp"

#include <string>

namespace aver::pbr {

// Immutable, with static storage duration, so a caller may hold the pointer for the life of the
// process. The string a renderer builds around it must have the same lifetime: ShaderDesc::prelude
// is a BORROWED const char* read at pipeline creation, and pipelines are rebuilt whenever the
// render targets change, so a temporary std::string dangles - and usually still works, which is
// the worst possible failure mode.
const char* materialShaderPrelude();

// The -D list that turns the prelude's material textures on and pins them to the registers the
// root signature actually declared. `tableBaseRegister` is the consuming layout's table-0 SRV
// count, i.e. rhi::PipelineLayout::srvCount — the same field the backend bases table 1 at.
// `samplerRegister` is the index in rhi::PipelineLayout::samplers the consumer put the material's
// wrapping sampler at; a static sampler is a property of the layout, so the renderer owns it and
// this only has to be told where it went.
// Semicolon-separated, so it appends straight onto rhi::ShaderDesc::defines.
//
// A pipeline that does NOT declare a second table must not pass this: the prelude then declares no
// material textures at all, which is what lets one shader string serve both. That is the mirror of
// rhi::meshGeometryDefines() and exists for the same reason — a shader naming a register its root
// signature never declared fails at pipeline creation, a long way from the layout that caused it.
std::string materialShaderDefines(u32 tableBaseRegister, u32 samplerRegister);

} // namespace aver::pbr
