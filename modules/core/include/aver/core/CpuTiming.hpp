#pragma once
// A CPU-side twin to rhi::GpuTimingReport (see that file's own comment for the shape this mirrors):
// a fixed set of named phases, a fixed parent for each, and a call count alongside every millisecond
// so a bucket reads as "3ms over 96,000 lookups" and not just "3ms".
//
// WHY THIS EXISTS. The scene walk logs one number -- "6.8ms in the rest" -- covering an unknown mix
// of loop overhead, content lookups, frustum tests, route decisions and a GPU dispatch call, and a
// swept entity count (250/1000/4000/16000) shows that number is linear in ENTITY COUNT, not draw
// count, which rules out the obvious guess (it is not the draw call itself) without saying which of
// four candidate fixes it would actually pay for. This header exists to answer that question once,
// honestly, rather than a fourth time by guessing -- this repo has already shipped two informative
// negatives (commit c7e5c645, ded1efdf) from optimising on a number that was not split this way.
//
// WHY IT IS NOT ScopedGpuStat, WORD FOR WORD FROM THAT CLASS'S OWN FILES. ScopedGpuStat's tree is
// discovered at runtime: a parent-stack push/pop and a vector::push_back per open, cheap at the
// "a dozen spans a frame" rate GPU passes run at and ruinous at the rate this header runs at (this
// walk opens/closes on the order of 100,000 times a frame at 16,000 entities). Its accumulator is
// keyed by (label, parent) and re-resolves that pairing every frame, again fine at a dozen spans and
// pointless work here, since the walk's structure -- which phase is a child of which -- is known at
// COMPILE time, not discovered. And kMaxGpuSpans = 64 overflows SILENTLY: D3D12Device.cpp's own
// comment records that the 65th span landed at an index bit-identical to the "no parent" sentinel,
// silently reparenting its children to top-level and double-counting them. Reproducing that shape at
// roughly 250x the span rate invites the same class of bug at a much higher hit rate. So: a fixed
// CpuSpanAccum[Count] array of PODs, filled by index, no strings, no allocation, no runtime-discovered
// parent -- see CpuSpan's own comment for the fixed set this indexes.
//
// DEPENDENCIES ARE DELIBERATELY MINIMAL: this header is included from Runtime/, from
// modules/render.voxi/ and from sandbox/, none of which should have to pull in the rest of
// Aver.Core (Log.hpp's <format>, CrashReport.hpp's OS handles, Assert.hpp's crash-report link) just
// to time a loop. It depends on Types.hpp and the standard library and nothing else in this engine --
// no RHI, no scene, no ImGui -- which is also why the debug-time misuse checks below use the
// standard library's own assert() rather than AVER_ASSERT: assert() disappears under NDEBUG where
// AVER_ASSERT does not, but pulling in Assert.hpp (which pulls in CrashReport.hpp) to catch a wiring
// mistake a developer would hit on the very first debug run of a new call site is a worse trade than
// losing that specific check in a Release build.
#include "aver/core/Types.hpp"

#include <cassert>
#include <chrono>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------------------------
// Tick source selection.
// ---------------------------------------------------------------------------------------------
// THE TICK SOURCE IS THE CRUX OF WHETHER THIS INSTRUMENT IS HONEST. std::chrono::steady_clock is
// QueryPerformanceCounter on MSVC -- roughly 15-25ns a call -- and this header takes on the order of
// 100,000 boundary readings a frame at 16,000 entities, which is 1.6-3.2ms of PURE INSTRUMENT on top
// of the 6.8ms walk it exists to measure: a 25-45% inflation of the very quantity being asked about.
// That is exactly the failure D3D12Device.cpp's own comment on GPU timestamp readback names from the
// other side ("reading this frame's own timings would create the stall it reports") -- a measurement
// that changes the thing it measures is not a measurement, it is a different, slower program.
// __rdtsc() costs roughly 6-10 cycles (call it 2-3ns), which brings the same 100,000 readings to
// under a tenth of a millisecond -- a tax this header can actually state a number for (see
// TimingOverhead's own comment) rather than wave away.
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_AMD64))
    #define AVER_CPU_TIMING_HAS_RDTSC 1
    #include <intrin.h>
