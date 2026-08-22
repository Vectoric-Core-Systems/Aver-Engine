// patchCbuffersForLayout: lowering a shader's cbuffers to what its PipelineLayout says they are.
//
// NO DEVICE, NO VULKAN HEADERS. The function under test is a string transform whose only other
// input is a PipelineLayout, and that is exactly the point of testing it here: the thing it gets
// right, and that no patch operating on source alone could get right, is *arithmetic on byte
// offsets*. Whether the padding before slot b3 is 128 bytes or 0 decides whether a skinning shader
// reads its own parameters or the object block, and that question is decidable at this desk.
//
// It is declared rather than included because modules/rhi.vulkan's include directories are PRIVATE
// and VulkanCommon.hpp would drag in the vendored Vulkan headers for no benefit -- PipelineLayout
// itself is public Aver.RHI.
#include "aver/rhi/RHIResources.hpp"
#include "aver/core/Log.hpp"

#include <string>

namespace aver::rhi::vkb {
bool patchCbuffersForLayout(std::string& src, const PipelineLayout& layout, bool mesh);
}

using namespace aver;
using namespace aver::rhi;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

// ---- the padding, which is the whole reason this needs a layout ---------------------------------
//
// SkinningPass declares constantDwords[3] = 4 and nothing else, so pushConstantLayout puts the
// object block at bytes 0..128 (it is placed unconditionally, declared or not) and SkinParams at
// byte 128. The shader declares ONLY b3. If the generated struct began with gVertexCount, the
// shader would read bytes 0..16 -- the top row of gWorld -- while the engine pushed the real values
// to 128. That failure renders: it does not crash, it skins every vertex by a garbage matrix.
static void testSlotThreePadsToOneTwentyEight() {
    AVER_INFO("-- a lone b3 block is padded to byte 128 --");
    std::string src =
        "StructuredBuffer<float4> gRest : register(t0);\n"
        "cbuffer SkinParams : register(b3) {\n"
        "    uint gVertexCount;\n"
        "    uint gBoneCount;\n"
        "    uint gSkinPad0;\n"
        "    uint gSkinPad1;\n"
        "};\n"
        "[numthreads(64,1,1)] void CSSkin(uint3 t : SV_DispatchThreadID) { if (t.x >= gVertexCount) return; }\n";

    PipelineLayout layout;
    layout.srvCount = 1;
    layout.constantDwords[3] = 4;

    const std::string before = src;
    check(vkb::patchCbuffersForLayout(src, layout, false), "the patch succeeds");
    check(src != before, "the source actually changed");
    check(has(src, "[[vk::push_constant]] ConstantBuffer<AverPcBlock> gAverPc;"), "a push-constant block is emitted");

    // 128 bytes of padding == 8 rows of uint4. `uint _pad[32]` would be four times too long, because
    // an array element in a constant block occupies a full 16-byte row.
    check(has(src, "uint4 _averPcPad3[8];"), "b3 is preceded by 8 uint4 rows (128 bytes)");
    check(!has(src, "_averPcPad3[32]"), "the padding is not counted in dwords");

    // The original block is gone, or DXC would emit a descriptor for it as well.
    check(!has(src, "cbuffer SkinParams"), "the original cbuffer is removed");

    // Every field keeps resolving by its own name, so the shader body is untouched.
    check(has(src, "static uint gVertexCount = gAverPc.gVertexCount;"), "gVertexCount is aliased");
    check(has(src, "static uint gBoneCount = gAverPc.gBoneCount;"), "gBoneCount is aliased");
    check(has(src, "static uint gSkinPad1 = gAverPc.gSkinPad1;"), "gSkinPad1 is aliased");

    // The alias must come AFTER the struct that defines the member, or the struct body would be
    // rewritten by its own aliases.
    check(src.find("struct AverPcBlock") < src.find("static uint gVertexCount"), "aliases follow the struct");

    // Untouched resources stay untouched.
    check(has(src, "StructuredBuffer<float4> gRest : register(t0);"), "the SRV is left alone");
}

// ---- b1 sits at byte 0, so it takes no padding ---------------------------------------------------
static void testObjectBlockNeedsNoPadding() {
    AVER_INFO("-- the object block at b1 starts at byte 0 --");
    std::string src =
        "cbuffer PerObject : register(b1) {\n"
        "    float4x4 gWorld;\n"
        "    float4   gBaseColor;\n"
        "    float4   gMaterial;\n"
        "    uint     gShadingModel;\n"
        "    float    gReflectance;\n"
        "    float    gF90;\n"
        "    float4   gEmissive;\n"
        "};\n"
        "float4 PSMain() : SV_Target { return gBaseColor * gWorld[0][0]; }\n";

    PipelineLayout layout;
    layout.constantDwords[kObjectConstantRegister] = kObjectConstantDwords;

    check(vkb::patchCbuffersForLayout(src, layout, false), "the patch succeeds");
    check(has(src, "[[vk::push_constant]]"), "a push-constant block is emitted");
    check(!has(src, "_averPcPad"), "no padding is emitted for the first slot");
    check(has(src, "static float4x4 gWorld = gAverPc.gWorld;"), "a matrix field is aliased with its type");
    check(has(src, "static uint gShadingModel = gAverPc.gShadingModel;"), "a scalar field is aliased");
    check(!has(src, "cbuffer PerObject"), "the original cbuffer is removed");
}

