#include "aver/trifactor/ClusterPages.hpp"

#include <algorithm>
#include <limits>
#include <numeric>

namespace aver::trifactor {

namespace {
// descriptor (counts, offsets) + ClusterBounds + errors + material slot + fallback/owner ids
constexpr u32 kClusterFixedBytes = 96;
}

u32 clusterRecordBytes(const fmt::OcMeshMeshlet& m) {
    return kClusterFixedBytes + static_cast<u32>(m.vertices.size()) * 4u + static_cast<u32>(m.triangles.size());
}

std::vector<u32> clusterRecordSizes(const fmt::OcMeshData& mesh) {
    std::vector<u32> out;
    for (const fmt::OcMeshMeshlet& m : mesh.meshlets) out.push_back(clusterRecordBytes(m));
    for (const fmt::OcMeshLod& lod : mesh.coarserLods)
        for (const fmt::OcMeshMeshlet& m : lod.meshlets) out.push_back(clusterRecordBytes(m));
    return out;
}

bool buildPagePlan(const std::vector<ClusterView>& clusters, const std::vector<u32>& sizes, u32 pageBytes,
                   PagePlan& out, std::string* why) {
    auto fail = [&](const char* m) { if (why) *why = m; return false; };
    if (clusters.empty()) return fail("empty cluster DAG");
    if (sizes.size() != clusters.size()) return fail("size table does not match the cluster count");
    if (pageBytes == 0) return fail("zero page budget");

    PagePlan plan;
    plan.budgetBytes = pageBytes;
    const u32 n = static_cast<u32>(clusters.size());
    u32 levels = 0;
    for (const ClusterView& c : clusters) levels = std::max(levels, c.level + 1);

    // Sibling sets, keyed so that one group's members (LOD 0) or outputs (coarser) land together.
    constexpr u32 kNone = 0xFFFFFFFFu;
    auto keyOf = [&](const ClusterView& c) -> u32 {
        const std::vector<u32>& link = c.level == 0 ? c.parents : c.children;
        return link.empty() ? kNone : *std::min_element(link.begin(), link.end());
    };

    plan.pageOfCluster.assign(n, kNone);
    for (u32 L = 0; L < levels; ++L) {
        std::vector<u32> ids;
        for (u32 i = 0; i < n; ++i) if (clusters[i].level == L) ids.push_back(i);
        std::stable_sort(ids.begin(), ids.end(), [&](u32 a, u32 b) {
            const u32 ka = keyOf(clusters[a]), kb = keyOf(clusters[b]);
            return ka != kb ? ka < kb : a < b;
        });

        ClusterPage cur;
        auto open = [&] {
            cur = ClusterPage{};
            cur.id = static_cast<u32>(plan.pages.size());
            cur.level = L;
            cur.firstSlot = static_cast<u32>(plan.pageClusters.size());
        };
        auto close = [&] {
            if (cur.slotCount == 0) return;
            plan.pages.push_back(cur);
            open();
        };
        open();
        for (usize i = 0; i < ids.size();) {
            usize j = i + 1;
            const u32 key = keyOf(clusters[ids[i]]);
            // A cluster with no sibling key is its own unit.
            while (key != kNone && j < ids.size() && keyOf(clusters[ids[j]]) == key) ++j;
            u64 unit = 0;
            for (usize k = i; k < j; ++k) unit += sizes[ids[k]];
            if (cur.slotCount > 0 && cur.bytes + unit > pageBytes) close();
            for (usize k = i; k < j; ++k) {
                plan.pageClusters.push_back(ids[k]);
                plan.pageOfCluster[ids[k]] = cur.id;
            }
            cur.slotCount += static_cast<u32>(j - i);
            cur.bytes += static_cast<u32>(unit);
            if (cur.bytes > pageBytes) cur.oversize = true;
            i = j;
        }
        close();
    }

    for (ClusterPage& p : plan.pages) {
        for (u32 s = 0; s < p.slotCount; ++s)
            for (u32 par : clusters[plan.pageClusters[p.firstSlot + s]].parents)
                p.parentPages.push_back(plan.pageOfCluster[par]);
        std::sort(p.parentPages.begin(), p.parentPages.end());
        p.parentPages.erase(std::unique(p.parentPages.begin(), p.parentPages.end()), p.parentPages.end());
        plan.totalBytes += p.bytes;
        if (p.isRoot()) plan.rootBytes += p.bytes;
    }
    out = std::move(plan);
    return true;
}

bool validatePagePlan(const PagePlan& plan, const std::vector<ClusterView>& clusters,
                      const std::vector<u32>& sizes, std::string* why) {
    auto fail = [&](const std::string& m) { if (why) *why = m; return false; };
    const u32 n = static_cast<u32>(clusters.size());
    if (plan.pageOfCluster.size() != n || plan.pageClusters.size() != n) return fail("cluster coverage size mismatch");
    std::vector<u8> seen(n, 0);
    u64 total = 0;
    for (const ClusterPage& p : plan.pages) {
        if (p.id >= plan.pages.size() || &plan.pages[p.id] != &p) return fail("page id is not its index");
        if (p.slotCount == 0) return fail("empty page");
        u64 bytes = 0;
        for (u32 s = 0; s < p.slotCount; ++s) {
            const u32 c = plan.pageClusters[p.firstSlot + s];
            if (c >= n || seen[c]) return fail("cluster listed twice or out of range");
            seen[c] = 1;
            if (plan.pageOfCluster[c] != p.id) return fail("pageOfCluster disagrees with the page list");
            if (clusters[c].level != p.level) return fail("page mixes LOD levels");
            bytes += sizes[c];
        }
        if (bytes != p.bytes) return fail("page byte count wrong");
        if (bytes > plan.budgetBytes && !p.oversize) return fail("page over budget but not marked oversize");
        total += bytes;
        std::vector<u32> expect;
        for (u32 s = 0; s < p.slotCount; ++s)
            for (u32 par : clusters[plan.pageClusters[p.firstSlot + s]].parents) expect.push_back(plan.pageOfCluster[par]);
        std::sort(expect.begin(), expect.end());
        expect.erase(std::unique(expect.begin(), expect.end()), expect.end());
        if (expect != p.parentPages) return fail("parentPages disagree with the clusters' parents");
        for (u32 pp : p.parentPages)
            if (plan.pages[pp].level <= p.level) return fail("parent page is not coarser");
    }
    for (u32 c = 0; c < n; ++c) if (!seen[c]) return fail("cluster missing from every page");
    if (total != plan.totalBytes) return fail("total bytes wrong");
    return true;
}

std::vector<f32> computePageDemand(const PagePlan& plan, const std::vector<ClusterView>& clusters,
                                   f32 thresholdPx, const View& view) {
    std::vector<f32> demand(plan.pages.size(), 0.0f);
    for (u32 id : selectCutIds(clusters, thresholdPx, view)) {
        f32 fallbackPx = 0.0f;
        for (u32 p : clusters[id].parents) {
            Vec3 c; f32 r;
            lodSphereOf(clusters[p], c, r);
            fallbackPx = std::max(fallbackPx, screenSpaceErrorPx(c, r, clusters[p].error, view));
        }
        f32& d = demand[plan.pageOfCluster[id]];
        d = std::max(d, fallbackPx);
    }
    return demand;
}

ResidencyPlan planResidency(const PagePlan& plan, const std::vector<f32>& demand, u64 budgetBytes,
                            const std::vector<u8>& current, f32 keepBias) {
    const u32 n = static_cast<u32>(plan.pages.size());
    ResidencyPlan out;
    out.resident.assign(n, 0);
    auto wasResident = [&](u32 p) { return p < current.size() && current[p]; };

    for (const ClusterPage& p : plan.pages)
        if (p.isRoot()) { out.resident[p.id] = 1; out.residentBytes += p.bytes; }
    out.rootsFit = out.residentBytes <= budgetBytes;

    std::vector<u32> order;
    std::vector<f32> key(n, 0.0f);
    for (u32 p = 0; p < n; ++p) {
        if (out.resident[p] || p >= demand.size() || !(demand[p] > 0.0f)) continue;
        key[p] = demand[p] * (wasResident(p) ? keepBias : 1.0f);
        order.push_back(p);
    }
    std::stable_sort(order.begin(), order.end(), [&](u32 a, u32 b) {
        if (key[a] != key[b]) return key[a] > key[b];
        return plan.pages[a].level > plan.pages[b].level;   // coarser first on a tie
    });

    std::vector<u32> need, stack;
    std::vector<u32> mark(n, 0);
    u32 stamp = 0;
    for (u32 p : order) {
        if (out.resident[p]) continue;
        ++stamp;
        need.clear();
        stack.assign(1, p);
        mark[p] = stamp;
        u64 cost = 0;
        while (!stack.empty()) {
            const u32 q = stack.back();
            stack.pop_back();
            need.push_back(q);
            cost += plan.pages[q].bytes;
            for (u32 par : plan.pages[q].parentPages)
                if (!out.resident[par] && mark[par] != stamp) { mark[par] = stamp; stack.push_back(par); }
        }
        if (out.residentBytes + cost > budgetBytes) continue;
        for (u32 q : need) out.resident[q] = 1;
        out.residentBytes += cost;
    }

    for (u32 p = 0; p < n; ++p) {
        if (out.resident[p] && !wasResident(p)) out.load.push_back(p);
        if (!out.resident[p] && wasResident(p)) out.evict.push_back(p);
    }
    return out;
}

} // namespace aver::trifactor
