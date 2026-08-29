// The HLSL half of Aver.Render.PBR.Materials: the BRDF and the Aver* contract a renderer fills in.
// Concatenated AFTER rhi::sharedShaderPrelude(), which owns the shared layouts and helpers.
#include "aver/pbr/PbrShaders.hpp"

#include "aver/pbr/Material.hpp"
#include "aver/rhi/ShaderFiles.hpp"

namespace aver::pbr {

// The material HLSL: the BRDF and the Aver* contract a renderer fills in. Concatenated AFTER
// rhi::sharedShaderPrelude(), which owns the shared layouts and helpers.
const char* materialShaderPrelude() {
    return rhi::shaderFile("material_prelude.hlsl").c_str();
}

// The -D list pinning the material textures to the registers the root signature declared. One
// define per slot: the HLSL preprocessor pastes tokens but cannot evaluate `t##(base+1)`.
std::string materialShaderDefines(u32 tableBaseRegister, u32 samplerRegister) {
    std::string s = "AVER_MATERIAL_SRV=" + std::to_string(tableBaseRegister);
    for (u32 i = 1; i < kTextureSlotCount; ++i)
        s += ";AVER_MATERIAL_SRV_" + std::to_string(i) + "=" + std::to_string(tableBaseRegister + i);
    s += ";AVER_MATERIAL_SAMPLER=" + std::to_string(samplerRegister);
    return s;
}

} // namespace aver::pbr