// ---- the other half of the same rule -------------------------------------------------------------
//
// constantDwords == 0 means a descriptor, and descriptorLayout() declares it as a dynamic UBO at
// binding == register in set kVkSetConstants. Without the annotation DXC leaves a space-less
// register in set 0, where nothing declares it -- which is exactly how ActorPreview's b4 block
// failed.
static void testZeroDwordSlotBecomesADescriptor() {
    AVER_INFO("-- a zero-dword slot is annotated as a descriptor, not folded --");
    std::string src =
        "cbuffer PreviewFrame : register(b4) {\n"
        "    float4x4 gViewProj;\n"
        "};\n"
        "float4 VSMain() : SV_Position { return gViewProj[0]; }\n";

    PipelineLayout layout;   // every constantDwords stays 0

    check(vkb::patchCbuffersForLayout(src, layout, false), "the patch succeeds");
    check(has(src, "[[vk::binding(4, 2)]] cbuffer PreviewFrame : register(b4)"), "b4 is bound at set 2, binding 4");
    check(!has(src, "push_constant"), "it is NOT folded into push constants");
    check(has(src, "float4x4 gViewProj;"), "the block keeps its fields");
}

// ---- idempotence: patchPerFrameSet runs first and must not be doubled ------------------------------
static void testAlreadyAnnotatedIsLeftAlone() {
    AVER_INFO("-- an existing [[vk::binding]] is not doubled --");
    std::string src =
        "[[vk::binding(0, 2)]] cbuffer PerFrame : register(b0) {\n"
        "    float4x4 gViewProj;\n"
        "};\n"
        "float4 VSMain() : SV_Position { return gViewProj[0]; }\n";

    PipelineLayout layout;
    const std::string before = src;
    check(vkb::patchCbuffersForLayout(src, layout, false), "the patch succeeds");
    check(src == before, "the source is byte-identical");

    // Byte-identical is not cosmetic: moduleForLayout uses exactly this to decide it can reuse the
    // layout-agnostic module instead of compiling a second one.
    const size_t first = src.find("[[vk::binding");
    check(src.find("[[vk::binding", first + 1) == std::string::npos, "only one annotation is present");
}

// ---- a shader with nothing to lower comes back untouched -------------------------------------------
static void testUnrelatedShaderIsUntouched() {
    AVER_INFO("-- a shader with no cbuffers is returned verbatim --");
    std::string src =
        "Texture2D gTex : register(t0);\n"
        "SamplerState gSamp : register(s0);\n"
        "float4 PSMain(float2 uv : TEXCOORD0) : SV_Target { return gTex.Sample(gSamp, uv); }\n";

    PipelineLayout layout;
    layout.srvCount = 1;
    layout.constantDwords[3] = 4;   // the layout HAS a push-constant slot; the shader just ignores it

    const std::string before = src;
    check(vkb::patchCbuffersForLayout(src, layout, false), "the patch succeeds");
    check(src == before, "the source is byte-identical");
}

// ---- two folded blocks land back to back, in byte order --------------------------------------------
//
// SPIR-V allows one PushConstant block per entry point, so b1 and b3 must share a struct. b1 is 128
// bytes, so b3 follows immediately and needs NO padding -- the same slot that needed 128 bytes of it
// when b1 was absent. That contrast is the clearest statement of why the layout is required.
static void testTwoBlocksFoldInByteOrder() {
    AVER_INFO("-- b1 and b3 fold into one block, b3 needing no padding this time --");
    std::string src =
        "cbuffer SkinParams : register(b3) {\n"
        "    uint gVertexCount;\n"
        "    uint gBoneCount;\n"
        "    uint gSkinPad0;\n"
        "    uint gSkinPad1;\n"
        "};\n"
        "cbuffer PerObject : register(b1) {\n"
        "    float4x4 gWorld;\n"
        "    float4   gBaseColor;\n"
        "    float4   gMaterial;\n"
        "    uint     gShadingModel;\n"
        "    float    gReflectance;\n"
        "    float    gF90;\n"
        "    float4   gEmissive;\n"
        "};\n"
        "float4 PSMain() : SV_Target { return gBaseColor * gVertexCount; }\n";

    PipelineLayout layout;
    layout.constantDwords[kObjectConstantRegister] = kObjectConstantDwords;
    layout.constantDwords[3] = 4;

    check(vkb::patchCbuffersForLayout(src, layout, false), "the patch succeeds");
    check(!has(src, "_averPcPad"), "no padding: b1 fills bytes 0..128 and b3 abuts it");
    check(!has(src, "cbuffer SkinParams"), "the b3 block is removed");
    check(!has(src, "cbuffer PerObject"), "the b1 block is removed");

    // One block, not two -- a second [[vk::push_constant]] is invalid SPIR-V.
    const size_t firstPc = src.find("[[vk::push_constant]]");
    check(firstPc != std::string::npos, "a push-constant block is emitted");
    check(src.find("[[vk::push_constant]]", firstPc + 1) == std::string::npos, "exactly one push-constant block");

    // b1's fields precede b3's inside the struct, because that is the order the byte offsets run in
    // -- and it is the order regardless of which block appeared first in the source, as here.
    const size_t world = src.find("float4x4 gWorld;");
    const size_t vcount = src.find("uint gVertexCount;");
    check(world != std::string::npos && vcount != std::string::npos && world < vcount,
          "b1's fields precede b3's, whatever order the source declared them in");
}

int main() {
    AVER_INFO("CbufferLayoutPatchTest -- lowering cbuffers to what the PipelineLayout says they are");
    testSlotThreePadsToOneTwentyEight();
    testObjectBlockNeedsNoPadding();
    testZeroDwordSlotBecomesADescriptor();
    testAlreadyAnnotatedIsLeftAlone();
    testUnrelatedShaderIsUntouched();
    testTwoBlocksFoldInByteOrder();

    if (g_failures == 0) {
        AVER_INFO("CbufferLayoutPatchTest: {} checks, all passed", g_checks);
        return 0;
    }
    AVER_ERROR("CbufferLayoutPatchTest: {} of {} checks FAILED", g_failures, g_checks);
    return 1;
}
