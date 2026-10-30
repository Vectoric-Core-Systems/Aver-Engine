#pragma once
// Feeds the framework's custom sky, fog and particles programs (framework_customenv_abi.h) to the renderers, for either host: the sky and the
// fog programs to Voxi, the particles programs to the GPU particle simulation. A script or a converted level REGISTERS programs; this READS what changed. With nothing
// registered nothing is pushed beyond clearing what an earlier frame pushed, so a project without custom programs runs exactly today's passes.
// Called from EnvFxFeed::update (G4 R8's per-frame feed), which both hosts already call. Include under AVER_MODULE_VOXI.
#if AVER_MODULE_VOXI

#include "aver/core/Log.hpp"
#include "aver/formats/OcGraph.hpp"
#include "aver/formats/OcGraphHlsl.hpp"
#include "aver/framework/framework_customenv_abi.h"
#include "aver/rhi/CustomEnvPrograms.hpp"
#include "aver/voxi/VoxiRenderer.hpp"
#if AVER_MODULE_PARTICLES
#include "aver/particles/ParticleGpu.hpp"
#endif

#include <algorithm>
#include <string>
#include <vector>

namespace aver::game {

class CustomEnvFeed {
public:
    // Reads program `index` of `kind` from the registry. False when the index is stale or the registered graph does not unpack.
    static bool load(int32_t kind, int32_t index, rhi::CustomEnvProgram& out) {
        const int32_t len = aver_fw_cenv_graph(kind, index, nullptr, 0);
        if (len <= 0) return false;
        std::string text(static_cast<size_t>(len) + 1, '\0');
        aver_fw_cenv_graph(kind, index, text.data(), len + 1);
        text.resize(static_cast<size_t>(len));
        fmt::OcGraphData g;
        fmt::EmbeddedHlslEnvPart part;
        std::string err;
        if (!fmt::parseOcgraph(text, g, &err) || !fmt::unpackEmbeddedHlslEnv(g, part, &err)) {
            AVER_WARN("[CustomEnv] a registered shader does not unpack: {}", err);
            return false;
        }
        const int32_t nameLen = aver_fw_cenv_name(kind, index, nullptr, 0);
        std::string name(static_cast<size_t>(nameLen > 0 ? nameLen : 0) + 1, '\0');
        if (nameLen > 0) aver_fw_cenv_name(kind, index, name.data(), nameLen + 1);
        name.resize(static_cast<size_t>(nameLen > 0 ? nameLen : 0));
        out = rhi::CustomEnvProgram{};
        out.name = name.empty() ? part.name : name;
        out.ns = part.ns;
        out.source = part.source;
        out.regs = part.regs;
        out.globalRegs = part.globalRegs;
        out.hasStart = part.hasStart;
        out.hasProcess = part.hasProcess;
        out.values = part.defaults;
        const int32_t n = aver_fw_cenv_registers(kind, index, nullptr, 0);
        if (n > 0) {
            std::vector<float> v(static_cast<size_t>(n));
            aver_fw_cenv_registers(kind, index, v.data(), n);
            out.values = v;
        }
        return true;
    }

    // Once per frame, before the frame renders.
    void update(voxi::VoxiRenderer& voxi) {
        const int64_t rev = aver_fw_cenv_revision(), regRev = aver_fw_cenv_registers_revision();
        if (rev == rev_ && regRev == regRev_) return;
        const bool rebuilt = rev != rev_;
        rev_ = rev;
        regRev_ = regRev;
        const bool any = aver_fw_cenv_count(AVER_FW_CENV_SKY) + aver_fw_cenv_count(AVER_FW_CENV_FOG) + aver_fw_cenv_count(AVER_FW_CENV_PARTICLES) > 0;
        if (!any && !pushed_) return;

        // sky: the active one, or the engine's own. A changed uniform value recomposes the program (the registers are baked into it).
        const int32_t si = aver_fw_cenv_sky_index();
        rhi::CustomEnvProgram sky;
        if (si >= 0 && load(AVER_FW_CENV_SKY, si, sky)) voxi.setCustomSky(&sky);
        else voxi.setCustomSky(nullptr);

        // fog: the registered fog programs in registration order (the table a volume's shader index names); registers every time.
        std::vector<rhi::CustomEnvProgram> fogs;
        const int32_t nf = std::min<int32_t>(aver_fw_cenv_count(AVER_FW_CENV_FOG), static_cast<int32_t>(rhi::kCustomFogShaderMax));
        for (int32_t i = 0; i < nf; ++i) {
            rhi::CustomEnvProgram p;
            if (load(AVER_FW_CENV_FOG, i, p)) fogs.push_back(std::move(p));
            else fogs.emplace_back();
        }
        voxi.setFogShaders(std::move(fogs));
        std::vector<std::vector<float>> regs;
        for (int32_t i = 0; i < nf; ++i) {
            rhi::CustomEnvProgram p;
            if (load(AVER_FW_CENV_FOG, i, p)) regs.push_back(p.values);
            else regs.emplace_back();
        }
        voxi.setFogShaderRegisters(regs);

#if AVER_MODULE_PARTICLES
        if (rebuilt || !pushedParticles_) {
            std::vector<rhi::CustomEnvProgram> progs;
            const int32_t np = aver_fw_cenv_count(AVER_FW_CENV_PARTICLES);
            for (int32_t i = 0; i < np; ++i) {
                rhi::CustomEnvProgram p;
                if (load(AVER_FW_CENV_PARTICLES, i, p)) progs.push_back(std::move(p));
            }
            particles::setCustomParticlePrograms(std::move(progs));
            pushedParticles_ = true;
        }
#endif
        if (rebuilt) AVER_INFO("[CustomEnv] {} sky, {} fog and {} particles program(s) registered{}", aver_fw_cenv_count(AVER_FW_CENV_SKY), aver_fw_cenv_count(AVER_FW_CENV_FOG),
                               aver_fw_cenv_count(AVER_FW_CENV_PARTICLES), si >= 0 ? "; a custom sky is active" : "");
        pushed_ = any;
    }

private:
    int64_t rev_ = -1, regRev_ = -1;
    bool pushed_ = false;
    bool pushedParticles_ = false;
};

} // namespace aver::game

#endif // AVER_MODULE_VOXI