#else
    // No __rdtsc on this target (or a compiler this header has not been proven correct against):
    // fall back to steady_clock rather than fail to compile, but see CpuTimingReport::supported --
    // this path is honest about being the slower, self-inflating one instead of quietly handing back
    // a number that looks exactly like the trustworthy one.
    #define AVER_CPU_TIMING_HAS_RDTSC 0
#endif

// ---------------------------------------------------------------------------------------------
// Shipping trim: a COMPILE-TIME off switch, alongside the RUNTIME one further down.
// ---------------------------------------------------------------------------------------------
// Two switches, not one, because they answer two different questions. AVER_CPU_TIMING answers "does
// a shipped binary carry this instrument at all" -- a packaged game has no reason to pay even the
// gated branch's couple of cycles per boundary, nor the .rdata for thirteen label strings, forever.
// cpuTimingEnabled (below) answers "is THIS run of a binary that already has it measuring right now"
// -- and that one has to be a RUNTIME value, not a second compile flag, because the whole point of a
// before/after comparison (the reason this stage exists) is to change ONE thing and hold everything
// else fixed. This engine's own build notes record a -D reconfigure of a shared build tree moving
// rendering by 16% of pixels on its own, before a single line of the change under test is considered
// -- recompiling per variant would make "did my fix help" indistinguishable from "did the reconfigure
// change something else". A runtime bool lets ONE binary produce both halves of that comparison.
#ifndef AVER_CPU_TIMING
    #define AVER_CPU_TIMING 1
#endif

