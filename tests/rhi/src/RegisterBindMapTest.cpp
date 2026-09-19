// buildRegisterBinds: where each HLSL t/u/s/b register has to land in Vulkan's descriptor sets.
//
// NO DEVICE, NO SDK, NO VULKAN HEADERS -- VulkanRegisterMap.hpp is separable from VulkanCommon.hpp
// precisely so this file can exist (see that header). What is under test is arithmetic over a
// PipelineLayout, and arithmetic does not need a GPU.
//
// THE BUG THIS PINS. DXC maps HLSL register SPACE onto the SPIR-V descriptor SET, and every shader
// in this engine declares everything in the default space0 -- deliberately, because a space means
// nothing to the D3D12 root signature reading the same text. Without an explicit map every t/u/s
// register lands in set 0 at binding == register number, and two things break at once:
//   - table 1's SRVs stay in set 0. PBR bases its material textures at t9 with srvCount 9, so
//     gBaseColorMap belongs at set 1 binding 0 and was landing at set 0 binding 9, which table 0
//     does not declare: "binding was not declared in pSetLayouts[0]".
//   - samplers collide with textures. s1/s2 land at set 0 bindings 1/2, where table 0 has already
//     declared SAMPLED_IMAGEs: "VkDescriptorType mismatch".
// Both were real, both were reported by the validation layer, and the numbers below are the ones
// that were actually observed on an RX 7800 XT before the fix.
#include "VulkanRegisterMap.hpp"
#include "aver/core/Log.hpp"

#include <string>

using namespace aver;
using namespace aver::rhi;
using aver::rhi::vkb::VkRegisterBind;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// The set/binding a register was mapped to, or {-1,-1} if it was not mapped at all.
struct Where { int set = -1; int binding = -1; };
static Where find(const VkRegisterBind* b, u32 n, char type, u32 number) {
    for (u32 i = 0; i < n; ++i)
        if (b[i].type == type && b[i].number == number)
            return Where{static_cast<int>(b[i].set), static_cast<int>(b[i].binding)};
    return Where{};
}

// Literals, NOT kVkSetTable0/kVkSetSamplers. A test that re-derives its expectations from the same
// constants the code under test uses proves only that one file is self-consistent; these are the
// set indices descriptorLayout() actually builds, written out so a change to either has to be
// deliberate.
static constexpr int kSetTable0 = 0;
static constexpr int kSetTable1 = 1;
static constexpr int kSetConstants = 2;
static constexpr int kSetSamplers = 3;
static constexpr int kSetInstances = 4;

// THE UAV BASE IS DERIVED, AND IT IS THE ONE EXCEPTION TO THE RULE ABOVE. A set index is an
// architectural choice -- table 0 is set 0 and always will be -- so pinning those as literals makes
// a change to either side deliberate, which is the point. kVkUavBindingBase is not that kind of
// number: it is defined as kMaxBindingSlots, a CAPACITY that is expected to grow whenever a table
// needs more slots, and the invariant worth testing is where UAVs sit RELATIVE to the SRV range,
// not what that number happens to be this month.
//
// Written as the literal 16 it was silently wrong for four days. optimisation-wave-2 (501a1bb6)
// raised kMaxBindingSlots 16 -> 24 because Voxi's table grew to 17 SRVs and 11 UAVs; the
// implementation moved with it, as it must -- UAVs based at 16 would now collide with SRVs 16..23 --
// and this copy did not, so five checks failed and nobody saw, because the suite had never been run.
//
// Derived from rhi::kMaxBindingSlots rather than from kVkUavBindingBase itself, which keeps the
// contract under test: if the Vulkan backend ever based its UAVs somewhere OTHER than
// kMaxBindingSlots, these checks would still catch it.
static constexpr int kUavBase = static_cast<int>(kMaxBindingSlots);
static const std::string kUavBaseStr  = std::to_string(kUavBase);
static const std::string kUavBase1Str = std::to_string(kUavBase + 1);

