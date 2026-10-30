#pragma once
// Test support: embedded HLSL shaders (OcGraphHlsl.hpp) registered in the process-wide material graph registry, and the engine's own composition of the
// translation unit a material pixel / vertex shader is compiled in (VoxiRenderer.cpp voxiShaderPrelude). No translator is involved: the sources below are
// written by hand in the shape a translator emits, so the engine's tests need nothing outside the engine.
#include "aver/formats/OcGraphHlsl.hpp"
#include "aver/pbr/MaterialGraphRegistry.hpp"
#include "aver/pbr/PbrShaders.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <string>

namespace embtest {

// Registers the part in the process-wide registry (as the loader would from its .ocgraph) and returns its id, 0 on failure.
inline aver::u32 registerPart(const aver::fmt::EmbeddedHlslPart& part, const std::string& key) {
    aver::fmt::OcGraphData g;
    g.name = part.name;
    aver::fmt::packEmbeddedHlsl(part, g);
    return aver::pbr::materialGraphs().add(key, part.name, g);
}

// A spatial shader with a vertex() that displaces by a texture, two varyings (one flat), a fragment() that samples with repeat, discards, writes
// ALPHA_SCISSOR_THRESHOLD and the rim term. Uniforms: register 0 the tint, register 1 .x the wave height. Textures: slot 0 albedo (repeat, mipmaps), slot 1 height (nearest).
inline aver::fmt::EmbeddedHlslPart displaced() {
    aver::fmt::EmbeddedHlslPart p;
    p.name = "EmbA";
    p.ns = "ea00000001_";
    p.regsU = 2;
    p.hasFragment = true;
    p.hasVertex = true;
    const std::string samplers =
        "EmbSampler2D EMBN(gdsm_albedo_tex)() { EmbSampler2D r; r.slot = 0u; r.m = 6u; return r; }\n"
        "EmbSampler2D EMBN(gdsm_height_tex)() { EmbSampler2D r; r.slot = 1u; r.m = 1u; return r; }\n";
    p.fragmentSource = samplers +
        "void EMBN(main_fragment)()\n"
        "{\n"
        "    float4 l_a = emb_texture(EMBN(gdsm_albedo_tex)(), (AVER_CUSTOM_UV + ((AVER_CUSTOM_VARYINGS.ea00000001_gdv_v_pos).xz * 0.01)));\n"
        "    AVER_CUSTOM_ALBEDO = ((l_a.xyz * AVER_CUSTOM_UBUF[0].xyz) + ((float3)(((float)(AVER_CUSTOM_VARYINGS.ea00000001_gdv_v_id)) * 0.0)));\n"
        "    AVER_CUSTOM_ALPHA = l_a.w;\n"
        "    AVER_CUSTOM_ALPHA_SCISSOR_THRESHOLD = 0.5;\n"
        "    AVER_CUSTOM_ROUGHNESS = 0.4;\n"
        "    AVER_CUSTOM_RIM = 0.3;\n"
        "    if (l_a.w < 0.1)\n"
        "    {\n"
        "        discard;\n"
        "    }\n"
        "}\n";
    p.vertexSource = samplers +
        "void EMBN(main_vertex)()\n"
        "{\n"
        "    (AVER_CUSTOM_VERTEX).y += (((emb_textureLod(EMBN(gdsm_height_tex)(), AVER_CUSTOM_UV, 0.0)).x * AVER_CUSTOM_UBUF[1].x) + (sin(AVER_CUSTOM_TIME) * AVER_CUSTOM_UBUF[1].x));\n"
        "    AVER_CUSTOM_VARYINGS.ea00000001_gdv_v_pos = AVER_CUSTOM_VERTEX;\n"
        "    AVER_CUSTOM_VARYINGS.ea00000001_gdv_v_id = AVER_CUSTOM_INSTANCE_ID;\n"
        "}\n";
    p.varyings.push_back(aver::fmt::EmbeddedHlslVarying{"float3 ea00000001_gdv_v_pos", false, 1});
    p.varyings.push_back(aver::fmt::EmbeddedHlslVarying{"int ea00000001_gdv_v_id", true, 1});
    return p;
}

// A fragment() that writes NORMAL (the engine converts it to a tangent-space perturbation). Uniform: register 0 .x.
inline aver::fmt::EmbeddedHlslPart normalWriter() {
    aver::fmt::EmbeddedHlslPart p;
    p.name = "EmbB";
    p.ns = "eb00000001_";
    p.regsU = 1;
    p.fx = aver::fmt::hlslfx::kUsesNormalWrite;
    p.hasFragment = true;
    p.fragmentSource =
        "void EMBN(main_fragment)()\n"
        "{\n"
        "    AVER_CUSTOM_NORMAL = normalize((AVER_CUSTOM_NORMAL + float3(0.0, (0.2 * AVER_CUSTOM_UBUF[0].x), 0.0)));\n"
        "    AVER_CUSTOM_ALBEDO = float3(0.5, 0.6, 0.7);\n"
        "}\n";
    return p;
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

}  // namespace embtest
