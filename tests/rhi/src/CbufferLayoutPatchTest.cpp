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
#include "aver/rhi/FrameConstants.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/rhi/ShaderFiles.hpp"
#include "aver/core/Log.hpp"

#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <string>

namespace aver::rhi::vkb {
bool patchCbuffersForLayout(std::string& src, const PipelineLayout& layout, bool mesh,
                            bool annotateDescriptors);
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
    check(vkb::patchCbuffersForLayout(src, layout, false, /*annotateDescriptors=*/true), "the patch succeeds");
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

    check(vkb::patchCbuffersForLayout(src, layout, false, /*annotateDescriptors=*/true), "the patch succeeds");
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

    check(vkb::patchCbuffersForLayout(src, layout, false, /*annotateDescriptors=*/true), "the patch succeeds");
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
    check(vkb::patchCbuffersForLayout(src, layout, false, /*annotateDescriptors=*/true), "the patch succeeds");
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
    check(vkb::patchCbuffersForLayout(src, layout, false, /*annotateDescriptors=*/true), "the patch succeeds");
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

    check(vkb::patchCbuffersForLayout(src, layout, false, /*annotateDescriptors=*/true), "the patch succeeds");
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

// ---- a slot can be FOLDED without its constantDwords saying so ------------------------------------
//
// THE BUG THIS PINS, which DXC found and no reading of the code did. pushConstantLayout places
// kObjectConstantRegister UNCONDITIONALLY -- declared or not, whatever constantDwords[1] holds. So
// a layout that leaves constantDwords[1] at zero, while its shader includes the shared prelude's
// PerObject block, has b1 folded into the push-constant struct by the first half of this function
// and, if the second half only checked constantDwords, annotated as a descriptor by the second.
// The annotation is inserted before the `cbuffer` keyword and the fold replaces the block after it,
// so the two compose into:
//     [[vk::binding(1, 2)]] struct AverPcBlock {
// which DXC rejects with "an attribute list cannot appear here" -- pointing at a line whose
// declaration no longer exists. Voxi's CSClear/CSMip/CSResolve are exactly this shape.
static void testFoldedSlotIsNeverAlsoAnnotated() {
    AVER_INFO("-- a folded slot is not also annotated, even at constantDwords 0 --");
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
        "RWTexture3D<float4> gOut : register(u0);\n"
        "[numthreads(4,4,4)] void CSClear(uint3 id : SV_DispatchThreadID) { gOut[id] = gBaseColor; }\n";

    PipelineLayout layout;
    layout.uavCount = 2;      // exactly CSClear's layout: UAVs only, constantDwords untouched
    check(layout.constantDwords[kObjectConstantRegister] == 0, "the layout really does leave b1 at zero");

    check(vkb::patchCbuffersForLayout(src, layout, false, /*annotateDescriptors=*/true), "the patch succeeds");
    check(has(src, "struct AverPcBlock"), "b1 is folded into the push-constant struct anyway");
    check(!has(src, "[[vk::binding(1"), "b1 is NOT also annotated as a descriptor");
    check(!has(src, "]] struct"), "no annotation is left stranded on the generated struct");
}

// ---- a cbuffer named only in a COMMENT is not a declaration -----------------------------------------
//
// The scanner runs over a comment-blanked copy for exactly this reason. These shaders are heavily
// commented and several comments quote the declarations this function rewrites; annotating one of
// those inserts an attribute into prose.
static void testCommentedCbufferIsNotMatched() {
    AVER_INFO("-- a cbuffer mentioned in a comment is ignored --");
    std::string src =
        "// The engine declares cbuffer PerFrame : register(b0) { float4x4 gViewProj; } in the prelude.\n"
        "/* and cbuffer Legacy : register(b2) { float4 gOld; }; was removed in 0.2 */\n"
        "float4 PSMain() : SV_Target { return 0; }\n";

    PipelineLayout layout;
    const std::string before = src;
    check(vkb::patchCbuffersForLayout(src, layout, false, /*annotateDescriptors=*/true), "the patch succeeds");
    check(src == before, "the source is byte-identical -- neither comment was touched");
}

// ---- PerFrame at 784 bytes: the floating origin's two rows (LARGE_WORLD 2.2.6) ------------------------
//
// PerFrameCB is one C++ struct and one HLSL cbuffer, and until now only a size assert tied them. This
// counts the cbuffer's rows from the shipped prelude text, so a row added on one side only fails here.
// The Vulkan lowering is generic (it annotates the block and never reads its fields), so the second
// half feeds it the block with the new rows to show they pass through untouched.
static std::string stripComments(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s.compare(i, 2, "//") == 0) { while (i < s.size() && s[i] != '\n') ++i; o += '\n'; continue; }
        if (s.compare(i, 2, "/*") == 0) { const size_t e = s.find("*/", i + 2); i = e == std::string::npos ? s.size() : e + 1; continue; }
        o += s[i];
    }
    return o;
}

