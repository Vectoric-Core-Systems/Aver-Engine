// Renders a CpuTimingReport as an indented, human-readable tree -- the CPU-side twin of
// rhi::formatGpuTiming (see that file's own comment for why a formatter lives beside the report it
// formats rather than inside whichever panel or console command first needed one): the editor's scene
// walk display and a console `walktime` command both want the identical text, and a single blob
// returned by value would force both to re-split it, so this emits one line at a time exactly like
// formatGpuTiming does.
//
// Header-only and dependency-free beyond CpuTiming.hpp itself -- same reasoning as
// GpuTimingFormat.hpp's own comment on RHI.hpp: this costs a shipped game module nothing extra to
// include, and cannot drag anything editor-only into it.
#pragma once

#include "aver/core/CpuTiming.hpp"

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace aver {

// One output line. Returned by value because every caller either logs, draws, or writes it straight
// to a console immediately -- see rhi::GpuTimingLineFn's own comment.
using CpuTimingLineFn = std::function<void(const std::string& line)>;

// Walks `r`'s flat, parent-indexed node list as an indented tree rooted at SceneWalk: inclusive ms,
// exclusive ms (inclusive minus direct children, the bucket's OWN time), percent of the walk total,
// and the call count -- the number that turns "3ms" into "3ms over 96,000 lookups" (see
// CpuTimingNode::calls's own comment).
//
// Returns SceneWalk's own ms -- the walk total -- mirroring formatGpuTiming's gpuTotalMs. TimingOverhead
// is deliberately EXCLUDED from this recursive walk and from the percentage-of-total math: it is not
// additional wall-clock time spent somewhere else, it is an ESTIMATE of how much of SceneWalk's own
// number, already printed inside the tree above, is the instrument rather than the engine. Folding it
// into the same tree as a sibling of SceneWalk would silently double it -- once inside whichever WalkX
// bucket happened to be open when a boundary was actually read, a second time as its own row competing
// for a share of the same total. It is reported instead as its own sentence after the tree, which is
// also exactly the shape CpuTiming.hpp's own comment on SELF-MEASUREMENT asks for: "of which ~Xms is
// this instrument".
//
// THE TWO NO-DATA CASES ARE DIFFERENT SENTENCES, never a bare 0.00ms that could be mistaken for a
// measurement -- see CpuTimingReport's own comment on why `supported` and an empty `nodes` are kept
// apart instead of collapsed into one flag.
inline f64 formatCpuTiming(const CpuTimingReport& r, const CpuTimingLineFn& emit) {
    if (!r.supported) {
        emit("This build cannot report low-overhead CPU timings (compiled out, or this process is on "
             "the steady_clock fallback rather than the calibrated TSC path -- see "
             "AVER_CPU_TIMING_HAS_RDTSC's own comment on why that path does not publish numbers at "
             "all rather than publish inflated ones).");
        return 0.0;
    }
    if (r.nodes.empty() || r.framesAccumulated == 0) {
        emit("CPU timing is supported but no reporting window has completed yet -- ask again shortly.");
        return 0.0;
    }

    const usize sceneWalkIdx = static_cast<usize>(CpuSpan::SceneWalk);
    const usize overheadIdx = static_cast<usize>(CpuSpan::TimingOverhead);

    // Direct-children index, built fresh per call exactly like formatGpuTiming's own -- this is not a
    // hot-path function (it runs once per log line or console command, never once per boundary), so
    // an O(nodes) pass here is free next to the cost of formatting and emitting the strings it feeds.
    // TimingOverhead never lands in any bucket here: its own parent is kNoParent (see kCpuSpanParent's
    // table), so it is top-level exactly like SceneWalk is, and the recursive walk below only ever
    // visits the ONE root it is explicitly given -- SceneWalk. TimingOverhead is read directly, by
    // index, afterwards instead of being discovered by this walk.
    std::vector<std::vector<u32>> children(r.nodes.size());
    for (u32 i = 0; i < r.nodes.size(); ++i) {
        if (r.nodes[i].parent != CpuTimingNode::kNoParent) children[r.nodes[i].parent].push_back(i);
    }

    const f64 walkTotalMs = r.nodes[sceneWalkIdx].ms;

    char hdr[224];
    std::snprintf(hdr, sizeof hdr,
                  "CPU scene-walk breakdown, averaged over %u occurrences of the walk (a window, not "
                  "a single frame -- see CpuTimingReport::framesAccumulated's own comment):",
                  static_cast<unsigned>(r.framesAccumulated));
    emit(hdr);

    // Recursive local functor, the same shape as rhi::formatGpuTiming's own Appender: a hand-rolled
    // struct whose operator() calls itself, the smallest thing that prints an indented tree without
    // reaching for a second dependency at this one call site.
    struct Appender {
        const CpuTimingLineFn& emit;
        const std::vector<std::vector<u32>>& children;
        const std::vector<CpuTimingNode>& nodes;
        f64 total;
        void operator()(u32 idx, u32 depth) const {
            const CpuTimingNode& n = nodes[idx];
            f64 childMs = 0.0;
            for (u32 c : children[idx]) childMs += nodes[c].ms;
            const f64 pct = total > 0.0 ? (n.ms / total * 100.0) : 0.0;
            char buf[224];
            std::snprintf(buf, sizeof buf,
                          "%*s%s  %.3fms incl / %.3fms excl  (%.1f%% of walk)  over %llu calls",
                          static_cast<int>(depth) * 2 + 2, "", n.label, n.ms, n.ms - childMs, pct,
                          static_cast<unsigned long long>(n.calls));
            emit(buf);
            for (u32 c : children[idx]) (*this)(c, depth + 1);
        }
    };
    const Appender append{emit, children, r.nodes, walkTotalMs};
    append(static_cast<u32>(sceneWalkIdx), 0);

    // SELF-MEASUREMENT, stated last and separately -- a profiler that cannot say how much of its own
    // number is itself cannot be used to make a percent-level call, and this instrument's whole
    // reason for existing is to make one safely. This time is already INSIDE the tree printed above
    // (baked into whichever WalkX bucket happened to be open when each boundary was actually read),
    // not additional to it -- see this function's own comment on why TimingOverhead is not a sibling
    // row in that tree.
    const CpuTimingNode& overhead = r.nodes[overheadIdx];
    char ohbuf[224];
    std::snprintf(ohbuf, sizeof ohbuf,
                  "  of which an estimated %.3fms (%llu boundary crossings) is this instrument timing "
                  "itself -- already included above, not additional time.",
                  overhead.ms, static_cast<unsigned long long>(overhead.calls));
    emit(ohbuf);

    return walkTotalMs;
}

} // namespace aver
