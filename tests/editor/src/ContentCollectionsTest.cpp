// Content Browser type chips, favourites and collections: classification, filtering, text round trip.
#include "../../../sandbox/src/ContentCollections.hpp"

#include "aver/core/Log.hpp"

#include <string>

using namespace aver;
using namespace aver::editor;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

int main() {
    check(cbTypeForExt(".ocmesh") == CbType::Mesh && cbTypeForExt(".png") == CbType::Texture, "mesh and texture extensions");
    check(cbTypeForExt(".ocmat") == CbType::Material && cbTypeForExt(".wav") == CbType::Audio, "material and audio");
    check(cbTypeForExt(".ocworld") == CbType::Level && cbTypeForExt(".cs") == CbType::Script, "level and code");
    check(cbTypeForExt(".zzz") == CbType::Other && cbTypeForExt("") == CbType::Other, "unknown is Other");

    u32 m = 0;
    check(!cbTypeFiltering(m) && cbTypePasses(m, false, ".png"), "no chip lit passes everything");
    m = cbTypeToggled(m, CbType::Texture);
    check(cbTypeFiltering(m) && cbTypePasses(m, false, ".png") && !cbTypePasses(m, false, ".ocmesh"), "one chip lit filters to it");
    check(cbTypePasses(m, true, ""), "folders always pass");
    m = cbTypeToggled(m, CbType::Mesh);
    check(cbTypePasses(m, false, ".ocmesh") && cbTypePasses(m, false, ".png"), "two chips are an OR");
    check(!cbTypeFiltering(kCbAllTypes), "all chips lit is no filter");

    check(cbContentKey("C:\\P\\Content\\Meshes\\a.ocmesh", "C:/P/Content") == "Meshes/a.ocmesh", "content key, slashes normalised");
    check(cbContentKey("C:/P/Other/a", "C:/P/Content").empty(), "outside the content dir has no key");
    check(cbContentKey("C:/P/ContentX/a", "C:/P/Content").empty(), "a sibling prefix is not inside");

    CbCollections c;
    check(c.toggleFavourite("a/b.png") && c.isFavourite("a/b.png"), "favourite on");
    check(!c.toggleFavourite("a/b.png") && !c.isFavourite("a/b.png"), "favourite off");
    check(c.create("Props") && !c.create("Props") && !c.create("") && !c.create("a\nb"), "collection names: unique, non-empty, one line");
    check(c.add("Props", "m/x.ocmesh") && !c.add("Props", "m/x.ocmesh") && !c.add("Nope", "q"), "add once, to an existing collection");
    c.toggleFavourite("m/x.ocmesh");
    c.rebase("m", "models");
    check(c.find("Props")->paths[0] == "models/x.ocmesh" && c.favourites[0] == "models/x.ocmesh", "a moved folder carries references");
    c.rebase("models/x.ocmesh", "models/y.ocmesh");
    check(c.find("Props")->paths[0] == "models/y.ocmesh", "a renamed file carries its references");

    const CbCollections r = cbParse(cbSerialize(c));
    check(r.favourites == c.favourites && r.collections.size() == 1 && r.collections[0].name == "Props" &&
          r.collections[0].paths == c.collections[0].paths, "text round trip");
    const CbCollections junk = cbParse("garbage\nfav\nitem x\ncol A\nitem p\nitem p\n\r\nfav z \n");
    check(junk.collections.size() == 1 && junk.collections[0].paths.size() == 1 && junk.favourites.size() == 1,
          "malformed lines are skipped, duplicates dropped");
    check(c.removeFrom("Props", "models/y.ocmesh") && c.remove("Props") && c.collections.empty(), "remove item and collection");

    if (g_failures == 0) AVER_INFO("=== all ContentCollections checks passed ===");
    else                 AVER_ERROR("=== {} ContentCollections check(s) FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