namespace aver {

// ---------------------------------------------------------------------------------------------
// The fixed span set.
// ---------------------------------------------------------------------------------------------
// FIXED IN ADVANCE and identical across every agent that touches the scene walk this frame's
// instrumentation is being added to: a facility and its callers, written by different hands, drift
// exactly here if either side is free to rename, reorder, add or drop an entry. Do not. `Count`
// stays last -- it sizes every table below and doubles as the "no parent" sentinel for the two
// top-level spans, so moving it silently changes both.
enum class CpuSpan : u16 {
    SceneWalk,        // top-level: the whole of drawWorld
    WalkEntity,       //   loop overhead, w.at, destroyPending, CMeshRenderer sparse-set lookup,
                      //   w.worldMatrix (composeChain ancestor walk), the owner-hide walk
    WalkLookup,       //   GameContent hash probes ONLY: meshFor, boundsFor,
                      //   meshDefaultMaterial, partsFor -- a probe actually MADE, so with
                      //   the walk's mesh cache on, only the misses open this span
    WalkFrustum,      //   8-corner world-box build + 6-plane test
    WalkDecide,       //   EntityDecision fill, options.decide(), chooseRoute, planEntityDraws
    WalkEmitDepth,    //   the depth-prepass delivery branch
    WalkEmitRaster,   //   the raster delivery branch
    WalkEmitDirect,   //   the culled/hidden direct route to Voxi
    WalkResolveLook,  //   the resolveDrawLook ladder, entered from all three branches -- once per
                      //   material token per walk (its memo hits are a table read, not this span)
    VoxiSubmit,       //   VoxiRenderer::submit
    ClusterDispatch,  //   GPU cluster dispatch (converted from an existing steady_clock pair)
    WalkOther,        //   explicit remainder -- see below
    TimingOverhead,   // top-level: the instrument measuring itself
    Count
};

// Number of real table entries -- everything except the `Count` sentinel itself.
inline constexpr usize kCpuSpanCount = static_cast<usize>(CpuSpan::Count);

// Human-readable label per span, indexed by the enum value. `static constexpr const char*`, not
// std::string: every label is a string LITERAL known at compile time, so copying one into an
// allocating std::string at startup (the way GpuAccum's `label` has to, because ITS labels come from
// a runtime `const char*` handed to pushMarker) would be an allocation this header exists specifically
// to avoid paying anywhere near its hot path.
inline constexpr const char* kCpuSpanLabel[kCpuSpanCount] = {
    "SceneWalk",
    "WalkEntity",
    "WalkLookup",
    "WalkFrustum",
    "WalkDecide",
    "WalkEmitDepth",
    "WalkEmitRaster",
    "WalkEmitDirect",
    "WalkResolveLook",
    "VoxiSubmit",
    "ClusterDispatch",
    "WalkOther",
    "TimingOverhead",
};

// Static parent per span. `CpuSpan::Count` here means "no parent, top-level" -- the same trick
// GpuTimingNode::kNoParent plays with an out-of-range u32, spelled with the enum's own one-past-the-
// end value instead of a second magic constant, since this table is typed CpuSpan already.
//
// EVERY ENTRY FROM WalkEntity THROUGH WalkOther IS A CHILD OF SceneWalk, MUTUALLY EXCLUSIVE, AND
// THEY SUM TO SceneWalk EXACTLY -- that is what makes the report trustworthy rather than merely
// plausible, and it is why SceneWalk itself is never a target of CpuLap::to() or a CpuNest span:
// see CpuLap's own comment for how the sum is made structurally true instead of merely disciplined.
// WalkOther is the one entry that exists SOLELY so the sum has somewhere honest to put a gap; a
// caller that finds itself with real time attributable to none of the other nine should charge it
// here rather than let it vanish, which is the one way this kind of report can silently lie.
inline constexpr CpuSpan kCpuSpanParent[kCpuSpanCount] = {
    CpuSpan::Count,      // SceneWalk       -- top-level
    CpuSpan::SceneWalk,  // WalkEntity
    CpuSpan::SceneWalk,  // WalkLookup
    CpuSpan::SceneWalk,  // WalkFrustum
    CpuSpan::SceneWalk,  // WalkDecide
    CpuSpan::SceneWalk,  // WalkEmitDepth
    CpuSpan::SceneWalk,  // WalkEmitRaster
    CpuSpan::SceneWalk,  // WalkEmitDirect
    CpuSpan::SceneWalk,  // WalkResolveLook
    CpuSpan::SceneWalk,  // VoxiSubmit
    CpuSpan::SceneWalk,  // ClusterDispatch
    CpuSpan::SceneWalk,  // WalkOther
    CpuSpan::Count,      // TimingOverhead  -- top-level, sibling of SceneWalk, not inside it
};

// ---------------------------------------------------------------------------------------------
// The public report shape -- deliberately mirrors rhi::GpuTimingNode / rhi::GpuTimingReport.
// ---------------------------------------------------------------------------------------------
// One node of a CPU timing report. FLAT AND PARENT-INDEXED for the same reason GpuTimingNode is: the
// source data (kCpuSpanParent above) is already exactly this shape, so this is a straight copy, and
// a caller that wants indentation walks it with an O(n) children-list pass exactly like
// rhi::formatGpuTiming's own Appender -- see CpuTimingFormat.hpp, which is the same walk with the
// serial numbers filed off.
//
// `label` is a raw `const char*` into kCpuSpanLabel, not a std::string -- see that table's own
// comment. It is never null and never owned; nothing here ever frees or reallocates it.
struct CpuTimingNode {
    static constexpr u32 kNoParent = 0xFFFFFFFFu;  // same sentinel value GpuTimingNode uses
    const char* label = nullptr;
    f64 ms = 0;      // inclusive: this bucket's own time, plus (for SceneWalk) everything beneath it
    u64 calls = 0;   // occurrences folded into `ms` -- the number that turns "3ms" into "3ms over
                     // 96,000 lookups", which names a fix where the bare millisecond figure does not
    u32 parent = kNoParent;
};

// A snapshot of the scene walk's CPU timing, as of the last reporting window this process completed.
//
// TWO INDEPENDENT "NO DATA" AXES, deliberately not collapsed into one empty result -- the same
// distinction GpuTimingReport's own comment insists on, for the same reason: the console needs to
// tell "ask again later" apart from "this build will never answer honestly", and a single bool
// cannot say both.
//   - `supported` is the CAPABILITY axis. False means either AVER_CPU_TIMING trimmed this facility
//     out of the binary entirely, or this process is on the steady_clock fallback (not MSVC x64) --
//     in BOTH cases `nodes` stays empty rather than publishing numbers this header itself does not
//     trust for a percent-level decision (see AVER_CPU_TIMING_HAS_RDTSC's own comment on why the
//     fallback's own overhead can run to a quarter of the walk it is timing).
//   - `nodes` empty (or `framesAccumulated` 0) with `supported` true is the CONTENT axis: capable and
//     enabled, but no reporting window has completed since boot (or since the last one) yet.
struct CpuTimingReport {
    bool supported = false;
    // Occurrences of the measured top-level scope folded into this window -- typically one
    // drawWorld() call each, so twice a rendered frame on a host whose depth prepass walks the
    // scene a second time (see GameRender.hpp). NOT a GPU-style since-boot average: see CpuLap's own
    // comment on why this facility rotates a bounded window instead.
    u32 framesAccumulated = 0;
    std::vector<CpuTimingNode> nodes;
};

// ---------------------------------------------------------------------------------------------
// Runtime kill switch.
// ---------------------------------------------------------------------------------------------
// GATING: tested at each boundary (CpuLap::to(), both of CpuLap's ends, both of CpuNest's ends).
// When true this costs one global bool load and a predicted-taken branch before a handful of
// instructions -- a couple of cycles, negligible even at 100,000 boundaries a frame. When false, it
// is one load and a NOT-taken branch that skips the tick read and the accumulator write entirely --
// which is the actual saving, since the read is the expensive half. See AVER_CPU_TIMING's own
// comment for why this is a second, runtime-only switch rather than a rebuild.
//
// TOGGLING THIS MID-WALK (between one boundary and the next, rather than between one drawWorld()
// call and the next) is not a case this header defends against: a CpuLap or CpuNest whose start_
// tick was taken before the flag flipped off, and which closes after it flipped back on, folds one
// stale delta into whatever span happens to close next. That is a bounded, one-time, self-correcting
// error -- the FOLLOWING boundary re-synchronises to a fresh, valid tick -- and accepting it is what
// keeps every boundary a single branch instead of two. The flag is meant to be flipped between
// measurement runs (an editor checkbox, a launch argument), never inside one.
inline bool cpuTimingEnabled = true;

namespace detail {

// The POD this whole facility revolves around: no string, no allocation, indexed by CpuSpan, filled
// entirely at compile-known offsets. `ticks` are raw units of whatever readTicks() returns (TSC
// counts on the fast path, nanoseconds on the fallback) -- never converted to milliseconds until a
// report is actually built, which is what lets calibration happen once instead of on every span.
struct CpuSpanAccum {
    u64 ticks = 0;
    u64 calls = 0;
};

// The span currently open, and the tick it opened on -- THREAD_LOCAL, unlike almost everything else
// in this file. The render walk itself is single-threaded, but "which span is open right now" is
// exactly the piece of state a CpuNest reads to find out what it is suspending (see CpuNest's own
// comment), and making just this one variable thread_local is nearly free (one extra TLS
// indirection, paid at the same boundaries that already pay for a tick read) while removing an
// entire class of hazard if this header is ever reached from a second thread -- a background job
// that happens to construct a CpuLap of its own would otherwise silently stomp the render thread's
// in-flight span instead of getting its own. The AGGREGATE state below (the accumulator arrays, the
// window/occurrence counters) is deliberately NOT thread_local: the whole point of folding several
// CpuLap occurrences into one window is to produce ONE combined report, and thread_local there would
// silently report only whichever thread happened to read it -- wrong for this specific purpose even
// though it would be "safer" in the abstract. That aggregate state is therefore plain, mutable,
// global, and NOT thread-safe against concurrent writers: correct today because only the render
// thread ever opens a CpuLap or CpuNest for a CpuSpan, and worth restating loudly here rather than
// leaving a future reader to discover it by racing.
struct ActiveSpan {
    CpuSpan span = CpuSpan::Count;  // Count doubles as "nothing open", same sentinel as kNoParent
    u64 start = 0;
};
inline thread_local ActiveSpan cpuActiveSpan{};

// The live window (currently being filled) and the last window a report is allowed to read (frozen,
// safe to walk without racing a boundary still being written). See CpuLap's own comment on the
// window-rotation policy these two buffers implement.
inline CpuSpanAccum cpuLiveAccum[kCpuSpanCount]{};
inline CpuSpanAccum cpuPublishedAccum[kCpuSpanCount]{};
inline u32 cpuLiveOccurrences = 0;
inline u32 cpuPublishedOccurrences = 0;

// How many completed top-level occurrences (CpuLap constructions) make one reporting window. Chosen
// as a few tens of frames' worth: long enough that OS-scheduling jitter on any one frame washes out
// of the average, short enough that a benchmark sweep changing entity count (the whole reason this
// header exists -- 250/1000/4000/16000) sees a window that reflects the CURRENT configuration within
// a second or two rather than being diluted by whatever configuration ran before it. This is also
// exactly why this facility rotates a bounded window instead of the GPU side's since-boot running
// average (tsAccumFrames_): a since-boot average would blend an old entity count into a new one's
// numbers for the rest of the process's life, hiding precisely the delta a sweep exists to show.
inline constexpr u32 kCpuTimingWindowOccurrences = 30;

// Nanoseconds per tick, and the instrument's own per-boundary tax in nanoseconds -- both calibrated
// exactly ONCE, lazily, at the first CpuLap ever constructed (see cpuEnsureCalibrated, defined in
// CpuTiming.cpp so the busy-wait loop and <chrono> plumbing it needs do not bloat this header's hot
// path). 0 until calibration runs; a report built before that point (which cpuPublishedOccurrences
// == 0 already rules out on its own) would otherwise silently show zero milliseconds for real ticks.
inline f64 cpuNsPerTick = 0.0;
inline f64 cpuNsPerTransitionOverhead = 0.0;
inline bool cpuCalibrated = false;

// Defined in CpuTiming.cpp: the one-time calibration this header's hot path never pays for. Declared
// here, not inlined, on purpose -- it pulls in the busy-wait loop and the calibration constants, none
// of which belong in a header included from three unrelated modules for the sake of a few boundary
// reads a frame.
void cpuEnsureCalibrated();

// The actual clock read. AVER_FORCEINLINE-worthy by construction (a single intrinsic, or a single
// chrono call) -- kept a free function rather than a macro so both CpuLap and CpuNest share one
// definition instead of two copies drifting.
//
// __rdtsc() IS NOT A SERIALISING INSTRUCTION: the CPU may execute it a few instructions early or
// late relative to surrounding code, so any ONE reading can be off by the cost of a handful of
// instructions. That is exactly the kind of error this header's use case shrugs off and a single
// authoritative sample could not: this instrument sums tens of thousands of deltas into a handful of
// buckets, so a few-instruction wobble on any one boundary is noise several orders of magnitude
// below the millisecond totals it is being read into, and paying for a serialising read (CPUID
// fencing, or the slower __rdtscp) to remove that noise would cost far more than the noise itself.
//
// MODERN X86 TSC IS INVARIANT -- it ticks at a fixed rate regardless of the core's current clock
// speed or C-state, on every CPU this engine's minimum spec covers. "rdtsc drifts with clock speed"
// was true of the ORIGINAL rdtsc behaviour and has not been true of shipping hardware for roughly two
// decades; it survives as received wisdom because it used to be correct, not because it still is.
// This is what makes calibrating the ns-per-tick ratio ONCE, at startup, valid for the rest of the
// process -- a non-invariant counter would need re-calibrating every time the core's frequency
// changed, which would defeat the entire "convert at report time only" design.
inline u64 cpuReadTicks() {
#if AVER_CPU_TIMING_HAS_RDTSC
    return __rdtsc();
#else
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

// Adds `deltaTicks` to `span`'s bucket WITHOUT counting an occurrence -- used when a span is merely
// PAUSED (a CpuNest suspending it) rather than finished. Pausing a span many times in one occurrence
// must not inflate the `calls` count that is meant to answer "how many times did this run", only how
// long it ran; only a genuine close (see cpuSpanClose) counts an occurrence.
inline void cpuSpanFlush(CpuSpan span, u64 deltaTicks) {
    if (span == CpuSpan::Count) return;  // "nothing was open" sentinel -- nothing to fold in
    cpuLiveAccum[static_cast<usize>(span)].ticks += deltaTicks;
}

// Adds `deltaTicks` AND counts one finished occurrence -- used exactly where a span is deemed
// COMPLETE: the outgoing span of a CpuLap::to() transition, a CpuLap's destructor, or a CpuNest's
// destructor closing its own span.
inline void cpuSpanClose(CpuSpan span, u64 deltaTicks) {
    if (span == CpuSpan::Count) return;
    CpuSpanAccum& a = cpuLiveAccum[static_cast<usize>(span)];
    a.ticks += deltaTicks;
    a.calls += 1;
}

// Called from CpuLap's constructor only -- see CpuLap's own comment for why tying window rotation to
// "a top-level Lap opened" is the one hook that cannot be forgotten by whoever wires this facility up
// to the real walk. Bumps the occurrence count and, once a full window's worth has landed in
// cpuLiveAccum, freezes it into cpuPublishedAccum and starts the next window clean.
//
// A FACILITY THAT SILENTLY ACCUMULATED FOREVER INSTEAD OF ROTATING would not fail loudly -- it would
// keep returning a perfectly well-formed, plausible-looking report whose numbers just happened to be
// a running average blending whatever entity count or code path was live minutes ago into whatever is
// live now. That is worse than a crash, because nothing about the report's SHAPE would say so; it is
// exactly the "torn pair" and "gates went blind" failure mode this codebase's own build notes warn
// reads as a valid measurement right up until someone notices the number never moves.
inline void cpuOnLapOpen() {
    cpuEnsureCalibrated();
    if (cpuLiveOccurrences >= kCpuTimingWindowOccurrences) {
        std::memcpy(cpuPublishedAccum, cpuLiveAccum, sizeof(cpuLiveAccum));
        cpuPublishedOccurrences = cpuLiveOccurrences;
        std::memset(cpuLiveAccum, 0, sizeof(cpuLiveAccum));
        cpuLiveOccurrences = 0;
    }
    ++cpuLiveOccurrences;
}

} // namespace detail

// ---------------------------------------------------------------------------------------------
// CpuLap -- an EXCLUSIVE phase marker.
// ---------------------------------------------------------------------------------------------
// Opens `first` at construction. `lap.to(next)` closes whatever is currently open and opens `next`
// with ONE tick read serving as both the outgoing span's end and the incoming span's start -- the
// two are back-to-back with nothing between them, so one read carries exactly the same information
// as two would, at half the cost. The destructor closes whatever was last open.
//
// ONE Lap PER MEASURED SCOPE, not one per phase: the intended shape is a single CpuLap constructed
// once at the top of drawWorld(), threaded through the whole per-entity loop via repeated `.to()`
// calls as the walk moves from WalkEntity to WalkLookup to WalkFrustum and on, rather than a fresh
// CpuLap per entity or per phase. That shape is exactly what makes the destructor sufficient for the
// eight `continue` statements the entity loop needs: a `continue` simply leaves whatever span was
// open at that point still open, ticking, until the NEXT `.to()` call (the following iteration's
// first transition) closes it -- and on the very last entity, if a `continue` fires with nothing left
// to run afterward, the outer Lap's own destructor closes it instead. Nothing at any `continue` site
// has to remember to close anything by hand, which is the identical argument RHIResources.hpp makes
// for ScopedGpuStat's destructor over a hand-repeated popMarker() -- a forgotten close there leaves a
// backend's marker stack off by one for the rest of the run; a forgotten close here leaves a
// CpuSpanAccum bucket silently short, which is just as wrong and far less likely to be noticed.
//
// SceneWalk and TimingOverhead ARE NEVER PASSED HERE. Both are computed, not measured -- SceneWalk as
// the sum of its children (see kCpuSpanParent's own comment on why that sum is exact BECAUSE it is
// computed rather than separately measured and hoped to agree), TimingOverhead from the startup
// calibration loop (see cpuNsPerTransitionOverhead). Passing either is a wiring mistake caught by the
// debug-only assert below, not a supported use.
class CpuLap {
public:
    explicit CpuLap(CpuSpan first) : span_(first) {
#if AVER_CPU_TIMING
        assert(first != CpuSpan::SceneWalk && first != CpuSpan::TimingOverhead &&
               first != CpuSpan::Count &&
               "CpuLap must open on a real leaf span -- SceneWalk and TimingOverhead are computed at "
               "report time, never measured directly (see this class's own comment).");
        // The Lap's own open/close bracket runs once or twice a frame (see CpuTimingReport::
        // framesAccumulated's comment on the depth-prepass case) -- nowhere near the ~100,000-a-frame
        // hot path the runtime gate exists to protect, which is .to() below. Reading a tick here
        // unconditionally, regardless of cpuTimingEnabled, keeps start_ always valid even if the flag
        // flips between this constructor and the first .to() call, at a cost too small to matter.
        detail::cpuOnLapOpen();
        start_ = detail::cpuReadTicks();
        detail::cpuActiveSpan = {span_, start_};
#endif
    }

