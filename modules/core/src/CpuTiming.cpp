// CpuTiming's cold path: the one-time TSC calibration and self-measurement, and the report builder.
// Deliberately kept out of CpuTiming.hpp -- neither runs anywhere near the ~100,000-boundary-a-frame
// hot path that header exists to keep cheap, so neither has any business bloating a header three
// unrelated modules (Runtime/, render.voxi, sandbox) include for the sake of a handful of inline
// tick reads. See CpuTiming.hpp's own comments for the design this file only implements.
#include "aver/core/CpuTiming.hpp"

namespace aver {
namespace detail {

// Runs exactly once per process, however many times cpuOnLapOpen() calls it: a function-local static
// initialiser is guaranteed by the standard to run its initialiser exactly once even if entered
// concurrently, which costs nothing extra here (this facility is single-threaded by design -- see
// CpuTiming.hpp's own comment on cpuActiveSpan) but is the idiomatic, safest way to spell "once" all
// the same, and reads more plainly than a hand-rolled bool guard would.
void cpuEnsureCalibrated() {
    static const bool done = [] {
#if AVER_CPU_TIMING_HAS_RDTSC
        // Calibrates cpuNsPerTick: TSC ticks are meaningless on their own, so every conversion to a
        // millisecond a report ever prints depends on this ratio being right. Measured against
        // steady_clock -- the same clock the rest of the engine already trusts for wall time -- over
        // a short BUSY-WAIT window, not a Sleep(): Windows' scheduler quantum (~15ms) is longer than
        // the whole window this is trying to measure, so sleeping for it would turn "a short
        // calibration" into the very multi-millisecond stall this function exists to be the only one
        // of. Modern x86 TSC is invariant (see CpuTiming.hpp's own comment on cpuReadTicks), which is
        // what makes a SINGLE calibration like this one valid for the rest of the process instead of
        // needing to be repeated every time the core's clock speed changes.
        constexpr auto kCalibrationWindow = std::chrono::microseconds(2000);
        const auto steadyStart = std::chrono::steady_clock::now();
        const u64 tscStart = __rdtsc();
        while (std::chrono::steady_clock::now() - steadyStart < kCalibrationWindow) {
            // Busy-spin on purpose -- see the comment above.
        }
        const u64 tscEnd = __rdtsc();
        const auto steadyEnd = std::chrono::steady_clock::now();
        const f64 steadyNs = std::chrono::duration<f64, std::nano>(steadyEnd - steadyStart).count();
        const u64 tscDelta = tscEnd - tscStart;
        cpuNsPerTick = tscDelta > 0 ? steadyNs / static_cast<f64>(tscDelta) : 0.0;
#else
        // The fallback IS steady_clock, already in nanoseconds -- see cpuReadTicks(). No conversion,
        // and no calibration loop needed for one, but CpuTimingReport::supported still says so
        // honestly (see collectCpuTiming below): this path exists so the engine compiles and this
        // instrument still LOGS something on a target this header has not been proven correct
        // against, not so it can quietly claim the fast path's trustworthiness.
        cpuNsPerTick = 1.0;
#endif

        // Calibrates cpuNsPerTransitionOverhead: SELF-MEASUREMENT IS MANDATORY here, not optional --
        // a profiler that cannot state its own tax cannot be trusted to make the percent-level call
        // this whole stage exists for, and this codebase has already had to retract measurement
        // conclusions before. This loop runs the exact sequence a real CpuLap::to() or CpuNest
        // boundary performs -- read a tick, fold a delta into one CpuSpanAccum's ticks and calls --
        // against a THROWAWAY accumulator, so the measurement's own cost never pollutes a real span's
        // numbers with itself. kOverheadSamples is large enough that steady_clock's own read cost and
        // resolution (tens of nanoseconds) are a rounding error against the total elapsed time rather
        // than a meaningful fraction of it.
        constexpr u64 kOverheadSamples = 100'000;
        CpuSpanAccum scratch{};
        u64 tick = cpuReadTicks();
        const auto overheadStart = std::chrono::steady_clock::now();
        for (u64 i = 0; i < kOverheadSamples; ++i) {
            const u64 now = cpuReadTicks();
            scratch.ticks += (now - tick);
            scratch.calls += 1;
            tick = now;
        }
        const auto overheadEnd = std::chrono::steady_clock::now();
        const f64 overheadNs =
            std::chrono::duration<f64, std::nano>(overheadEnd - overheadStart).count();
        cpuNsPerTransitionOverhead = overheadNs / static_cast<f64>(kOverheadSamples);
        (void)scratch;  // discarded on purpose -- see the comment above

        cpuCalibrated = true;
        return true;
    }();
    (void)done;
}

} // namespace detail

// Public mirror of the last completed window's accumulators -- a caller outside this file (an editor
// panel, a console command, a log line at the end of a benchmark sweep) asks for this rather than
// touching detail:: state directly, exactly the role D3D12Device::gpuTiming() plays for tsAccum_.
CpuTimingReport collectCpuTiming() {
    CpuTimingReport report;
#if AVER_CPU_TIMING
#if AVER_CPU_TIMING_HAS_RDTSC
    report.supported = true;
    if (detail::cpuPublishedOccurrences == 0) return report;  // capable, nothing published yet
    report.framesAccumulated = detail::cpuPublishedOccurrences;

    report.nodes.reserve(kCpuSpanCount);
    f64 totalRealCalls = 0.0;
    for (usize i = 0; i < kCpuSpanCount; ++i) {
        const CpuSpan span = static_cast<CpuSpan>(i);
        CpuTimingNode node;
        node.label = kCpuSpanLabel[i];
        const CpuSpan parentSpan = kCpuSpanParent[i];
        node.parent =
            (parentSpan == CpuSpan::Count) ? CpuTimingNode::kNoParent : static_cast<u32>(parentSpan);
        // SceneWalk and TimingOverhead are filled in the two passes below, never from the raw
        // accumulator table -- neither is ever the target of a CpuLap::to() or a CpuNest, so their
        // slots in cpuPublishedAccum stay zero forever and reading them here would under-report both.
        if (span != CpuSpan::SceneWalk && span != CpuSpan::TimingOverhead) {
            const detail::CpuSpanAccum& a = detail::cpuPublishedAccum[i];
            node.ms = static_cast<f64>(a.ticks) * detail::cpuNsPerTick * 1e-6;
            node.calls = a.calls;
            totalRealCalls += static_cast<f64>(a.calls);
        }
        report.nodes.push_back(node);
    }

    // SceneWalk = the sum of its mutually-exclusive children, computed here rather than measured
    // directly -- see kCpuSpanParent's own comment on why summing at report time is what GUARANTEES
    // the exact-sum property this report's trustworthiness depends on, instead of merely hoping every
    // call site along the walk stayed disciplined about charging every instruction to some bucket.
    f64 sceneWalkMs = 0.0;
    for (usize i = 0; i < kCpuSpanCount; ++i) {
        if (kCpuSpanParent[i] == CpuSpan::SceneWalk) sceneWalkMs += report.nodes[i].ms;
    }
    CpuTimingNode& sceneWalk = report.nodes[static_cast<usize>(CpuSpan::SceneWalk)];
    sceneWalk.ms = sceneWalkMs;
    // The occurrence count this whole report reflects, not a sum of its children's calls -- summing
    // heterogeneous counts (entities walked, lookups made, dispatches issued) would not answer "how
    // many times did the walk run", it would just add unrelated things together.
    sceneWalk.calls = report.framesAccumulated;

    // TimingOverhead = the instrument's own ESTIMATED tax for this window: the one-time calibrated
    // per-boundary cost times how many boundaries the walk actually crossed this window. This time is
    // already included inside whichever WalkX bucket happened to be open when each boundary ran --
    // it is not additional wall-clock time alongside SceneWalk, which is why CpuTimingFormat.hpp
    // reports it as a footnote against SceneWalk's total rather than as a sibling competing for a
    // share of it. `calls` here is what was taxed, never the calibration loop's own sample size,
    // which never appears in a report at all.
    CpuTimingNode& overhead = report.nodes[static_cast<usize>(CpuSpan::TimingOverhead)];
    overhead.ms = detail::cpuNsPerTransitionOverhead * totalRealCalls * 1e-6;
    overhead.calls = static_cast<u64>(totalRealCalls);
#else
    // See CpuTimingReport::supported's own comment: the fallback's own read cost can run to a
    // quarter of the walk it is timing, so this leaves `nodes` empty rather than publishing numbers
    // this header does not itself trust for a percent-level decision.
    report.supported = false;
#endif  // AVER_CPU_TIMING_HAS_RDTSC
#endif  // AVER_CPU_TIMING
    return report;
}

} // namespace aver
