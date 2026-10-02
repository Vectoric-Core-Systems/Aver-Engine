// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
//
// The GI/shadow slice of Voxi's HLSL a foreign pipeline may borrow. See the public header's own
// comment for what this is and, more importantly, what it deliberately is not: PSMainVoxi and every
// compute pass stay in VoxiShaders.hpp, private to this module, never textually reachable from here.
#include "aver/voxi/VoxiGiShaders.hpp"
#include "aver/rhi/ShaderFiles.hpp"   // the GI prelude is a deployed file

namespace aver::voxi {

const char* giShaderPrelude() {
    // Keyed on shaderFileRevision(), not a plain static: the loader owns the cache and
    // reloadShaderFiles() clears it, so a once-initialised static would outlive the drop and make
    // hot reload a lie for every shader this prelude reaches. Same shape sharedShaderPrelude() uses.
    static std::string s;
    static u64 built = ~0ull;
    if (built != rhi::shaderFileRevision()) {
        s = rhi::shaderFile("voxi_gi.hlsli");
        built = rhi::shaderFileRevision();
    }
    return s.c_str();
}

std::string giShaderDefines(u32 srvBase, u32 samplerBase, u32 frameConstantRegister) {
    return "AVER_GI_SRV=" + std::to_string(srvBase) +
           ";AVER_GI_SRV_1=" + std::to_string(srvBase + 1) +
           ";AVER_GI_SAMPLER=" + std::to_string(samplerBase) +
           ";AVER_GI_SAMPLER_1=" + std::to_string(samplerBase + 1) +
           ";AVER_GI_FRAME_REG=" + std::to_string(frameConstantRegister);
}

} // namespace aver::voxi