// ---- the exact shape that was failing: Voxi's GI layout ------------------------------------------
//
// srvCount 9 (kGiSrvCount), srvCount1 8 (kMaterialSrvCount), uavCount 4 (kGiUavCount), 3 samplers.
// PBR logs this at startup as "material system ready: 8 slots based at t9".
static void testVoxiGiLayout() {
    AVER_INFO("-- Voxi's GI layout: the shape the four validation errors came from --");
    PipelineLayout l;
    l.srvCount = 9;
    l.uavCount = 4;
    l.srvCount1 = 8;
    l.samplerCount = 3;
    l.constantDwords[kObjectConstantRegister] = kObjectConstantDwords;

    VkRegisterBind binds[vkb::kMaxRegisterBinds];
    const u32 n = vkb::buildRegisterBinds(l, binds, vkb::kMaxRegisterBinds);

    // Table 0's SRVs are already where they need to be; t8 (gGiShadowTex) never errored.
    check(find(binds, n, 't', 0).set == kSetTable0 && find(binds, n, 't', 0).binding == 0, "t0 -> set 0, binding 0");
    check(find(binds, n, 't', 8).set == kSetTable0 && find(binds, n, 't', 8).binding == 8, "t8 -> set 0, binding 8");

    // THE FIRST BUG. t9 is table 1's FIRST register, so it restarts at binding 0 in set 1 -- it was
    // landing at set 0 binding 9, which table 0 (9 bindings, 0..8) does not declare.
    check(find(binds, n, 't', 9).set == kSetTable1, "t9 (gBaseColorMap) -> set 1, not set 0");
    check(find(binds, n, 't', 9).binding == 0, "t9 restarts at binding 0 within its own set");
    check(find(binds, n, 't', 14).set == kSetTable1 && find(binds, n, 't', 14).binding == 5,
          "t14 (gL1BaseColorMap) -> set 1, binding 5");
    check(find(binds, n, 't', 16).binding == 7, "t16, table 1's last, -> binding 7");
    check(find(binds, n, 't', 17).set == -1, "t17 is past both tables and is NOT mapped");

    // THE SECOND BUG. Samplers were colliding with table 0's textures at set 0 bindings 1 and 2.
    check(find(binds, n, 's', 1).set == kSetSamplers, "s1 (gShadowSamp) -> set 3, not set 0");
    check(find(binds, n, 's', 1).binding == 1, "s1 -> binding 1");
    check(find(binds, n, 's', 2).set == kSetSamplers && find(binds, n, 's', 2).binding == 2,
          "s2 (gMaterialSampler) -> set 3, binding 2");
    check(find(binds, n, 's', 3).set == -1, "s3 is beyond samplerCount and is NOT mapped");

    // UAVs were already correct, via -fvk-u-shift. The map has to reproduce that exactly, because
    // supplying a map SUPPRESSES the shift -- the two are mutually exclusive in DXC.
    check(find(binds, n, 'u', 0).set == kSetTable0 && find(binds, n, 'u', 0).binding == kUavBase,
          "u0 -> set 0, binding " + kUavBaseStr + " (the shift's own answer)");
    check(find(binds, n, 'u', 1).binding == kUavBase + 1,
          "u1 (gVoxelAccum) -> binding " + kUavBase1Str);

    // b1 is root constants here, so it is folded into [[vk::push_constant]] and its register is
    // DELETED from the source -- there is no resource left for DXC to want a mapping for.
    check(find(binds, n, 'b', kObjectConstantRegister).set == -1, "b1 is push constants and is NOT mapped");
    // b0 and b4 are descriptors: dynamic UBOs at binding == register in set 2.
    check(find(binds, n, 'b', 0).set == kSetConstants && find(binds, n, 'b', 0).binding == 0, "b0 -> set 2, binding 0");
    check(find(binds, n, 'b', 4).set == kSetConstants && find(binds, n, 'b', 4).binding == 4,
          "b4 (VoxiFrame) -> set 2, binding 4");
}

// ---- table 1 is based on srvCount, so moving srvCount moves it -------------------------------------
static void testTableOneIsRelativeToTableZero() {
    AVER_INFO("-- table 1's base register follows srvCount --");
    PipelineLayout l;
    l.srvCount = 2;
    l.srvCount1 = 3;

    VkRegisterBind binds[vkb::kMaxRegisterBinds];
    const u32 n = vkb::buildRegisterBinds(l, binds, vkb::kMaxRegisterBinds);

    check(find(binds, n, 't', 1).set == kSetTable0 && find(binds, n, 't', 1).binding == 1, "t1 -> set 0, binding 1");
    // t2, not t9 -- HLSL numbers table 1 continuously above table 0 because that is what D3D12's
    // root signature wants, and only this backend splits them into separate sets.
    check(find(binds, n, 't', 2).set == kSetTable1 && find(binds, n, 't', 2).binding == 0, "t2 -> set 1, binding 0");
    check(find(binds, n, 't', 4).set == kSetTable1 && find(binds, n, 't', 4).binding == 2, "t4 -> set 1, binding 2");
    check(find(binds, n, 't', 5).set == -1, "t5 is past table 1 and is NOT mapped");
}

