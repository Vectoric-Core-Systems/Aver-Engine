// SceneSiblingOrderTest: World::setSiblingOrder / setOrderStamp, the scene half of reordering the Outliner's siblings.
// Siblings sort by orderStamp; a reorder redistributes the stamps they hold, leaves other entities alone, and can be undone by
// restoring recorded stamps. No GPU. Exit code = failure count.
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/scene/World.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::scene;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

// The children of `p` (all roots when p is invalid) sorted by orderStamp: the order the Outliner shows.
static std::vector<Entity> siblingsOf(World& w, Entity p, const std::vector<Entity>& pool) {
    std::vector<Entity> v;
    for (const Entity e : pool) if (w.parent(e) == p) v.push_back(e);
    std::sort(v.begin(), v.end(), [&](Entity a, Entity b) { return w.orderStamp(a) < w.orderStamp(b); });
    return v;
}

int main() {
    AVER_INFO("SceneSiblingOrderTest");
    World& w = World::instance();
    const Transform xf;
    const Entity p  = w.create("p", kInvalidEntity, xf);
    const Entity a  = w.create("a", p, xf);
    const Entity b  = w.create("b", p, xf);
    const Entity c  = w.create("c", p, xf);
    const Entity d  = w.create("d", p, xf);
    const Entity o  = w.create("other", kInvalidEntity, xf);
    w.flush();
    const std::vector<Entity> pool{p, a, b, c, d, o};

    check((siblingsOf(w, p, pool) == std::vector<Entity>{a, b, c, d}), "children sort in creation order");
    const u64 oBefore = w.orderStamp(o), pBefore = w.orderStamp(p);
    const std::vector<u64> before{w.orderStamp(a), w.orderStamp(b), w.orderStamp(c), w.orderStamp(d)};

    const u64 rev0 = w.hierarchyRevision();
    check(w.setSiblingOrder({c, a, d, b}), "a valid re-sequence is accepted");
    check((siblingsOf(w, p, pool) == std::vector<Entity>{c, a, d, b}), "the siblings now sort as listed");
    check(w.hierarchyRevision() != rev0, "and the hierarchy revision moved so caches rebuild");
    std::vector<u64> now{w.orderStamp(a), w.orderStamp(b), w.orderStamp(c), w.orderStamp(d)};
    std::sort(now.begin(), now.end());
    check(now == before, "the same stamp values were redistributed (nothing new was minted)");
    check(w.orderStamp(o) == oBefore && w.orderStamp(p) == pBefore, "entities outside the set keep their stamp");

    // A subset keeps its slots among the rest: b now sits before d.
    check(w.setSiblingOrder({b, d}), "a subset can be re-sequenced");
    check((siblingsOf(w, p, pool) == std::vector<Entity>{c, a, b, d}), "the subset trades places, the others stay put");

    // Undo by restoring recorded stamps.
    const std::vector<u64> snap{w.orderStamp(a), w.orderStamp(b), w.orderStamp(c), w.orderStamp(d)};
    check(w.setSiblingOrder({a, b, c, d}), "re-sequence again");
    w.setOrderStamp(a, snap[0]); w.setOrderStamp(b, snap[1]); w.setOrderStamp(c, snap[2]); w.setOrderStamp(d, snap[3]);
    check((siblingsOf(w, p, pool) == std::vector<Entity>{c, a, b, d}), "restoring the recorded stamps brings the old order back");

    // Refusals leave everything alone.
    const std::vector<Entity> keep = siblingsOf(w, p, pool);
    check(!w.setSiblingOrder({a, o}), "mixed parents are refused");
    check(!w.setSiblingOrder({a, a}), "a duplicate is refused");
    check((siblingsOf(w, p, pool) == keep), "a refusal changes nothing");

    // A fresh child still lands after every restored stamp.
    const Entity e = w.create("e", p, xf);
    w.flush();
    check(siblingsOf(w, p, pool).size() == 4 && w.orderStamp(e) > w.orderStamp(d) && w.orderStamp(e) > w.orderStamp(a), "a new child sorts after the restored ones");

    AVER_INFO("{} checks, {} failed", g_checks, g_failures);
    return g_failures;
}