static void testPerFrameIsSevenEightyFour() {
    AVER_INFO("-- PerFrameCB and cbuffer PerFrame agree at 784 bytes, origin rows last --");
    check(sizeof(PerFrameCB) == 784, "sizeof(PerFrameCB) == 784");
    check(offsetof(PerFrameCB, worldOrigin) == 752, "worldOrigin at 752");
    check(offsetof(PerFrameCB, wavePhase) == 768, "wavePhase at 768");
    check(offsetof(PerFrameCB, jitter) + sizeof(PerFrameCB::jitter) == offsetof(PerFrameCB, worldOrigin),
          "the origin rows are appended: nothing before them moved");

    const std::string text = stripComments(shaderFile("shared_prelude.hlsl"));
    const size_t head = text.find("cbuffer PerFrame");
    check(head != std::string::npos, "the prelude declares cbuffer PerFrame");
    if (head == std::string::npos) return;
    const size_t open = text.find('{', head);
    const size_t close = text.find('}', open);
    check(open != std::string::npos && close != std::string::npos, "the block has a body");
    if (open == std::string::npos || close == std::string::npos) return;

    size_t rows = 0;
    std::string lastNames[2];
    size_t pos = open + 1;
    while (pos < close) {
        const size_t semi = text.find(';', pos);
        if (semi == std::string::npos || semi > close) break;
        std::string decl = text.substr(pos, semi - pos);
        pos = semi + 1;
        size_t b = 0;
        while (b < decl.size() && std::isspace(static_cast<unsigned char>(decl[b]))) ++b;
        size_t t = b;
        while (t < decl.size() && !std::isspace(static_cast<unsigned char>(decl[t]))) ++t;
        const std::string type = decl.substr(b, t - b);
        size_t n = t;
        while (n < decl.size() && std::isspace(static_cast<unsigned char>(decl[n]))) ++n;
        size_t e = n;
        while (e < decl.size() && (std::isalnum(static_cast<unsigned char>(decl[e])) || decl[e] == '_')) ++e;
        const std::string name = decl.substr(n, e - n);
        size_t count = 1;
        if (e < decl.size() && decl[e] == '[') count = static_cast<size_t>(std::atoi(decl.c_str() + e + 1));
        const size_t per = type == "float4x4" ? 4 : (type == "float4" ? 1 : 0);
        check(per != 0, "every PerFrame field is a float4 or float4x4 (" + name + ")");
        rows += per * count;
        lastNames[0] = lastNames[1];
        lastNames[1] = name;
    }
    check(rows * 16 == sizeof(PerFrameCB), "the HLSL cbuffer is exactly sizeof(PerFrameCB) bytes of rows");
    check(lastNames[0] == "gWorldOrigin" && lastNames[1] == "gWavePhase", "the last two rows are gWorldOrigin, gWavePhase");

    // The lowering never reads fields, so the grown block passes through byte for byte.
    std::string src =
        "[[vk::binding(0, 2)]] cbuffer PerFrame : register(b0) {\n"
        "    float4 gTaaJitter;\n"
        "    float4 gWorldOrigin;\n"
        "    float4 gWavePhase;\n"
        "};\n"
        "float4 PSMain() : SV_Target { return gWorldOrigin + gWavePhase; }\n";
    PipelineLayout layout;
    const std::string before = src;
    check(vkb::patchCbuffersForLayout(src, layout, false, /*annotateDescriptors=*/true), "the patch succeeds");
    check(src == before, "an annotated PerFrame with the origin rows is left byte-identical");
}

int main() {
    AVER_INFO("CbufferLayoutPatchTest -- lowering cbuffers to what the PipelineLayout says they are");
    testPerFrameIsSevenEightyFour();
    testSlotThreePadsToOneTwentyEight();
    testObjectBlockNeedsNoPadding();
    testZeroDwordSlotBecomesADescriptor();
    testAlreadyAnnotatedIsLeftAlone();
    testUnrelatedShaderIsUntouched();
    testFoldedSlotIsNeverAlsoAnnotated();
    testCommentedCbufferIsNotMatched();
    testTwoBlocksFoldInByteOrder();

    if (g_failures == 0) {
        AVER_INFO("CbufferLayoutPatchTest: {} checks, all passed", g_checks);
        return 0;
    }
    AVER_ERROR("CbufferLayoutPatchTest: {} of {} checks FAILED", g_failures, g_checks);
    return 1;
}