    ~CpuLap() {
#if AVER_CPU_TIMING
        if (!cpuTimingEnabled) return;
        const u64 now = detail::cpuReadTicks();
        detail::cpuSpanClose(span_, now - start_);
        detail::cpuActiveSpan = {CpuSpan::Count, 0};
#endif
    }

    // Closes the currently-open span as one finished occurrence and opens `next`.
    void to(CpuSpan next) {
#if AVER_CPU_TIMING
        assert(next != CpuSpan::SceneWalk && next != CpuSpan::TimingOverhead &&
               next != CpuSpan::Count && "see CpuLap's own comment: neither is ever measured directly.");
        if (!cpuTimingEnabled) { span_ = next; return; }  // see cpuTimingEnabled's own comment on
                                                           // mid-walk toggling for why this is safe
        const u64 now = detail::cpuReadTicks();
        detail::cpuSpanClose(span_, now - start_);
        span_ = next;
        start_ = now;
        detail::cpuActiveSpan = {span_, start_};
#else
        (void)next;
#endif
    }

    CpuLap(const CpuLap&) = delete;
    CpuLap& operator=(const CpuLap&) = delete;

private:
    CpuSpan span_;
    u64 start_ = 0;
};

// ---------------------------------------------------------------------------------------------
// CpuNest -- for a region reachable from more than one enclosing phase.
// ---------------------------------------------------------------------------------------------
// WalkLookup, WalkResolveLook, VoxiSubmit and ClusterDispatch are each called from several different
// WalkEmit* branches, so the call site cannot simply `lap.to()` into them -- doing so would require
// every caller to know, and correctly restore, whichever phase was running before it, which is
// exactly the kind of bookkeeping CpuLap's single-current-span model does not support and should not
// be made to. CpuNest instead SUSPENDS whatever is currently open (read out of detail::cpuActiveSpan,
// since the call site has no reference to the enclosing CpuLap and cannot pass it one), accumulates
// into its OWN bucket for its own lifetime, and RESUMES the suspended span when it destructs --
// restarting its clock at that exact instant so the time spent inside the nest is counted once, here,
// and not a second time inside whatever it suspended. This is what keeps these four spans exclusive
// of their enclosing phase, the same property kCpuSpanParent's own comment states for the walk's
// other children: without it, WalkLookup's time would be double-counted once under WalkLookup and
// again inside whichever WalkEmit* phase called it, which is the identical bug the GPU side's
// kNoAccumParent comment describes from the opposite direction (a span whose parent cannot be
// resolved gets counted as top-level AND under its real parent).
//
// NESTING (a CpuNest opened while another CpuNest is already open -- VoxiSubmit's own dispatch
// calling into ClusterDispatch is the concrete case this walk needs) IS SUPPORTED, and deliberately
// NOT via a fixed-size stack array: a hand-rolled array needs its own capacity constant and its own
// silent-overflow behaviour once that capacity is exceeded, which is precisely the kMaxGpuSpans
// failure D3D12Device.cpp's own comment describes and this header exists in part to avoid repeating.
// Instead, each CpuNest stores the ONE ActiveSpan it suspended in its OWN local member, and restores
// exactly that on destruction. Because C++ destroys automatic objects in the reverse of their
// construction order, a chain of nested CpuNest objects on the call stack already forms a LIFO stack
// on its own, at zero extra storage and with no depth this header has to cap or guard: the only bound
// on nesting depth is the C++ call stack itself, which is a bound the compiler and OS already enforce
// far more cheaply than a hand-rolled array ever could.
class CpuNest {
public:
    explicit CpuNest(CpuSpan span) : span_(span) {
#if AVER_CPU_TIMING
        assert(span != CpuSpan::SceneWalk && span != CpuSpan::TimingOverhead &&
               span != CpuSpan::Count && "see CpuLap's own comment: neither is ever measured directly.");
        active_ = cpuTimingEnabled;
        if (!active_) return;
        start_ = detail::cpuReadTicks();
        suspended_ = detail::cpuActiveSpan;
        if (suspended_.span != CpuSpan::Count) {
            detail::cpuSpanFlush(suspended_.span, start_ - suspended_.start);
        }
        detail::cpuActiveSpan = {span_, start_};
#endif
    }

    ~CpuNest() {
#if AVER_CPU_TIMING
        if (!active_) return;
        const u64 now = detail::cpuReadTicks();
        detail::cpuSpanClose(span_, now - start_);
        // Resume whatever this nest suspended, restarting ITS clock at this instant -- the interval
        // just spent inside this nest has already been counted, here, and must not be counted again
        // against the span being resumed.
        detail::cpuActiveSpan = {suspended_.span, now};
#endif
    }

    CpuNest(const CpuNest&) = delete;
    CpuNest& operator=(const CpuNest&) = delete;

private:
    CpuSpan span_;
    u64 start_ = 0;
#if AVER_CPU_TIMING
    bool active_ = false;
    detail::ActiveSpan suspended_{};
#endif
};

// Builds a CpuTimingReport from the last completed reporting window. Defined in CpuTiming.cpp: it
// walks all kCpuSpanCount entries, converts ticks to milliseconds using the calibration
// cpuEnsureCalibrated() already ran, and computes SceneWalk's and TimingOverhead's node values (see
// their own comments) -- none of which belongs inlined into a header three unrelated modules include
// for the sake of a handful of boundary reads a frame.
CpuTimingReport collectCpuTiming();

} // namespace aver
