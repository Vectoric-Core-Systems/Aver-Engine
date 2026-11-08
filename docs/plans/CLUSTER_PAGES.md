# Cluster paging: page format and CPU residency planner

Status 2026-11-09: CPU half only (`modules/trifactor/include/aver/trifactor/ClusterPages.hpp`, test `ClusterPagesTest`). Nothing reads or writes pages on disk yet and there is no GPU side; both are wave 3. This file is the design the planner was written against.

## Why

`.ocmesh` keeps every LOD level's meshlets resident (finding 8 in `WAVE2_UE.md`), so a mesh cannot exceed memory and nothing streams. Paging splits the cluster DAG into units that can be loaded and dropped independently.

## Page

A page is a run of whole sibling sets at one LOD level, filled to a byte budget (default 128 KiB, `kDefaultPageBytes`):

- Sibling set at LOD 0: the clusters one simplification group merges. At LOD >= 1: the output clusters of one group (`ClusterGroupNode::ownClusterRange`). Sets are never split across pages, so a group's children are always reachable from one parent set; a single set larger than the budget gets its own page (`oversize`).
- A page never mixes LOD levels.
- `parentPages` = the pages holding the parents of any cluster in the page. Pages with none are ROOT pages (the coarsest level).

### Residency rule

A page may be resident only if all its `parentPages` are resident. Every resident cluster therefore has its fallback ancestor (`fallbackAncestorId`) in memory, and the cut can always fall back to a coarser cluster instead of opening a hole. Root pages are always resident.

## Planner

`computePageDemand` runs the existing cut oracle (`selectCutIds`) and gives each page the largest on-screen error (px) the view would show if that page were missing and the parents stood in. `planResidency` takes root pages first, then pages by descending demand, each with the ancestors it still needs, while they fit the budget. Pages already resident get a bias (default 1.25) so near-ties do not thrash. Output is the resident set plus load and evict lists. If the budget cannot hold the roots they stay resident and `rootsFit` is false.

Not kept: resident pages with zero demand. A caller that wants slack against camera jitter should pass a larger budget or the previous frame's demand maxed with this one.

## On-disk layout (planned, not written)

`.ocmesh` v-next stores, after the always-resident header, vertex streams and root pages:

```
PageDirectory   u32 pageCount; per page { u32 level, u32 byteOffset, u32 byteSize, u32 clusterCount, u32 parentPageStart, u16 parentPageCount, u16 flags }
ParentPageList  u32[]  (indexed by the directory)
Page payload    per cluster: MeshletDesc (existing MLET v4 record), ClusterBounds, ownError/parentError, materialSlot, fallbackAncestorId (page-local + page index), then vertex index list (u32) and triangle bytes (u8)
```

Rules: the directory and root pages sit in the first read; payloads are independently seekable; a reader that does not know the paged section ignores it and uses the existing resident meshlets (absent-vs-zero rule, `FORMAT_SPECS.md` 3.2). Sizes in the planner (`clusterRecordBytes`: 96 B fixed + 4 B per vertex index + 1 B per triangle byte) are the estimate this layout is expected to match; the writer must check them.

## Not done

- Writer/reader for the layout above, DDC key, and a version bump.
- Async IO, GPU upload, GPU residency feedback (a buffer of pages the cull pass wants), page-table indirection in the cull kernels.
- Demand from many views or shadow cascades (the planner takes one demand vector; summing per view is the intended extension).
