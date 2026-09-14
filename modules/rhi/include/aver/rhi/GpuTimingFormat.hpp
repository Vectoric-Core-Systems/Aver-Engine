// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
//
// Renders a GpuTimingReport as an indented, human-readable tree.
//
// WHY IT LIVES HERE RATHER THAN IN THE EDITOR. This walk started as the body of the editor console's
// `frametime` command, in sandbox/src/EditorConsole.hpp -- which the packaged game cannot include,
// and which is the only reason a shipped AverGame.exe had no way to show timings it was already
// paying to collect. D3D12Device::initGpuTiming runs unconditionally, not behind a flag, so every
// build of every host has been measuring per-pass GPU time all along and only the editor could look
// at it. Moving the formatter beside the report it formats is what makes one line in GameApp enough.
//
// EMIT-PER-LINE, not a returned string: the editor routes each line to its console AND the log with
// its own level, the game writes each to the log, and a future overlay would draw each as a row. A
// single blob would force all three to re-split it.
//
// Header-only and dependency-free beyond RHI.hpp itself, so it costs the game module nothing to
// include and cannot pull the editor's world into a shipped binary.
#pragma once

#include "aver/rhi/RHI.hpp"

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace aver::rhi {

// One output line. Returned by value because every caller either logs or draws it immediately.
using GpuTimingLineFn = std::function<void(const std::string& line)>;

// Walks `r`'s flat, parent-indexed node list as an indented tree: inclusive ms (what the node itself
// reported), exclusive ms (inclusive minus the sum of its direct children, i.e. the pass's own
// time), and percent of the GPU total.
//
// Returns the GPU total -- the sum of TOP-LEVEL marked spans only, mirroring collectGpuTiming's own
// topLevelMs, which excludes anything unmarked. A caller that wants the CPU-bound/GPU-bound
// comparison prints that itself, because the CPU side of it belongs to the caller's frame loop and
// not to this report.
//
// THE TWO NO-DATA CASES ARE DIFFERENT SENTENCES, never zeros. "This backend cannot do timestamps" and
// "it can, but nothing has been collected yet" lead to different next actions, and a 0.00ms that
// could be mistaken for a measurement is the worst possible way to say either.
inline f64 formatGpuTiming(const GpuTimingReport& r, const GpuTimingLineFn& emit) {
    if (!r.supported) {
        emit("This backend cannot report per-pass GPU timings (unsupported here, not just empty).");
        return 0.0;
    }
    if (r.nodes.empty() || r.framesAccumulated == 0) {
        emit("GPU timing is supported but no data has been collected yet -- ask again in a moment.");
        return 0.0;
    }

    std::vector<std::vector<u32>> children(r.nodes.size());
    std::vector<u32> topLevel;
    for (u32 i = 0; i < r.nodes.size(); ++i) {
        if (r.nodes[i].parent == GpuTimingNode::kNoParent) topLevel.push_back(i);
        else                                               children[r.nodes[i].parent].push_back(i);
    }
    f64 gpuTotalMs = 0.0;
    for (u32 i : topLevel) gpuTotalMs += r.nodes[i].ms;

    char hdr[192];
    std::snprintf(hdr, sizeof hdr,
                  "GPU per-pass breakdown, averaged over %u frames and a couple of frames old (see "
                  "IDevice::gpuTiming's own comment) -- not a live number:",
                  static_cast<unsigned>(r.framesAccumulated));
    emit(hdr);

    // Recursive local functor, the same shape as D3D12Device.cpp's own Appender in collectGpuTiming:
    // a hand-rolled struct whose operator() calls itself is the smallest thing that prints an
    // indented tree without adding a dependency for one call site.
    struct Appender {
        const GpuTimingLineFn& emit;
        const std::vector<std::vector<u32>>& children;
        const std::vector<GpuTimingNode>& nodes;
        f64 total;
        void operator()(u32 idx, u32 depth) const {
            const GpuTimingNode& n = nodes[idx];
            f64 childMs = 0.0;
            for (u32 c : children[idx]) childMs += nodes[c].ms;
            const f64 pct = total > 0.0 ? (n.ms / total * 100.0) : 0.0;
            char buf[192];
            std::snprintf(buf, sizeof buf, "%*s%s  %.2fms incl / %.2fms excl  (%.1f%% of GPU total)",
                          static_cast<int>(depth) * 2 + 2, "", n.label.c_str(), n.ms, n.ms - childMs, pct);
            emit(buf);
            for (u32 c : children[idx]) (*this)(c, depth + 1);
        }
    };
    const Appender append{emit, children, r.nodes, gpuTotalMs};
    for (u32 i : topLevel) append(i, 0);

    return gpuTotalMs;
}

} // namespace aver::rhi
