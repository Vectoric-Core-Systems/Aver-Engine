// MaterialResidencyTest -- phased material residency: a material is built at once with stand-ins and its textures swap in as they
// become ready, within a per-frame upload budget.
//
// A recording RHI and a fake async resolver stand in for the GPU and the texture workers, so the cases are exact: what a pending
// slot shows (the stand-in rule per slot and alpha mode), that nothing waits (the build returns with every texture pending), that
// textures land in the order and at the pace the budget allows, that a texture two materials share uploads once, that a failure
// leaves the identity fallback and stops asking, that prefetch only queues, and that a material destroyed or a device shut down
// while textures are pending leaves nothing behind (no upload for the dead, a clean re-init).
#include "MockRhi.hpp"
#include "aver/core/Log.hpp"
#include "aver/pbr/MaterialSystem.hpp"

#include <map>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::pbr;
using aver::test::MockDevice;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

namespace {

// What the fake workers say about each texture path.
enum class Fake { Pending, Ready, Failed };
struct FakeLoader {
    MockDevice* dev = nullptr;
    std::map<std::string, Fake> state;
    std::map<std::string, rhi::TextureHandle> uploaded;   // path -> its GPU texture, once taken
    std::map<std::string, u32> takes, waits, prefetches;  // asks by kind
    usize bytes = 1u << 20;
    u32 totalTakes() const { u32 n = 0; for (const auto& kv : takes) n += kv.second; return n; }
};
FakeLoader g;

MaterialSystem::ResolvedTexture fakeResolve(const TextureRef& ref, TextureSlot, MaterialSystem::TextureAsk ask, void*) {
    using Ask = MaterialSystem::TextureAsk;
    MaterialSystem::ResolvedTexture r;
    const auto it = g.state.find(ref.path);
    const Fake s = it == g.state.end() ? Fake::Pending : it->second;
    if (ask == Ask::Prefetch) ++g.prefetches[ref.path];
    if (ask == Ask::Wait) ++g.waits[ref.path];
    if (s == Fake::Failed) return r;
    if (s == Fake::Pending || ask != Ask::Take) { r.pending = true; return r; }
    ++g.takes[ref.path];
    rhi::TextureDesc d;
    d.width = d.height = 256;
    d.debugName = ref.path.c_str();
    r.handle = g.dev->factory.createTexture(d);
    r.bytes = g.bytes;
    g.uploaded[ref.path] = r.handle;
    return r;
}

MaterialDesc textured(const std::string& name, const std::string& base, const std::string& normal = {},
                      const std::string& mr = {}, const std::string& emissive = {}) {
    MaterialDesc d;
    d.name = name;
    d.textures[static_cast<u32>(TextureSlot::BaseColor)].path = base;
    d.textures[static_cast<u32>(TextureSlot::Normal)].path = normal;
    d.textures[static_cast<u32>(TextureSlot::MetalRough)].path = mr;
    d.textures[static_cast<u32>(TextureSlot::Emissive)].path = emissive;
    return d;
}

std::string shown(MockDevice& dev, rhi::BindingSetHandle set, TextureSlot slot) {
    return dev.factory.nameOf(dev.factory.at(set, static_cast<u32>(slot)));
}

void reset(MockDevice& dev) {
    g = FakeLoader{};
    g.dev = &dev;
}

void testStandInRule() {
    AVER_INFO("=== the stand-in rule, per slot and alpha mode ===");
    MaterialDesc opaque;
    MaterialDesc mask; mask.alphaMode = AlphaMode::Mask;
    MaterialDesc blend; blend.alphaMode = AlphaMode::Blend;
    using S = MaterialSystem::StandIn;
    check(MaterialSystem::standInFor(TextureSlot::BaseColor, opaque) == S::Grey, "an opaque base colour shows mid grey times its factor");
    check(MaterialSystem::standInFor(TextureSlot::BaseColor, mask) == S::Hidden, "a masked base colour is hidden (alpha 0)");
    check(MaterialSystem::standInFor(TextureSlot::BaseColor, blend) == S::Hidden, "a blended base colour is hidden");
    check(MaterialSystem::standInFor(TextureSlot::MetalRough, opaque) == S::Dielectric, "metal-rough is rough and non-metal");
    check(MaterialSystem::standInFor(TextureSlot::Emissive, opaque) == S::Dark, "emissive does not glow");
    check(MaterialSystem::standInFor(TextureSlot::Normal, opaque) == S::Identity, "a normal map shows the flat normal");
    check(MaterialSystem::standInFor(TextureSlot::Occlusion, opaque) == S::Identity, "occlusion shows white");
}

void testBuildDoesNotWait() {
    AVER_INFO("=== a material is built at once with stand-ins, then refines ===");
    MockDevice dev;
    reset(dev);
    MaterialSystem ms;
    check(ms.init(dev, 0), "the material system initialises on the recording device");
    ms.setAsyncTextureResolver(&fakeResolve, nullptr);

    MaterialDesc d = textured("Brick", "t/brick.png", "t/brick_n.png", "t/brick_mr.png", "t/brick_e.png");
    const MaterialHandle h = MaterialLibrary::get().create(d);
    ms.update();
    const rhi::BindingSetHandle set = ms.bindingSet(h);
    check(set != 0 && set != ms.fallbackBindingSet(), "the binding set exists on first ask");
    check(g.totalTakes() == 0, "no texture was uploaded: every one is still pending");
    check(!ms.fullyResident(h) && ms.pendingMaterialCount() == 1, "the material reports itself not resident");
    check(shown(dev, set, TextureSlot::BaseColor) == "pbr stand-in grey", "base colour: grey stand-in");
    check(shown(dev, set, TextureSlot::Normal) == "pbr fallback normal", "normal: flat");
    check(shown(dev, set, TextureSlot::MetalRough) == "pbr stand-in dielectric", "metal-rough: dielectric");
    check(shown(dev, set, TextureSlot::Emissive) == "pbr fallback zero", "emissive: dark");
    check(shown(dev, set, TextureSlot::Occlusion) == "pbr fallback white", "occlusion has no map at all: the usual white");
    f32 avg[3] = {0, 0, 0};
    check(ms.averageBaseColor(set, avg) && avg[0] > 0.2f && avg[0] < 0.22f, "the average colour a ray path reads is the grey stand-in, not white");

    // Only the base colour lands.
    g.state["t/brick.png"] = Fake::Ready;
    ms.update();
    check(g.takes["t/brick.png"] == 1, "the ready base colour is taken once");
    check(dev.factory.nameOf(dev.factory.at(set, 0)) == "t/brick.png", "and now bound in place of the stand-in");
    check(shown(dev, set, TextureSlot::Normal) == "pbr fallback normal", "the others still show theirs");
    check(!ms.fullyResident(h), "still pending: three textures are not here");
    // The rest.
    for (const char* p : {"t/brick_n.png", "t/brick_mr.png", "t/brick_e.png"}) g.state[p] = Fake::Ready;
    ms.update();
    check(ms.fullyResident(h) && ms.pendingMaterialCount() == 0, "all four resident: nothing pending");
    check(shown(dev, set, TextureSlot::MetalRough) == "t/brick_mr.png", "metal-rough replaced the stand-in");
    ms.update();
    check(g.totalTakes() == 4, "later updates ask for nothing more (4 uploads in all)");
    MaterialLibrary::get().destroy(h);
    ms.update();
    ms.shutdown();
}

void testMaskedHidden() {
    AVER_INFO("=== a masked material's surface is hidden until its texture lands ===");
    MockDevice dev;
    reset(dev);
    MaterialSystem ms;
    ms.init(dev, 0);
    ms.setAsyncTextureResolver(&fakeResolve, nullptr);
    MaterialDesc d = textured("Leaf", "t/leaf.png");
    d.alphaMode = AlphaMode::Mask;
    const MaterialHandle h = MaterialLibrary::get().create(d);
    const rhi::BindingSetHandle set = ms.bindingSet(h);
    check(shown(dev, set, TextureSlot::BaseColor) == "pbr fallback zero", "alpha 0: nothing is drawn as a solid card");
    g.state["t/leaf.png"] = Fake::Ready;
    ms.update();
    check(shown(dev, set, TextureSlot::BaseColor) == "t/leaf.png", "the leaf texture replaces it");
    MaterialLibrary::get().destroy(h);
    ms.update();
    ms.shutdown();
}

void testBudget() {
    AVER_INFO("=== uploads are budgeted per update ===");
    MockDevice dev;
    reset(dev);
    g.bytes = 5u << 20;   // 5 MiB each against the default 8 MiB budget
    MaterialSystem ms;
    ms.init(dev, 0);
    ms.setAsyncTextureResolver(&fakeResolve, nullptr);
    MaterialDesc d = textured("Big", "t/a.png", "t/b.png", "t/c.png");
    const MaterialHandle h = MaterialLibrary::get().create(d);
    ms.bindingSet(h);
    for (const char* p : {"t/a.png", "t/b.png", "t/c.png"}) g.state[p] = Fake::Ready;
    ms.update();
    check(g.totalTakes() == 2, "5 MiB then 5 MiB: the second still fits the 8 MiB budget at the moment it starts, the third waits");
    check(g.waits["t/b.png"] >= 1, "the third was asked as Wait (demanded, no upload) so the workers know it is wanted");
    check(!ms.fullyResident(h), "so the material is not resident yet");
    ms.update();
    check(g.totalTakes() == 3 && ms.fullyResident(h), "the next update uploads the last one");
    ms.setUploadBudget(1);
    check(ms.uploadBudget() == 1, "the budget can be changed");
    MaterialLibrary::get().destroy(h);
    ms.update();
    ms.shutdown();
}

void testSharedTexture() {
    AVER_INFO("=== a texture two materials name uploads once ===");
    MockDevice dev;
    reset(dev);
    MaterialSystem ms;
    ms.init(dev, 0);
    ms.setAsyncTextureResolver(&fakeResolve, nullptr);
    const MaterialHandle a = MaterialLibrary::get().create(textured("A", "t/shared.png"));
    const MaterialHandle b = MaterialLibrary::get().create(textured("B", "t/shared.png"));
    const rhi::BindingSetHandle sa = ms.bindingSet(a), sb = ms.bindingSet(b);
    g.state["t/shared.png"] = Fake::Ready;
    ms.update();
    check(g.takes["t/shared.png"] == 1, "one upload for two materials");
    check(ms.fullyResident(a) && ms.fullyResident(b), "both resident");
    check(dev.factory.at(sa, 0) == dev.factory.at(sb, 0) && dev.factory.at(sa, 0) == g.uploaded["t/shared.png"], "and both bind that texture");
    MaterialLibrary::get().destroy(a);
    ms.update();
    check(!dev.factory.destroyed.count(g.uploaded["t/shared.png"]), "releasing one material keeps the texture the other uses");
    MaterialLibrary::get().destroy(b);
    ms.update();
    check(dev.factory.destroyed.count(g.uploaded["t/shared.png"]) == 1, "releasing the last destroys it");
    ms.shutdown();
}

void testFailure() {
    AVER_INFO("=== a texture that cannot be had leaves the identity fallback and stops being asked for ===");
    MockDevice dev;
    reset(dev);
    MaterialSystem ms;
    ms.init(dev, 0);
    ms.setAsyncTextureResolver(&fakeResolve, nullptr);
    g.state["t/missing.png"] = Fake::Failed;
    const MaterialHandle h = MaterialLibrary::get().create(textured("Broken", "t/missing.png", "t/n.png"));
    const rhi::BindingSetHandle set = ms.bindingSet(h);
    check(shown(dev, set, TextureSlot::BaseColor) == "pbr fallback white", "base colour: the identity white, as with no map");
    check(ms.failedResolveCount() == 1, "the failure is remembered");
    check(!ms.fullyResident(h), "the other texture is still pending");
    g.state["t/n.png"] = Fake::Ready;
    ms.update();
    check(ms.fullyResident(h), "a failed texture does not hold the material pending");
    ms.update();
    check(g.totalTakes() == 1, "the failure is not asked for again");
    MaterialLibrary::get().destroy(h);
    ms.update();
    ms.shutdown();
}

void testPrefetch() {
    AVER_INFO("=== prefetch only queues ===");
    MockDevice dev;
    reset(dev);
    MaterialSystem ms;
    ms.init(dev, 0);
    ms.setAsyncTextureResolver(&fakeResolve, nullptr);
    const MaterialHandle h = MaterialLibrary::get().create(textured("Pre", "t/p1.png", "t/p2.png"));
    g.state["t/p1.png"] = Fake::Ready;
    ms.prefetch(h);
    check(g.prefetches["t/p1.png"] == 1 && g.prefetches["t/p2.png"] == 1, "each textured slot was queued once");
    check(g.totalTakes() == 0, "even a ready texture is not uploaded by a prefetch");
    check(ms.pendingMaterialCount() == 0, "and no material was built");
    ms.bindingSet(h);
    ms.update();
    check(g.takes["t/p1.png"] == 1, "the draw takes the ready one, with only the copy left to do");
    ms.prefetch(h);
    check(g.prefetches["t/p1.png"] == 1, "a resident texture is not queued again");
    MaterialLibrary::get().destroy(h);
    ms.update();
    ms.shutdown();
}

void testLifetime() {
    AVER_INFO("=== a material destroyed or a device shut down while textures are pending ===");
    MockDevice dev;
    reset(dev);
    MaterialSystem ms;
    ms.init(dev, 0);
    ms.setAsyncTextureResolver(&fakeResolve, nullptr);
    const MaterialHandle gone = MaterialLibrary::get().create(textured("Gone", "t/gone.png"));
    const MaterialHandle kept = MaterialLibrary::get().create(textured("Kept", "t/kept.png"));
    ms.bindingSet(gone);
    ms.bindingSet(kept);
    check(ms.pendingMaterialCount() == 2, "two pending");
    MaterialLibrary::get().destroy(gone);   // the level changed
    g.state["t/gone.png"] = Fake::Ready;    // its worker finishes anyway
    g.state["t/kept.png"] = Fake::Ready;
    ms.update();
    check(g.takes["t/gone.png"] == 0, "the destroyed material's texture is never uploaded");
    check(g.takes["t/kept.png"] == 1 && ms.pendingMaterialCount() == 0, "the surviving material completes");
    // Shutdown with one pending, then a fresh init on the same object.
    const MaterialHandle again = MaterialLibrary::get().create(textured("Again", "t/again.png"));
    ms.bindingSet(again);
    check(ms.pendingMaterialCount() == 1, "one pending again");
    ms.shutdown();
    check(ms.pendingMaterialCount() == 0 && !ms.ready(), "shutdown forgets the pending list");
    MockDevice dev2;
    g.dev = &dev2;
    check(ms.init(dev2, 0), "re-init on a new device");
    ms.setAsyncTextureResolver(&fakeResolve, nullptr);
    g.state["t/again.png"] = Fake::Ready;
    const rhi::BindingSetHandle set = ms.bindingSet(again);
    ms.update();
    check(ms.fullyResident(again) && dev2.factory.nameOf(dev2.factory.at(set, 0)) == "t/again.png", "the material builds and completes on the new device");
    MaterialLibrary::get().destroy(kept);
    MaterialLibrary::get().destroy(again);
    ms.update();
    ms.shutdown();
}

MaterialSystem::ResolvedTexture blockingResolve(const TextureRef& ref, TextureSlot, void*) {
    MaterialSystem::ResolvedTexture r;
    rhi::TextureDesc d;
    d.debugName = ref.path.c_str();
    r.handle = g.dev->factory.createTexture(d);
    return r;
}

void testBlockingResolverUnchanged() {
    AVER_INFO("=== a host with only the blocking resolver behaves as before ===");
    MockDevice dev;
    reset(dev);
    MaterialSystem ms;
    ms.init(dev, 0);
    ms.setTextureResolver(&blockingResolve, nullptr);
    const MaterialHandle h = MaterialLibrary::get().create(textured("Old", "t/old.png"));
    const rhi::BindingSetHandle set = ms.bindingSet(h);
    check(shown(dev, set, TextureSlot::BaseColor) == "t/old.png" && ms.fullyResident(h), "the texture is bound at the first ask, nothing pending");
    MaterialLibrary::get().destroy(h);
    ms.update();
    ms.shutdown();
}

} // namespace

int main() {
    testStandInRule();
    testBuildDoesNotWait();
    testMaskedHidden();
    testBudget();
    testSharedTexture();
    testFailure();
    testPrefetch();
    testLifetime();
    testBlockingResolverUnchanged();
    if (g_failures) { AVER_ERROR("MaterialResidencyTest: {} failure(s)", g_failures); return 1; }
    AVER_INFO("MaterialResidencyTest: all passed");
    return 0;
}