// ---- UAVs split across the two tables the same way -------------------------------------------------
static void testUavsSplitAcrossTables() {
    AVER_INFO("-- table 1's UAVs restart at the UAV base within their own set --");
    PipelineLayout l;
    l.uavCount = 2;
    l.uavCount1 = 2;

    VkRegisterBind binds[vkb::kMaxRegisterBinds];
    const u32 n = vkb::buildRegisterBinds(l, binds, vkb::kMaxRegisterBinds);

    check(find(binds, n, 'u', 1).set == kSetTable0 && find(binds, n, 'u', 1).binding == kUavBase + 1,
          "u1 -> set 0, binding " + kUavBase1Str);
    check(find(binds, n, 'u', 2).set == kSetTable1 && find(binds, n, 'u', 2).binding == kUavBase,
          "u2 -> set 1, restarting at binding " + kUavBaseStr);
    check(find(binds, n, 'u', 3).set == kSetTable1 && find(binds, n, 'u', 3).binding == kUavBase + 1,
          "u3 -> set 1, binding " + kUavBase1Str);
}

// ---- the map has to be COMPLETE, which is why cbuffers are in it -----------------------------------
//
// DXC refuses a partial map outright: "error: missing -fvk-bind-register for resource". Every
// descriptor cbuffer must therefore appear, and every FOLDED one must not.
static void testDescriptorCbuffersArePresentAndFoldedOnesAreNot() {
    AVER_INFO("-- descriptor cbuffers are mapped; root-constant ones are not --");
    PipelineLayout l;
    l.srvCount = 1;                    // forces a non-empty map, which is what makes the rest required
    l.constantDwords[1] = kObjectConstantDwords;
    l.constantDwords[3] = 4;

    VkRegisterBind binds[vkb::kMaxRegisterBinds];
    const u32 n = vkb::buildRegisterBinds(l, binds, vkb::kMaxRegisterBinds);

    check(find(binds, n, 'b', 1).set == -1, "b1 (root constants) is NOT mapped");
    check(find(binds, n, 'b', 3).set == -1, "b3 (root constants) is NOT mapped");
    check(find(binds, n, 'b', 0).set == kSetConstants, "b0 (a descriptor) IS mapped");
    check(find(binds, n, 'b', 2).set == kSetConstants, "b2 (a descriptor) IS mapped");
    check(find(binds, n, 'b', 4).set == kSetConstants, "b4 (a descriptor) IS mapped");
}

// ---- an empty layout asks for nothing, which is how moduleForLayout skips the whole mechanism ------
static void testEmptyLayoutStillMapsItsConstants() {
    AVER_INFO("-- a layout with no tables still maps its constant slots --");
    PipelineLayout l;

    VkRegisterBind binds[vkb::kMaxRegisterBinds];
    const u32 n = vkb::buildRegisterBinds(l, binds, vkb::kMaxRegisterBinds);

    check(find(binds, n, 't', 0).set == -1, "no SRVs are mapped");
    check(find(binds, n, 'u', 0).set == -1, "no UAVs are mapped");
    check(find(binds, n, 's', 0).set == -1, "no samplers are mapped");
    // Every slot is zero-dword by default, so all five are descriptors.
    check(n == kMaxConstantSlots, "exactly the five constant slots are mapped");
}

// ---- every register the biggest possible layout declares still fits --------------------------------
static void testMaxLayoutFits() {
    AVER_INFO("-- kMaxRegisterBinds is large enough for the widest layout --");
    PipelineLayout l;
    l.srvCount = kMaxBindingSlots;
    l.uavCount = kMaxBindingSlots;
    l.srvCount1 = kMaxBindingSlots;
    l.uavCount1 = kMaxBindingSlots;
    l.samplerCount = 4;

    VkRegisterBind binds[vkb::kMaxRegisterBinds];
    const u32 n = vkb::buildRegisterBinds(l, binds, vkb::kMaxRegisterBinds);

    const u32 expected = 4 * kMaxBindingSlots + 4 + kMaxConstantSlots;
    check(n == expected, "all " + std::to_string(expected) + " registers are emitted, none dropped");
    check(n <= vkb::kMaxRegisterBinds, "the count stays within kMaxRegisterBinds");
    // The last sampler is the one a too-small buffer would silently drop.
    check(find(binds, n, 's', 3).set == kSetSamplers, "the last sampler survived");

    // ...and the same layout WITH instancing must still fit, which is the case that sized
    // kMaxRegisterBinds's +1. A buffer one short here would drop a register silently, and the
    // dropped one would be gInstanceWorlds, since it is emitted last.
    const u32 ni = vkb::buildRegisterBinds(l, binds, vkb::kMaxRegisterBinds, /*instanced=*/true);
    check(ni == expected + 1, "instancing adds exactly one register to a maximal layout");
    check(ni <= vkb::kMaxRegisterBinds, "kMaxRegisterBinds is big enough for the instanced maximum");
}

