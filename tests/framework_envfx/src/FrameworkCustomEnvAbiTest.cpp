// FrameworkCustomEnvAbiTest -- the custom sky / fog / particles program registry (framework_customenv_abi.h) through the Aver.Framework DLL, and the
// fog volume's shader index in the packed block (framework_envfx_abi.h). Headless: no device, no engine run.
#include "aver/core/FogVolume.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcGraph.hpp"
#include "aver/formats/OcGraphHlsl.hpp"
#include "aver/framework/framework_customenv_abi.h"
#include "aver/framework/framework_envfx_abi.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;
static void check(bool cond, const char* what) {
    if (cond) return;
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static std::string graphText(const char* kind, const char* name, std::vector<float> regs, u32 nregs) {
    fmt::EmbeddedHlslEnvPart p;
    p.kind = kind;
    p.name = name;
    p.ns = "ge00000001_";
    p.regs = nregs;
    p.hasStart = std::string(kind) == "particles";
    p.hasProcess = p.hasStart;
    p.defaults = std::move(regs);
    p.source = "void ge00000001_main_x() {}\n";
    fmt::OcGraphData g;
    fmt::packEmbeddedHlslEnv(p, g);
    return fmt::writeOcgraph(g);
}

static std::string fetch(int32_t (*fn)(int32_t, int32_t, char*, int32_t), int32_t kind, int32_t index) {
    const int32_t n = fn(kind, index, nullptr, 0);
    if (n < 0) return "<none>";
    std::string s(static_cast<size_t>(n) + 1, '\0');
    fn(kind, index, s.data(), n + 1);
    s.resize(static_cast<size_t>(n));
    return s;
}

int main() {
    aver_fw_cenv_reset();
    check(aver_fw_cenv_abi_version() == AVER_FW_CENV_ABI_VERSION, "the version is the header's");
    check(AVER_FW_CENV_SKY == 0 && AVER_FW_CENV_FOG == 1 && AVER_FW_CENV_PARTICLES == 2, "the kinds are 0, 1, 2");

    const std::string sky = graphText("sky", "dusk", {0.1f, 0.2f, 0.3f, 0.4f}, 1);
    const std::string fog = graphText("fog", "mist", {2.5f, 0, 0, 0, 1, 1, 1, 0}, 2);
    const std::string par = graphText("particles", "sparks", {1, 2, 3, 4}, 1);
    const int64_t rev0 = aver_fw_cenv_revision();

    check(aver_fw_cenv_register(AVER_FW_CENV_SKY, "dusk", sky.c_str()) == 1, "a sky graph registers");
    check(aver_fw_cenv_revision() > rev0, "registering moves the revision");
    check(aver_fw_cenv_register(AVER_FW_CENV_FOG, "mist", fog.c_str()) == 1 && aver_fw_cenv_register(AVER_FW_CENV_PARTICLES, "sparks", par.c_str()) == 1, "a fog and a particles graph register");
    check(aver_fw_cenv_register(AVER_FW_CENV_FOG, "wrong", sky.c_str()) == 0, "a sky graph is refused as a fog shader");
    check(aver_fw_cenv_register(AVER_FW_CENV_SKY, "bad", "not a graph") == 0, "text that is not a graph is refused");
    check(aver_fw_cenv_register(AVER_FW_CENV_SKY, "", sky.c_str()) == 0 && aver_fw_cenv_register(AVER_FW_CENV_SKY, nullptr, sky.c_str()) == 0 && aver_fw_cenv_register(7, "x", sky.c_str()) == 0,
          "an empty name, a null name and an unknown kind are refused");
    check(aver_fw_cenv_count(AVER_FW_CENV_SKY) == 1 && aver_fw_cenv_count(AVER_FW_CENV_FOG) == 1 && aver_fw_cenv_count(AVER_FW_CENV_PARTICLES) == 1, "one program of each kind");

    // A graph written before the rename (DOMAIN godotenv, NODE gd GodotEnvShader) is still read; the writer's own output uses the new spellings.
    const std::string oldSky = "OCGRAPH 1\nDOMAIN godotenv\nNODE gd GodotEnvShader kind=sky name=old ns=ge1_ regs=1 gregs=0 start=0 process=0 reg=3F800000,00000000,00000000,3F800000 src0=void\n";
    check(sky.find("DOMAIN customenv") != std::string::npos && sky.find("EmbeddedHlslEnv") != std::string::npos && sky.find("GodotEnvShader") == std::string::npos, "new graphs are written with the new spellings");
    check(aver_fw_cenv_register(AVER_FW_CENV_SKY, "old", oldSky.c_str()) == 1 && aver_fw_cenv_count(AVER_FW_CENV_SKY) == 2, "a graph in the old spelling still registers");
    check(aver_fw_cenv_unregister(AVER_FW_CENV_SKY, "old") == 1 && aver_fw_cenv_count(AVER_FW_CENV_SKY) == 1, "and goes again");

    check(fetch(aver_fw_cenv_name, AVER_FW_CENV_FOG, 0) == "mist" && fetch(aver_fw_cenv_name, AVER_FW_CENV_FOG, 1) == "<none>", "the name comes back; a stale index is -1");
    check(fetch(aver_fw_cenv_graph, AVER_FW_CENV_FOG, 0) == fog, "the registered graph text comes back byte for byte");
    float regs[8] = {};
    check(aver_fw_cenv_registers(AVER_FW_CENV_FOG, 0, nullptr, 0) == 8, "measuring the registers gives the float count");
    check(aver_fw_cenv_registers(AVER_FW_CENV_FOG, 0, regs, 8) == 8 && regs[0] == 2.5f && regs[4] == 1.0f, "the graph's defaults are the registers");

    const int64_t reg0 = aver_fw_cenv_registers_revision(), rev1 = aver_fw_cenv_revision();
    const float nv[8] = {4.0f, 0, 0, 0, 1, 1, 1, 0};
    check(aver_fw_cenv_set_registers(AVER_FW_CENV_FOG, "mist", nv, 8) == 1, "registers are set");
    check(aver_fw_cenv_registers_revision() > reg0 && aver_fw_cenv_revision() == rev1, "setting registers moves the registers revision only (nothing rebuilds)");
    aver_fw_cenv_registers(AVER_FW_CENV_FOG, 0, regs, 8);
    check(regs[0] == 4.0f, "the new value reads back");
    const int64_t reg1 = aver_fw_cenv_registers_revision();
    aver_fw_cenv_set_registers(AVER_FW_CENV_FOG, "mist", nv, 8);
    check(aver_fw_cenv_registers_revision() == reg1, "setting the same values changes nothing");
    check(aver_fw_cenv_set_registers(AVER_FW_CENV_FOG, "nope", nv, 8) == 0 && aver_fw_cenv_set_registers(AVER_FW_CENV_FOG, "mist", nullptr, 8) == 0, "an unknown shader and a null table are refused");
    const int denorm = 3;
    float oddRegs[8] = {};
    std::memcpy(&oddRegs[0], &denorm, 4);
    aver_fw_cenv_set_registers(AVER_FW_CENV_FOG, "mist", oddRegs, 8);
    float back[8] = {};
    aver_fw_cenv_registers(AVER_FW_CENV_FOG, 0, back, 8);
    int bits = 0;
    std::memcpy(&bits, &back[0], 4);
    check(bits == 3, "an int uniform's raw word (a denormal float) survives");

    check(aver_fw_cenv_sky_index() == -1, "the engine's own sky is active at first");
    check(aver_fw_cenv_use_sky("dusk") == 1 && aver_fw_cenv_sky_index() == 0, "a registered sky shader is made the sky");
    check(aver_fw_cenv_use_sky("mist") == 0, "a fog shader cannot be the sky");
    check(aver_fw_cenv_use_sky(nullptr) == 1 && aver_fw_cenv_sky_index() == -1, "null goes back to the engine's sky");

    // a fog volume names its shader; the packed block carries the shader's place in the table
    aver_fw_envfx_reset();
    aver_fw_cenv_register(AVER_FW_CENV_FOG, "mist", fog.c_str());
    aver_fw_cenv_register(AVER_FW_CENV_FOG, "haze", fog.c_str());
    const int32_t id = aver_fw_fogvol_create("v");
    const float world[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 10, 20, 30};
    const float half[3] = {100, 100, 100};
    check(aver_fw_fogvol_set(id, AVER_FW_FOGVOL_BOX, world, half, 1.0f, nullptr, nullptr, 0.0f, 0.1f) == 1, "a volume is set");
    std::vector<float> block(sizeof(FogVolumeBlock) / sizeof(float));
    check(aver_fw_fogvol_pack(0, 0, 0, block.data(), static_cast<int32_t>(block.size())) == 1, "one volume packs");
    const FogVolumeBlock* b = reinterpret_cast<const FogVolumeBlock*>(block.data());
    check(fogVolumeShaderOf(b->vec[0][3][3]) == 0u && fogVolumeShapeOf(b->vec[0][3][3]) == 3, "without a shader the shape word is the shape");
    check(aver_fw_fogvol_set_shader(id, "haze") == 1, "a shader name is set");
    aver_fw_fogvol_pack(0, 0, 0, block.data(), static_cast<int32_t>(block.size()));
    check(fogVolumeShaderOf(b->vec[0][3][3]) == 2u && fogVolumeShapeOf(b->vec[0][3][3]) == 3, "haze is the second fog shader: shape word = 3 + 8 * 2");
    check(b->vec[0][4][0] == 10.0f && b->vec[0][4][2] == 30.0f && b->vec[0][5][0] == 100.0f, "the centre and the half extents ride the albedo and emission slots");
    check(aver_fw_fogvol_set_shader(id, "not-registered") == 1, "an unregistered name is kept");
    aver_fw_fogvol_pack(0, 0, 0, block.data(), static_cast<int32_t>(block.size()));
    check(fogVolumeShaderOf(b->vec[0][3][3]) == 0u, "and means no shader until it is registered");
    check(aver_fw_fogvol_set_shader(9999, "haze") == 0, "a stale volume id is refused");
    check(aver_fw_cenv_unregister(AVER_FW_CENV_FOG, "haze") == 1 && aver_fw_cenv_unregister(AVER_FW_CENV_FOG, "haze") == 0, "unregistering works once");

    aver_fw_cenv_register(AVER_FW_CENV_SKY, "dusk", sky.c_str());
    aver_fw_cenv_use_sky("dusk");
    aver_fw_envfx_reset();
    check(aver_fw_cenv_count(AVER_FW_CENV_SKY) == 0 && aver_fw_cenv_count(AVER_FW_CENV_FOG) == 0 && aver_fw_cenv_sky_index() == -1, "the envfx session reset forgets every shader and the sky");

    if (g_failures) {
        AVER_ERROR("FrameworkCustomEnvAbiTest: {} failure(s)", g_failures);
        return 1;
    }
    AVER_INFO("FrameworkCustomEnvAbiTest: ok");
    return 0;
}
