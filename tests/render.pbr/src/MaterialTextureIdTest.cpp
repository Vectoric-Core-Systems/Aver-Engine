// MaterialTextureIdTest -- MaterialSystem owns the ray path's texture ids (docs/plans/wave4/TEXTURE_STREAMING.md 2.6).
//
// A recording RHI and a blocking resolver; no GPU. What is asserted: ids are unique per texture and stable for its
// life (asking again, a dirty re-upload of the material that names it, a second material sharing it), a null handle
// is unbound, the fallback textures get ids too (the ray path samples what raster binds), an id is not handed to a
// new texture until two update() calls after its texture was destroyed, the pool is capped at kMaterialTexIdCap and
// shutdown starts over.
#include "MockRhi.hpp"
#include "aver/core/Log.hpp"
#include "aver/pbr/MaterialSystem.hpp"

#include <cstdint>
#include <map>
#include <set>
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

MockDevice* g_dev = nullptr;

// One new texture per distinct path, as the real resolver's cache hands out.
MaterialSystem::ResolvedTexture resolve(const TextureRef& ref, TextureSlot, void*) {
    static std::map<std::string, rhi::TextureHandle> made;
    MaterialSystem::ResolvedTexture r;
    if (ref.path.empty()) return r;
    const std::string key = std::to_string(reinterpret_cast<std::uintptr_t>(g_dev)) + ref.path;
    auto it = made.find(key);
    if (it == made.end()) {
        rhi::TextureDesc d;
        d.debugName = ref.path.c_str();
        it = made.emplace(key, g_dev->factory.createTexture(d)).first;
    }
    r.handle = it->second;
    return r;
}

MaterialDesc textured(const std::string& name, const std::string& base, const std::string& normal = {}) {
    MaterialDesc d;
    d.name = name;
    d.textures[static_cast<u32>(TextureSlot::BaseColor)].path = base;
    d.textures[static_cast<u32>(TextureSlot::Normal)].path = normal;
    return d;
}

void testIds() {
    AVER_INFO("=== ids are unique, stable and shared ===");
    MockDevice dev;
    g_dev = &dev;
    MaterialSystem ms;
    ms.init(dev, 0);
    ms.setTextureResolver(&resolve, nullptr);
    const MaterialHandle a = MaterialLibrary::get().create(textured("A", "ids/a.png", "ids/shared_n.png"));
    const MaterialHandle b = MaterialLibrary::get().create(textured("B", "ids/b.png", "ids/shared_n.png"));
    const rhi::BindingSetHandle sa = ms.bindingSet(a), sb = ms.bindingSet(b);
    const auto* ta = ms.textures(sa);
    const auto* tb = ms.textures(sb);
    check(ta && tb, "both materials report their effective textures");

    check(ms.textureId(0) == kUnboundTexture, "a null handle is unbound");
    std::set<u32> ids;
    bool allBound = true;
    for (const auto* t : {ta, tb})
        for (const rhi::TextureHandle h : *t) {
            const u32 id = ms.textureId(h);
            allBound = allBound && id != kUnboundTexture && id < kMaterialTexIdCap;
            ids.insert(id);
        }
    check(allBound, "every effective handle, fallbacks included, gets an id inside the cap");
    std::set<rhi::TextureHandle> distinct;
    for (const auto* t : {ta, tb}) for (const rhi::TextureHandle h : *t) distinct.insert(h);
    check(ids.size() == distinct.size() && ms.textureIdsInUse() == distinct.size(), "one id per distinct texture");
    check(ms.textureId((*ta)[static_cast<u32>(TextureSlot::Normal)]) == ms.textureId((*tb)[static_cast<u32>(TextureSlot::Normal)]),
          "a texture two materials share has one id");
    check(ms.textureId((*ta)[0]) != ms.textureId((*tb)[0]), "different textures have different ids");

    const u32 before = ms.textureId((*ta)[0]);
    check(ms.textureId((*ta)[0]) == before, "asking again returns the same id");

    // A dirty re-upload of the material (its factor changes) rebinds the same textures: ids do not move.
    MaterialDesc edited = textured("A", "ids/a.png", "ids/shared_n.png");
    edited.roughnessFactor = 0.25f;
    MaterialLibrary::get().update(a, edited);
    ms.update();
    check(ms.textureId((*ms.textures(sa))[0]) == before, "a dirty re-upload keeps the id of a texture it still names");

    MaterialLibrary::get().destroy(a);
    MaterialLibrary::get().destroy(b);
    ms.update();
    ms.shutdown();
}

