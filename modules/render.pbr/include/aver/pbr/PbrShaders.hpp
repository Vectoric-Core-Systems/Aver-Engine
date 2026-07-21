#pragma once

// The material system's HLSL, owned by Aver.Render.PBR.Materials.
//
// A renderer composes rhi::sharedShaderPrelude() + pbr::materialShaderPrelude() + its own source
// and compiles the three as one translation unit. The order is not negotiable: this text uses the
// constant-buffer layouts, the vertex structures and the colour-space helpers the shared prelude
// declares.
namespace aver::pbr {

// Immutable, with static storage duration, so a caller may hold the pointer for the life of the
// process. The string a renderer builds around it must have the same lifetime: ShaderDesc::prelude
// is a BORROWED const char* read at pipeline creation, and pipelines are rebuilt whenever the
// render targets change, so a temporary std::string dangles - and usually still works, which is
// the worst possible failure mode.
const char* materialShaderPrelude();

} // namespace aver::pbr
