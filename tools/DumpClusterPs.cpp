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
#include "aver/rhi/RHIResources.hpp"
#include "aver/pbr/PbrShaders.hpp"
#include "ClusterMaterialShader.hpp"

#include <cstring>
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    // The same two numbers SandboxApp passes: the material textures are based immediately above the
    // cluster pipeline's own 4 SRVs, and its sampler is s0 (this layout declares no others).
    constexpr aver::u32 kClusterSrvCount = 4;
    constexpr aver::u32 kSamplerSlot = 0;

    if (argc > 1 && std::strcmp(argv[1], "--defines") == 0) {
        const std::string defs = aver::pbr::materialShaderDefines(kClusterSrvCount, kSamplerSlot);
        // Semicolon-separated in the engine; one per line here so a shell can loop over them.
        std::string cur;
        for (const char c : defs) {
            if (c == ';') { if (!cur.empty()) std::printf("%s\n", cur.c_str()); cur.clear(); }
            else cur += c;
        }
        if (!cur.empty()) std::printf("%s\n", cur.c_str());
        return 0;
    }

    const std::string src = std::string(aver::rhi::sharedShaderPrelude()) +
                            aver::pbr::materialShaderPrelude() +
                            std::string(aver::sandbox::kClusterMaterialPS);
    std::fwrite(src.data(), 1, src.size(), stdout);
    return 0;
}