void testLifetime() {
    AVER_INFO("=== an id is held for two updates after its texture dies ===");
    MockDevice dev;
    g_dev = &dev;
    MaterialSystem ms;
    ms.init(dev, 0);
    ms.setTextureResolver(&resolve, nullptr);
    const MaterialHandle gone = MaterialLibrary::get().create(textured("Gone", "life/gone.png"));
    const rhi::BindingSetHandle s = ms.bindingSet(gone);
    const rhi::TextureHandle tex = (*ms.textures(s))[0];
    const u32 id = ms.textureId(tex);
    check(id != kUnboundTexture, "the texture has an id");
    const u32 inUse = ms.textureIdsInUse();

    MaterialLibrary::get().destroy(gone);
    ms.update();   // the texture is destroyed here
    check(dev.factory.destroyed.count(tex) == 1, "the texture was destroyed");
    check(ms.textureIdsInUse() == inUse - 1, "its id left the live set");

    // A new texture in the same frame must not receive the id yet.
    rhi::TextureDesc d;
    d.debugName = "life/new1";
    const rhi::TextureHandle n1 = dev.factory.createTexture(d);
    check(ms.textureId(n1) != id, "a texture asked for right after gets a different id");
    ms.update();
    d.debugName = "life/new2";
    const rhi::TextureHandle n2 = dev.factory.createTexture(d);
    check(ms.textureId(n2) != id, "...and one update later it is still held");
    ms.update();
    d.debugName = "life/new3";
    const rhi::TextureHandle n3 = dev.factory.createTexture(d);
    check(ms.textureId(n3) == id, "two updates after the destroy the id is reused");
    ms.shutdown();
}

void testCapAndShutdown() {
    AVER_INFO("=== the pool is capped and shutdown starts over ===");
    MockDevice dev;
    g_dev = &dev;
    MaterialSystem ms;
    ms.init(dev, 0);
    std::vector<rhi::TextureHandle> hs;
    u32 bound = 0;
    for (u32 i = 0; i < kMaterialTexIdCap + 10; ++i) {
        rhi::TextureDesc d;
        d.debugName = "cap";
        const rhi::TextureHandle h = dev.factory.createTexture(d);
        hs.push_back(h);
        if (ms.textureId(h) != kUnboundTexture) ++bound;
    }
    check(bound == kMaterialTexIdCap, "only kMaterialTexIdCap textures were given an id");
    check(ms.textureIdsInUse() == kMaterialTexIdCap, "exactly kMaterialTexIdCap ids are handed out");
    check(ms.textureId(hs.back()) == kUnboundTexture, "the texture past the cap is unbound");
    check(ms.textureId(hs.front()) == 0, "the first id is 0");
    ms.shutdown();
    check(ms.textureIdsInUse() == 0, "shutdown forgets every id");
    MockDevice dev2;
    g_dev = &dev2;
    check(ms.init(dev2, 0), "re-init");
    rhi::TextureDesc d;
    d.debugName = "after";
    check(ms.textureId(dev2.factory.createTexture(d)) == 0, "ids start again from 0");
    ms.shutdown();
}

} // namespace

int main() {
    testIds();
    testLifetime();
    testCapAndShutdown();
    if (g_failures) { AVER_ERROR("MaterialTextureIdTest: {} failure(s)", g_failures); return 1; }
    AVER_INFO("MaterialTextureIdTest: all passed");
    return 0;
}
