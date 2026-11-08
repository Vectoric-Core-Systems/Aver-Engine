// Cluster paging, CPU half: how a cluster DAG is cut into pages, and which pages should be resident
// for a view under a memory budget. No GPU, no streaming IO; the on-disk layout this plans for is
// described in docs/plans/CLUSTER_PAGES.md.
//
// A PAGE is a run of whole sibling sets (one simplification group's outputs, or at LOD 0 the members
// of one group), never mixing LOD levels, filled up to a byte budget. The invariant the planner keeps:
// a page is resident only if every page holding one of its clusters' parents is resident, so each
// resident cluster always has a coarser fallback to draw. Pages with no parent pages (the coarsest
// level) are ROOT pages and are always resident.
#pragma once

#include "aver/core/Types.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/trifactor/ClusterSelect.hpp"

#include <string>
#include <vector>

namespace aver::trifactor {

inline constexpr u32 kDefaultPageBytes = 128u * 1024u;

// Bytes one cluster occupies in a page: fixed record (descriptor + bounds + topology links) plus
// 4 B per vertex index and 1 B per triangle index byte.
u32 clusterRecordBytes(const fmt::OcMeshMeshlet& m);

// clusterRecordBytes for every cluster of `mesh`, in the order buildClusterDagViews numbers them.
std::vector<u32> clusterRecordSizes(const fmt::OcMeshData& mesh);

struct ClusterPage {
    u32 id = 0;
    u32 level = 0;
    u32 firstSlot = 0;     // range into PagePlan::pageClusters
    u32 slotCount = 0;
    u32 bytes = 0;
    bool oversize = false; // one sibling set alone exceeds the budget
    std::vector<u32> parentPages;   // sorted, unique; empty => root page
    bool isRoot() const { return parentPages.empty(); }
};

struct PagePlan {
    u32 budgetBytes = 0;
    std::vector<ClusterPage> pages;
    std::vector<u32> pageClusters;    // cluster ids, grouped by page
    std::vector<u32> pageOfCluster;   // cluster id -> page id
    u64 totalBytes = 0;
    u64 rootBytes = 0;
};

// `sizes` is per cluster (clusterRecordSizes). Fails on a size/array mismatch or an empty DAG.
bool buildPagePlan(const std::vector<ClusterView>& clusters, const std::vector<u32>& sizes, u32 pageBytes,
                   PagePlan& out, std::string* why = nullptr);

// Structural check: every cluster in exactly one page, pages single-level, parent pages strictly
// coarser and consistent with the clusters' parents, sizes add up, budget respected unless oversize.
bool validatePagePlan(const PagePlan& plan, const std::vector<ClusterView>& clusters,
                      const std::vector<u32>& sizes, std::string* why = nullptr);

// Projected-error demand per page: for each cluster the cut at `thresholdPx` draws, the on-screen
// error (px) the view would show if its page were missing and the parents had to stand in; a page's
// demand is the maximum over its cut clusters, and 0 when the cut does not touch it.
std::vector<f32> computePageDemand(const PagePlan& plan, const std::vector<ClusterView>& clusters,
                                   f32 thresholdPx, const View& view);

struct ResidencyPlan {
    std::vector<u8> resident;     // per page
    std::vector<u32> load;        // pages to bring in
    std::vector<u32> evict;       // pages to drop
    u64 residentBytes = 0;
    bool rootsFit = true;         // false when the budget cannot even hold the root pages
};

// Chooses the resident set under `budgetBytes`: root pages first (always), then pages by descending
// demand, each with the ancestors it needs, while they fit. A page already resident gets
// `keepBias` x its demand so a near tie does not thrash. `current` may be empty (nothing resident).
ResidencyPlan planResidency(const PagePlan& plan, const std::vector<f32>& demand, u64 budgetBytes,
                            const std::vector<u8>& current, f32 keepBias = 1.25f);

} // namespace aver::trifactor
