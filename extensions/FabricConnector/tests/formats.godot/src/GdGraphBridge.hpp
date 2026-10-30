#pragma once
// Test support (G6 SH2): a prepared Godot shader (GdEngineShader) as the EmbeddedHlsl node of a material graph, and the engine's own composition of the
// translation unit a material pixel / vertex shader is compiled in. The converter (tools/fabric/FabricShader.cpp) makes the same mapping.
#include "aver/formats/OcGraphHlsl.hpp"
#include "aver/formats/godot/GodotShaderEngine.hpp"
#include "aver/pbr/MaterialGraphRegistry.hpp"
#include "aver/pbr/PbrShaders.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <string>

namespace gdtest {

inline void partOf(const aver::fmt::godot::GdEngineShader& s, aver::fmt::EmbeddedHlslPart& p) {
    p = aver::fmt::EmbeddedHlslPart{};
    p.name = s.name;
    p.ns = s.ns;
    p.regsU = static_cast<aver::u32>(s.uniforms.registerCount);
    p.regsI = static_cast<aver::u32>(s.instanceUniforms.registerCount);
    p.regsG = static_cast<aver::u32>(s.globalUniforms.registerCount);
    p.fx = s.fxBits;
    p.hasFragment = s.hasFragment;
    p.hasVertex = s.hasVertex;
    p.fragmentSource = s.fragmentSource;
    p.vertexSource = s.vertexSource;
    for (const auto& v : s.varyings) p.varyings.push_back(aver::fmt::EmbeddedHlslVarying{v.decl, v.flat, static_cast<aver::u32>(v.rows)});
}

// Registers the shader in the process-wide registry (as the loader would from its .ocgraph) and returns its id, 0 on failure.
inline aver::u32 registerShader(const aver::fmt::godot::GdEngineShader& s, const std::string& key) {
    aver::fmt::EmbeddedHlslPart p;
    partOf(s, p);
    aver::fmt::OcGraphData g;
    g.name = s.name;
    aver::fmt::packEmbeddedHlsl(p, g);
    return aver::pbr::materialGraphs().add(key, s.name, g);
}

// The engine's composition: voxiShaderPrelude() in VoxiRenderer.cpp, then the renderer's own source.
inline std::string compose(const std::string& rendererSource) {
    auto& reg = aver::pbr::materialGraphs();
    std::string s = reg.hasEmbeddedVaryings() ? reg.vsOutMembersDefine() : std::string();
    s += aver::rhi::sharedShaderPrelude();
    if (!reg.hlsl().empty()) s += "\n#define AVER_MATERIAL_GRAPH 1\n";
    if (reg.hasEmbeddedShaders()) s += "#define AVER_CUSTOM_FX_STOCK 1\n";
    s += aver::pbr::materialShaderPrelude();
    s += reg.hlsl();
    s += rendererSource;
    return s;
}

}  // namespace gdtest