// gInstanceWorlds: the register D3D12 reserves as a root SRV one past every declared t-register, and
// which the shared HLSL names via AVER_INSTANCE_SRV = rhi::declaredSrvCount(layout). Both backends
// must derive the SAME number from the same layout or they disagree silently -- D3D12 binds t17 while
// the SPIR-V expects something else, and nothing reports it.
static void testInstanceRegister() {
    PipelineLayout l{};
    l.srvCount = 9;                   // Voxi's GI table
    l.srvCount1 = 8;                  // the PBR material table above it
    l.constantDwords[1] = 32;

    VkRegisterBind binds[vkb::kMaxRegisterBinds];

    // WITHOUT instancing there must be no such mapping at all. Emitting one for a pipeline that does
    // not declare the resource would make the map describe a register the shader does not have, and
    // DXC rejects a map with an entry for a resource it cannot find.
    u32 n = vkb::buildRegisterBinds(l, binds, vkb::kMaxRegisterBinds, /*instanced=*/false);
    check(find(binds, n, 't', 17).set == -1, "no instance register when the pipeline is not instanced");

    n = vkb::buildRegisterBinds(l, binds, vkb::kMaxRegisterBinds, /*instanced=*/true);
    check(find(binds, n, 't', 17).set == kSetInstances, "t17 -> the instances set");
    check(find(binds, n, 't', 17).binding == 0, "t17 -> binding 0, the only binding in that set");
    // 9 + 8 = 17 is declaredSrvCount, so the instance register sits immediately above table 1's last.
    check(find(binds, n, 't', 16).set == kSetTable1, "t16 is still table 1's last SRV, not the instance one");

    // THE NUMBER MOVES WITH THE LAYOUT -- it is not a constant 17. A layout with fewer SRVs puts
    // gInstanceWorlds lower, and hard-coding 17 anywhere would break exactly that case.
    PipelineLayout small{};
    small.srvCount = 2;
    n = vkb::buildRegisterBinds(small, binds, vkb::kMaxRegisterBinds, /*instanced=*/true);
    check(find(binds, n, 't', 2).set == kSetInstances, "a 2-SRV layout puts the instance register at t2");
    check(find(binds, n, 't', 17).set == -1, "and nothing at t17");

    // The collision this set exists to avoid: a layout declaring the full 16 SRVs would put the
    // instance register at t16, which inside set 0 is kVkUavBindingBase -- on top of UAV slot 0.
    PipelineLayout full{};
    full.srvCount = kMaxBindingSlots;
    full.uavCount = 1;
    n = vkb::buildRegisterBinds(full, binds, vkb::kMaxRegisterBinds, /*instanced=*/true);
    const Where inst = find(binds, n, 't', kMaxBindingSlots);
    const Where uav0 = find(binds, n, 'u', 0);
    check(inst.set == kSetInstances, "a full 16-SRV layout still puts the instance register in its own set");
    check(!(inst.set == uav0.set && inst.binding == uav0.binding),
          "and it therefore cannot collide with UAV slot 0, which sits at binding " + kUavBaseStr +
          " of set 0");
}

int main() {
    AVER_INFO("RegisterBindMapTest -- HLSL registers to Vulkan descriptor sets");
    testVoxiGiLayout();
    testTableOneIsRelativeToTableZero();
    testUavsSplitAcrossTables();
    testDescriptorCbuffersArePresentAndFoldedOnesAreNot();
    testEmptyLayoutStillMapsItsConstants();
    testMaxLayoutFits();
    testInstanceRegister();

    if (g_failures == 0) {
        AVER_INFO("RegisterBindMapTest: {} checks, all passed", g_checks);
        return 0;
    }
    AVER_ERROR("RegisterBindMapTest: {} of {} checks FAILED", g_failures, g_checks);
    return 1;
}
