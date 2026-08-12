// Prints the EXACT HLSL the cluster path's pixel shader is compiled from, plus the -D list it is
// compiled with, so both can be handed to dxc without running the engine.
//
// WHY THIS EXISTS. A shader in this tree is a C++ string literal composed at runtime from two
// preludes and a feature's own source; the only thing that ever type-checks it is createShader,
// inside a live device, at startup. So "the C++ compiles" says nothing at all about whether the
// HLSL does -- a wrong register, a missing symbol or a renamed material function is a runtime
// warning on somebody else's machine. This makes the composed text inspectable and compilable
// offline, which is the difference between believing a shader is right and knowing it.
//
//     DumpClusterPs.exe            > cluster_ps.hlsl     (the source)
//     DumpClusterPs.exe --defines                        (the -D list, one per line)
//
// STAGE 3 (GI/shadow shading parity): when AVER_MODULE_VOXI is compiled in -- which it is by
// default, so this is the composition an ordinary build actually ships -- both modes reflect the
// MERGED table 0 ensureLodMeshPipeline() (SandboxApp.cpp) builds: Voxi's GI volume and shadow map
// folded into the cluster pipeline's own table 0, giShaderPrelude() appended after the material
// prelude, and AVER_CLUSTER_VOXI=1 in the -D list. The three register numbers below
// (kClusterGiSrvBase/kClusterGiSamplerBase/kClusterGiFrameRegister) are SandboxApp.cpp's own
// anonymous-namespace constants, typed a second time because this tool cannot link Sandbox.exe's
// translation unit -- see ensureLodMeshPipeline's register-map comment for what they mean and what
// checks them; nothing at compile time ties this copy to that one, which is exactly why running
// this tool after touching either side is the check.
#include "aver/rhi/RHIResources.hpp"
#include "aver/pbr/PbrShaders.hpp"
#include "ClusterMaterialShader.hpp"
#if AVER_MODULE_VOXI
#include "aver/voxi/VoxiGiShaders.hpp"
#endif

#include <cstring>
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    // The same numbers SandboxApp passes: the material textures are based immediately above the
    // cluster pipeline's own 4 geometry SRVs plus (Stage 3) Voxi's merged 9, and its sampler is s0.
    constexpr aver::u32 kClusterGeometrySrvCount = 4;
    constexpr aver::u32 kSamplerSlot = 0;
#if AVER_MODULE_VOXI
    constexpr aver::u32 kClusterGiSrvBase       = 4;   // t4 the GI volume, t5 the shadow map
    constexpr aver::u32 kClusterGiSamplerBase   = 1;   // s1 volume, s2 shadow
    constexpr aver::u32 kClusterGiFrameRegister = 3;   // b3 VoxiFrame
    constexpr aver::u32 kClusterSrvCount = kClusterGeometrySrvCount + aver::voxi::kGiSrvCount;
#else
    constexpr aver::u32 kClusterSrvCount = kClusterGeometrySrvCount;
#endif

    if (argc > 1 && std::strcmp(argv[1], "--defines") == 0) {
        std::string defs = aver::pbr::materialShaderDefines(kClusterSrvCount, kSamplerSlot);
#if AVER_MODULE_VOXI
        defs += ";AVER_CLUSTER_VOXI=1;" +
                aver::voxi::giShaderDefines(kClusterGiSrvBase, kClusterGiSamplerBase, kClusterGiFrameRegister);
#endif
        // Semicolon-separated in the engine; one per line here so a shell can loop over them.
        std::string cur;
        for (const char c : defs) {
            if (c == ';') { if (!cur.empty()) std::printf("%s\n", cur.c_str()); cur.clear(); }
            else cur += c;
        }
        if (!cur.empty()) std::printf("%s\n", cur.c_str());
        return 0;
    }

    std::string src = std::string(aver::rhi::sharedShaderPrelude()) +
                      aver::pbr::materialShaderPrelude();
#if AVER_MODULE_VOXI
    src += aver::voxi::giShaderPrelude();
#endif
    src += std::string(aver::sandbox::kClusterMaterialPS);
    std::fwrite(src.data(), 1, src.size(), stdout);
    return 0;
}
