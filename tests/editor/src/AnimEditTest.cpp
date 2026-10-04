// AnimEditTest -- sandbox/src/AnimEdit.hpp: the pure decisions behind editing one OcTrack's keys and
// retiming a whole OcAnimation, with no ImGui, no AnimEditor, no device and no I/O anywhere near it.
//
// WHY THIS TEST CAN EXIST AT ALL, when almost nothing about the editor can be tested: every function
// under test takes a plain aver::fmt:: struct in and returns a plain value or mutates that struct in
// place. AnimEdit.hpp's own top comment makes the same claim RevisionControl.hpp's does, and this
// file follows RevisionControlTest.cpp for the same reason that one exists: a decision this pure is
// reachable with no repository, no window and no engine, and it is exactly the kind of decision that
// fails silently in a screenshot rather than loudly in a crash.
//
// WHAT IT IS DEFENDING. AnimEdit.hpp's own top comment names three traps and this file is organised
// around proving each one shut:
//
//   THE STRIDE. OcTrack::values is key-major, and componentsPerKey() triples under CubicSpline. An
//   insert or delete that gets this wrong keeps the KEY COUNT right (nothing here throws or logs)
//   while quietly reassigning which floats belong to which surviving key -- a bone that looks fine in
//   the track table and wrong the instant the clip plays. Every stride-touching test below therefore
//   checks VALUES at surviving keys, not merely counts, and the CubicSpline cases hand-derive their
//   expected Hermite numbers independently rather than trusting the same code path twice.
//
//   RETIMING. A scale or a trim that moves a track's keys without moving its notifies, its notify
//   durations and its curves by the same rule desyncs a footstep sound from the foot that triggers it
//   -- and does so silently, because nothing about a desynced notify fails to compile, fails to save,
//   or looks wrong in a static screenshot of frame zero. The retime tests below build a notify sitting
//   exactly on a key and then require it to still be exactly on a key afterwards.
//
//   THE PALETTE. A bone-colouring rule that only distinguishes DIRECT CHILDREN of the skeleton root
//   would paint an entire biped's upper body -- both arms included -- as one chain. The palette tests
//   below build a rig with a real fork below the root specifically to catch that mistake.
//
// ONE SOURCE FILE, Aver.Formats and Aver.Core -- the same claim RevisionControlTest.cpp makes for
// RevisionControl.hpp. AnimEdit.hpp reaches for aver/core/Types.hpp and aver/formats/OcAnim.hpp alone,
// so this target needs no window, no ImGui context, and no sandbox .cpp compiled alongside it: the
// header is entirely `inline`, and OcTrack::componentsPerKey()/valid() and OcAnimation::valid() are
// the only symbols this pulls from Aver.Formats itself (modules/formats/src/OcAnim.cpp). If this
// target ever needs Aver.Platform for anything beyond what Aver.Formats already carries transitively,
// or needs a sandbox .cpp compiled into it, AnimEdit.hpp has stopped being a pure header and the split
// its own top comment describes has been undone.
#include "AnimEdit.hpp"

#include "aver/formats/OcAnim.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::editor;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) {
        std::printf("[INFO ]   ok    %s\n", what);
    } else {
        ++g_failures;
        std::printf("[ERROR]   FAIL  %s\n", what);
    }
}

// A tight tolerance for values that ought to be bit-exact or nearly so: everything hand-derived below
// uses dyadic fractions (0.5, 0.25, ...) that float arithmetic represents exactly, so a real bug shows
// up as an error many times this size, never as noise at this size.
bool near(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }

bool vecNear(const std::vector<f32>& a, const std::vector<f32>& b, f32 eps = 1e-4f) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i)
        if (!near(a[i], b[i], eps)) return false;
    return true;
}

// Euclidean distance in the little RGB cube BoneColor lives in -- the same "would a person notice"
// yardstick the palette tests below hold two chains' colours to.
f32 colorDistance(const BoneColor& a, const BoneColor& b) {
    const f32 dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
    return std::sqrt(dr * dr + dg * dg + db * db);
}

// A three-key, TWO-CHANNEL (translation + rotation) Linear track -- the shape the stride trap
// actually bites on, because a single-channel track's stride math cannot tell a channel-offset bug
// from a key-offset bug. Returns a FRESH copy every call so insert/delete/move tests never see each
// other's mutations.
fmt::OcTrack makeTRTrack() {
    fmt::OcTrack t;
    t.channels = fmt::kOcChannelTranslation | fmt::kOcChannelRotation;
    t.interp = fmt::OcInterp::Linear;
    t.times = {0.0f, 1.0f, 2.0f};
    t.values = {
        0.0f, 0.0f, 0.0f,  0.0f, 0.0f, 0.0f, 1.0f,        // key0: T=(0,0,0)      R=identity
        1.0f, 2.0f, 3.0f,  0.0f, 0.0f, 0.7071068f, 0.7071068f,  // key1: T=(1,2,3) R=(0,0,.707,.707)
        2.0f, 4.0f, 6.0f,  0.0f, 0.0f, 1.0f, 0.0f,        // key2: T=(2,4,6)      R=(0,0,1,0)
    };
    return t;
}

// A two-key, single-channel (rotation) CubicSpline track, laid out BY HAND rather than through
// trackWriteComponent -- an independently built golden, exactly as RevisionControlTest's own porcelain
// records are typed out rather than produced by the parser under test. Only component index 2 (the
// third of the four rotation floats) and, on key0, component 3 carry non-zero data; every other
// component stays zero for the whole file, which is what makes it obvious below whether a stride bug
// bled one component's math into another's.
fmt::OcTrack makeCubicRotTrack() {
    fmt::OcTrack t;
    t.channels = fmt::kOcChannelRotation;
    t.interp = fmt::OcInterp::CubicSpline;
    t.times = {0.0f, 2.0f};
    t.values = {
        // key0: inTan=(0,0,0,0)         value=(0,0,0,1)          outTan=(0,0,0.25,0)
        0.0f, 0.0f, 0.0f, 0.0f,   0.0f, 0.0f, 0.0f, 1.0f,   0.0f, 0.0f, 0.25f, 0.0f,
        // key1: inTan=(0,0,-0.25,0)     value=(0,0,1,0)          outTan=(0,0,0,0)
        0.0f, 0.0f, -0.25f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,   0.0f, 0.0f, 0.0f, 0.0f,
    };
    return t;
}

} // namespace

int main() {
    std::printf("[INFO ] === anim edit ===\n");

    // ---- the load-bearing numbering and sentinel --------------------------------------------------
    //
    // AnimEdit.hpp's own comment says TrackKeySlot's 0/1/2 numbering mirrors AnimSampler's private
    // slot convention -- get this wrong and an edit here disagrees with the runtime sampler about
    // which third of a CubicSpline key means what, with nothing to say so.
    {
        check(static_cast<u8>(TrackKeySlot::InTangent) == 0, "slot numbering: InTangent is 0");
        check(static_cast<u8>(TrackKeySlot::Value) == 1, "slot numbering: Value is 1");
        check(static_cast<u8>(TrackKeySlot::OutTangent) == 2, "slot numbering: OutTangent is 2");
        check(kInvalidKeyIndex == static_cast<usize>(-1),
              "the sentinel is size_t(-1), the one value no real index can ever collide with");
        check(trackChannelWidth(fmt::kOcChannelRotation) == 4, "rotation is a quaternion: 4 floats");
        check(trackChannelWidth(fmt::kOcChannelTranslation) == 3, "translation is 3 floats");
        check(trackChannelWidth(fmt::kOcChannelScale) == 3, "scale is 3 floats");
    }

    // ---- trackComponentOffset: the one place the stride arithmetic is done -------------------------
    //
    // A track with all three channels enabled AND CubicSpline, so every one of the three per-channel
    // thirds and all three channels' base offsets are exercised in one fixture. componentsPerKey() is
    // (3+4+3)*3 = 30; these offsets are independently hand-derived from the documented per-channel
    // [in x width, value x width, out x width] layout, not read back from the function under test.
    {
        fmt::OcTrack all;
        all.channels = fmt::kOcChannelTranslation | fmt::kOcChannelRotation | fmt::kOcChannelScale;
        all.interp = fmt::OcInterp::CubicSpline;
        check(all.componentsPerKey() == 30, "stride: (3+4+3) channel floats tripled by CubicSpline");

        u32 off = 0;
        check(trackComponentOffset(all, fmt::kOcChannelTranslation, 0, TrackKeySlot::Value, off) && off == 3,
              "translation's VALUE third starts right after its own in-tangent third, at offset 3");
        check(trackComponentOffset(all, fmt::kOcChannelTranslation, 2, TrackKeySlot::OutTangent, off) && off == 8,
              "translation's out-tangent third ends the whole translation block at offset 8");
        check(trackComponentOffset(all, fmt::kOcChannelRotation, 0, TrackKeySlot::InTangent, off) && off == 9,
              "rotation's block starts at 9 -- right after translation's 9 floats, not at a flat width*3");
        check(trackComponentOffset(all, fmt::kOcChannelRotation, 3, TrackKeySlot::Value, off) && off == 16,
              "rotation's own 4th component's VALUE sits at 16, inside rotation's block, not spilling into scale's");
        check(trackComponentOffset(all, fmt::kOcChannelScale, 1, TrackKeySlot::OutTangent, off) && off == 28,
              "scale's block starts at 21 (9 + 12), so its 2nd component's out-tangent lands at 28");

        // THREE REFUSALS, each one a case where AnimEdit.hpp deliberately fails rather than aliasing
        // the request onto a different float -- see AnimEdit.hpp's own comment on why this is
        // STRICTER than AnimSampler's runtime reader.
        fmt::OcTrack noRot;
        noRot.channels = fmt::kOcChannelTranslation | fmt::kOcChannelScale;
        noRot.interp = fmt::OcInterp::Linear;
        check(!trackComponentOffset(noRot, fmt::kOcChannelRotation, 0, TrackKeySlot::Value, off),
              "a channel absent from the mask is refused, never silently mapped onto a present one");
        check(!trackComponentOffset(noRot, fmt::kOcChannelTranslation, 3, TrackKeySlot::Value, off),
              "a component at or past its channel's own width is refused, not wrapped into the next channel");
        check(!trackComponentOffset(noRot, fmt::kOcChannelTranslation, 0, TrackKeySlot::InTangent, off),
              "a tangent slot on a non-CubicSpline track is refused rather than aliased onto the value slot -- "
              "a drag on a handle that should not have been drawn must not silently move the bone instead");
    }

    // ---- trackReadComponent / trackWriteComponent: the single-float primitive -----------------------
    {
        fmt::OcTrack t;
        t.channels = fmt::kOcChannelTranslation;
        t.interp = fmt::OcInterp::Linear;
        t.times = {0.0f};
        t.values = {5.0f, 6.0f, 7.0f};

        f32 v = 0.0f;
        check(trackReadComponent(t, 0, fmt::kOcChannelTranslation, 1, TrackKeySlot::Value, v) && v == 6.0f,
              "reads the exact float the offset math says it should, not a neighbour");
        check(trackWriteComponent(t, 0, fmt::kOcChannelTranslation, 1, TrackKeySlot::Value, 42.0f),
              "writes through the same offset");
        check(trackReadComponent(t, 0, fmt::kOcChannelTranslation, 1, TrackKeySlot::Value, v) && v == 42.0f,
              "and the write is what the next read sees");
        check(!trackWriteComponent(t, 0, fmt::kOcChannelTranslation, 0, TrackKeySlot::Value,
                                    std::numeric_limits<f32>::quiet_NaN()),
              "a non-finite write is refused outright -- one poisoned float here poisons every sample of it");
        check(!trackReadComponent(t, 5, fmt::kOcChannelTranslation, 0, TrackKeySlot::Value, v),
              "an out-of-range key index fails instead of reading past the array");
    }

    // ---- findTrackKeySpan: clamped ends, and a time landing exactly on a key ------------------------
    {
        fmt::OcTrack t;
        t.times = {0.0f, 1.0f, 2.0f};
        f32 alpha = 0.0f;
        usize next = 0;

        check(findTrackKeySpan(t, -5.0f, alpha, next) == 0 && next == 0 && near(alpha, 0.0f),
              "a time before the first key clamps to it rather than extrapolating");
        check(findTrackKeySpan(t, 50.0f, alpha, next) == 2 && next == 2 && near(alpha, 0.0f),
              "a time after the last key clamps to it the same way");
        check(findTrackKeySpan(t, 1.5f, alpha, next) == 1 && next == 2 && near(alpha, 0.5f),
              "a time strictly between two keys finds both ends and the right fraction between them");
    }

    // ---- hermiteValue / hermiteVelocity: the shared Hermite basis -----------------------------------
    {
        check(near(hermiteValue(0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f), 0.0f), "at s=0 the curve is exactly v0");
        check(near(hermiteValue(0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f), 1.0f), "at s=1 the curve is exactly v1");
        // hermiteVelocity's own comment: at s=0 it returns EXACTLY b0, for ANY dt -- that independence
        // from segment length is what lets trackInsertKey split a segment without rewriting either
        // end's own tangent.
        check(near(hermiteVelocity(0.0f, 2.0f, 5.0f, 3.0f, 0.0f, 4.0f), 2.0f), "at s=0 the slope is exactly b0");
        check(near(hermiteVelocity(0.0f, 2.0f, 5.0f, 3.0f, 1.0f, 4.0f), 3.0f), "at s=1 the slope is exactly a1");
        check(near(hermiteVelocity(0.0f, 2.0f, 5.0f, 3.0f, 0.5f, 0.0f), 0.0f),
              "a zero-length segment has no slope to give, rather than dividing by zero");
    }

    // ---- trackSampleAt: Step holds, Linear blends, and channel order skips a missing channel --------
    {
        fmt::OcTrack step;
        step.channels = fmt::kOcChannelTranslation;
        step.interp = fmt::OcInterp::Step;
        step.times = {0.0f, 1.0f};
        step.values = {10.0f, 0.0f, 0.0f, 20.0f, 0.0f, 0.0f};
        std::vector<f32> out;
        check(trackSampleAt(step, 0.9f, out) && near(out[0], 10.0f),
              "Step holds the PRECEDING key's value right up to the next one, never blending toward it");

        // Translation and Scale enabled, Rotation NOT -- so a live track table has to skip the middle
        // slot in channel order rather than reading garbage where rotation would have been.
        fmt::OcTrack gap;
        gap.channels = fmt::kOcChannelTranslation | fmt::kOcChannelScale;
        gap.interp = fmt::OcInterp::Linear;
        gap.times = {0.0f, 1.0f};
        gap.values = {0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f,   2.0f, 0.0f, 0.0f, 3.0f, 3.0f, 3.0f};
        check(trackSampleAt(gap, 0.0f, out) && out.size() == 6, "T+S with no rotation: 3+3, not 3+4+3");
        check(vecNear(out, {0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f}),
              "channel order still runs translation-then-scale with rotation skipped, not shifted or duplicated");

        std::vector<f32> empty;
        fmt::OcTrack blank;
        check(!trackSampleAt(blank, 0.0f, empty) && empty.empty(),
              "a track with no keys and no mask fails outright rather than returning a zero pose");
    }

    // ---- trackInsertKey / trackDeleteKey: the stride trap, on a translation+rotation track ----------
    {
        fmt::OcTrack t = makeTRTrack();
        const std::vector<f32> originalValues = t.values;
        const std::vector<f32> originalTimes = t.times;

        const usize insertedAt = trackInsertKey(t, 0.5f);
        check(insertedAt == 1, "0.5 sorts between key0 (t=0) and key1 (t=1)");
        check(t.valid(), "OcTrack::valid() holds immediately after the insert");
        check(t.times.size() == 4 && t.values.size() == 4 * 7,
              "4 keys at 7 floats each -- the format's own invariant, not merely a plausible count");

        // What USED TO BE key1 (t=1, T=(1,2,3), R=(0,0,.707,.707)) now lives at index 2. A stride bug
        // keeps this count right while reading these floats off the WRONG base -- so every assertion
        // here reads a specific component, not just a length.
        f32 v = 0.0f;
        check(trackReadComponent(t, 2, fmt::kOcChannelTranslation, 0, TrackKeySlot::Value, v) && v == 1.0f,
              "the surviving key's translation.x is still its OWN 1.0, not the inserted key's or key0's 0.0");
        check(trackReadComponent(t, 2, fmt::kOcChannelTranslation, 2, TrackKeySlot::Value, v) && v == 3.0f,
              "and translation.z is still 3.0");
        check(trackReadComponent(t, 2, fmt::kOcChannelRotation, 3, TrackKeySlot::Value, v) && near(v, 0.7071068f),
              "and rotation's own 4th component is still .707, not translation's leftover 3.0 read at a shifted base");
        // What USED TO BE key2 (t=2, T=(2,4,6), R=(0,0,1,0)) now lives at index 3.
        check(trackReadComponent(t, 3, fmt::kOcChannelTranslation, 1, TrackKeySlot::Value, v) && v == 4.0f,
              "the far surviving key's translation.y is still its own 4.0");
        check(trackReadComponent(t, 3, fmt::kOcChannelRotation, 2, TrackKeySlot::Value, v) && v == 1.0f,
              "and its rotation.z is still its own 1.0");

        // ROUND TRIP: deleting exactly the key that was just inserted must restore the ORIGINAL track
        // byte-for-byte -- a stride bug that merely gets the COUNT back to 3 keys / 21 floats would
        // still pass a size check and fail this one.
        check(trackDeleteKey(t, insertedAt), "delete the freshly inserted key");
        check(t.times == originalTimes, "round trip: every key TIME is restored exactly");
        check(t.values == originalValues,
              "round trip: every one of the 21 floats is restored exactly, not merely the right COUNT of them");
    }
    {
        // Deleting a key that was NEVER inserted: the survivor's data must be ITS OWN, not key0's
        // leftover bytes reinterpreted under a shifted stride.
        fmt::OcTrack t = makeTRTrack();
        check(trackDeleteKey(t, 0), "delete key0 (t=0)");
        check(t.times.size() == 2 && t.times[0] == 1.0f && t.times[1] == 2.0f, "the two later keys survive, in order");
        f32 v = 0.0f;
        check(trackReadComponent(t, 0, fmt::kOcChannelTranslation, 0, TrackKeySlot::Value, v) && v == 1.0f,
              "what used to be key1 is now index 0, and its translation.x is still its OWN 1.0");
        check(trackReadComponent(t, 0, fmt::kOcChannelRotation, 3, TrackKeySlot::Value, v) && near(v, 0.7071068f),
              "and its rotation is still its own, not key0's identity rotation shifted down");
        check(t.valid(), "still a valid track with one key fewer");
    }

    // ---- trackInsertKey changes nothing about the pose anywhere else ------------------------------
    //
    // The whole reason inserting a key is safe mid-edit: its value is SAMPLED from the track it is
    // inserted into, so the pose at every OTHER instant -- on the segment it split and on segments it
    // did not touch -- must read back identical to what the track already gave before the insert.
    {
        fmt::OcTrack before = makeTRTrack();
        std::vector<f32> onSplitSegment, onUntouchedSegment, atInsertInstant;
        trackSampleAt(before, 0.25f, onSplitSegment);      // inside [0,1], about to be split
        trackSampleAt(before, 1.75f, onUntouchedSegment);  // inside [1,2], never touched by this insert
        trackSampleAt(before, 0.5f, atInsertInstant);      // exactly where the new key will land

        fmt::OcTrack after = makeTRTrack();
        trackInsertKey(after, 0.5f);

        std::vector<f32> afterSplit, afterUntouched;
        trackSampleAt(after, 0.25f, afterSplit);
        trackSampleAt(after, 1.75f, afterUntouched);
        check(vecNear(onSplitSegment, afterSplit),
              "Linear is exact by construction: a key added exactly on the line does not move the line "
              "anywhere else on the segment it split");
        check(vecNear(onUntouchedSegment, afterUntouched),
              "and a segment the insert never touched reads back identical too");

        f32 v = 0.0f;
        check(trackReadComponent(after, 1, fmt::kOcChannelTranslation, 0, TrackKeySlot::Value, v) &&
                  near(v, atInsertInstant[0]),
              "the inserted key's own value is exactly what the track already sampled at that instant -- "
              "the property that makes the insert a no-op on the pose at its own new key, too");
    }

    // ---- CubicSpline insert/delete: the stride TRIPLES, and the tangent math has to be exact --------
    {
        fmt::OcTrack t = makeCubicRotTrack();
        std::vector<f32> beforeElsewhere;
        trackSampleAt(t, 0.5f, beforeElsewhere);  // inside [0,2], about to become inside [0,1] post-split

        const usize insertedAt = trackInsertKey(t, 1.0f);
        check(insertedAt == 1, "t=1 is the midpoint of the only span this track has");
        check(t.valid(), "OcTrack::valid() holds after a CubicSpline insert, tripled stride included");
        check(t.times.size() == 3 && t.values.size() == 3 * 12,
              "3 keys at 12 floats each -- CubicSpline's tripled stride, not the Linear 4");

        // HAND-DERIVED GOLDENS (see this file's own comment above makeCubicRotTrack): at alpha=0.5,
        // dt=2, only component 2 (v0=0,b0=0.25,v1=1,a1=-0.25) and component 3 (v0=1, everything else 0
        // on that component) are non-zero; components 0 and 1 stay exactly zero throughout the file,
        // so any non-zero reading there is stride bleed from a neighbouring component or channel.
        f32 v = 0.0f;
        check(trackReadComponent(t, insertedAt, fmt::kOcChannelRotation, 0, TrackKeySlot::Value, v) && near(v, 0.0f),
              "component 0 carries no data anywhere in this track and must stay exactly zero at the new key");
        check(trackReadComponent(t, insertedAt, fmt::kOcChannelRotation, 1, TrackKeySlot::Value, v) && near(v, 0.0f),
              "component 1 likewise");
        check(trackReadComponent(t, insertedAt, fmt::kOcChannelRotation, 2, TrackKeySlot::Value, v) &&
                  near(v, 0.625f),
              "component 2's split VALUE matches the hand-worked Hermite result at s=0.5, dt=2");
        check(trackReadComponent(t, insertedAt, fmt::kOcChannelRotation, 2, TrackKeySlot::InTangent, v) &&
                  near(v, 0.75f),
              "component 2's IN-tangent matches the hand-worked Hermite velocity at the split point");
        check(trackReadComponent(t, insertedAt, fmt::kOcChannelRotation, 2, TrackKeySlot::OutTangent, v) &&
                  near(v, 0.75f),
              "and its OUT-tangent is the SAME slope on both sides -- splitting one segment must not "
              "introduce a kink at the seam");
        check(trackReadComponent(t, insertedAt, fmt::kOcChannelRotation, 3, TrackKeySlot::Value, v) && near(v, 0.5f),
              "component 3's split value (v0=1, everything else 0 on this component) is exactly 0.5");
        check(trackReadComponent(t, insertedAt, fmt::kOcChannelRotation, 3, TrackKeySlot::InTangent, v) &&
                  near(v, -0.75f),
              "component 3's tangent comes out negative -- a different sign than component 2's, which is "
              "exactly what would be lost if the stride math ever swapped the two components' data");

        // THE TWO ORIGINAL KEYS KEEP THE TANGENTS THEY ALREADY HAD -- see AnimEdit.hpp's own comment
        // on why re-deriving them would be both unnecessary and wrong.
        check(trackReadComponent(t, 0, fmt::kOcChannelRotation, 2, TrackKeySlot::OutTangent, v) && near(v, 0.25f),
              "key0's own out-tangent is untouched by a split that happens after it");
        check(trackReadComponent(t, 2, fmt::kOcChannelRotation, 2, TrackKeySlot::InTangent, v) && near(v, -0.25f),
              "key1 (now index 2)'s own in-tangent is untouched by a split that happens before it");

        // CONCATENATION IS EXACT: sampling the SAME instant on the now-split curve must agree with
        // sampling it on the original, unsplit one.
        std::vector<f32> afterElsewhere;
        trackSampleAt(t, 0.5f, afterElsewhere);
        check(vecNear(beforeElsewhere, afterElsewhere),
              "the two halves of a split CubicSpline segment concatenate back to the identical curve");

        // ROUND TRIP, same claim as the Linear case above: delete exactly the inserted key and the
        // CubicSpline track must be restored byte-for-byte, tripled stride included.
        fmt::OcTrack original = makeCubicRotTrack();
        check(trackDeleteKey(t, insertedAt), "delete the freshly inserted CubicSpline key");
        check(t.times == original.times && t.values == original.values,
              "round trip restores every one of the 24 floats exactly, not merely the right tripled count");
    }
    {
        // EXTENDING PAST THE TRACK'S OWN RANGE is documented as the one case that does NOT get the
        // pose-preservation guarantee for free: the new key's tangents are zero, and its value is a
        // flat clamp of the boundary key -- not an extrapolation.
        fmt::OcTrack t = makeCubicRotTrack();
        const usize insertedAt = trackInsertKey(t, -1.0f);
        check(insertedAt == 0, "a time before the first key sorts before it");
        check(t.valid(), "still valid after extending the track's own coverage");
        f32 v = 0.0f;
        check(trackReadComponent(t, 0, fmt::kOcChannelRotation, 3, TrackKeySlot::Value, v) && near(v, 1.0f),
              "the new key's value is a flat clamp of key0's own value (component 3 was 1.0 there)");
        check(trackReadComponent(t, 0, fmt::kOcChannelRotation, 2, TrackKeySlot::InTangent, v) && near(v, 0.0f) &&
                  trackReadComponent(t, 0, fmt::kOcChannelRotation, 2, TrackKeySlot::OutTangent, v) && near(v, 0.0f),
              "and BOTH its tangents are zero -- an eased entry, never a neighbour's slope inherited wholesale");
    }

    // ---- trackMoveKey: re-sorts past a neighbour, carries its data with it, both cases named --------
    {
        // CROSSES a neighbour: key0 (t=0) moved to 1.5, past the key now sitting at t=1.
        fmt::OcTrack t = makeTRTrack();
        const usize newIndex = trackMoveKey(t, 0, 1.5f);
        check(newIndex == 1 && newIndex != 0,
              "moving a key past a neighbour changes its index -- AnimEdit.hpp re-sorts rather than "
              "clamping the drag short of the neighbour");
        check(t.times.size() == 3 && t.times[0] == 1.0f && t.times[1] == 1.5f && t.times[2] == 2.0f,
              "times stay ascending after the move");
        f32 v = 0.0f;
        check(trackReadComponent(t, newIndex, fmt::kOcChannelTranslation, 0, TrackKeySlot::Value, v) && v == 0.0f,
              "the data that moved is BYTE-FOR-BYTE what used to sit at key0 (T.x=0), not resampled from "
              "wherever 1.5 now falls on the curve");
        check(trackReadComponent(t, newIndex, fmt::kOcChannelRotation, 3, TrackKeySlot::Value, v) && v == 1.0f,
              "including its own identity rotation.w=1, carried along rather than dropped");
        check(t.valid(), "still a valid track after crossing a neighbour");
    }
    {
        // DOES NOT cross a neighbour: key2 (t=2) moved to 1.8, still after key1 (t=1). The index this
        // lands at is the SAME index the key already had, in contrast with the crossing case above --
        // this is the "test says which" half of the requirement.
        fmt::OcTrack t = makeTRTrack();
        const usize newIndex = trackMoveKey(t, 2, 1.8f);
        check(newIndex == 2, "a move that stays after its remaining neighbour keeps the same index");
        check(t.times[2] == 1.8f, "and lands at the requested time");
    }
    {
        fmt::OcTrack t = makeTRTrack();
        check(trackMoveKey(t, 99, 1.0f) == kInvalidKeyIndex, "an out-of-range key index is refused");
        check(trackMoveKey(t, 0, std::numeric_limits<f32>::quiet_NaN()) == kInvalidKeyIndex,
              "a non-finite target time is refused rather than sorting NaN into the times array");
        check(t.times == makeTRTrack().times, "a refused move leaves the track completely untouched");
    }

    // ---- clip-level retiming: duration, tracks, notifies, notify DURATIONS and curves together ------

    // ---- clipMaxTimeReached: the safety net shiftClipTime leans on ---------------------------------
    {
        fmt::OcAnimation clip;
        fmt::OcTrack t;
        t.channels = fmt::kOcChannelTranslation;
        t.interp = fmt::OcInterp::Linear;
        t.times = {0.0f, 3.0f};
        t.values = {0, 0, 0, 1, 1, 1};
        clip.tracks.push_back(t);
        clip.notifies.push_back(fmt::OcNotify{2.0f, "Hit"});
        clip.notifyDurations = {4.0f};   // this notify's window ends at 6.0, past the track's own 3.0
        fmt::OcCurve c;
        c.name = "C";
        c.times = {1.0f};
        c.values = {0.0f};
        clip.curves.push_back(c);
        check(near(clipMaxTimeReached(clip), 6.0f),
              "the latest instant reached is the notify's END (time + duration), past every track key "
              "and every curve key -- not simply the largest of the three lists' own maxima taken alone");
    }

    // ---- scaleClipDuration: a retime moves duration, tracks, notifies AND curves together -----------
    {
        fmt::OcAnimation clip;
        clip.duration = 4.0f;
        fmt::OcTrack t;
        t.channels = fmt::kOcChannelTranslation;
        t.interp = fmt::OcInterp::Linear;
        t.times = {0.0f, 2.0f, 4.0f};
        t.values = {0, 0, 0,  1, 0, 0,  2, 0, 0};
        clip.tracks.push_back(t);
        // THE NOTIFY SITS EXACTLY ON THE MIDDLE KEY (t=2) -- the case the header's own comment on
        // scaleClipDuration exists to get right: a footstep notify that drifts off its key is worse
        // than one that never moved.
        clip.notifies.push_back(fmt::OcNotify{2.0f, "Footstep"});
        clip.notifyDurations = {0.5f};
        fmt::OcCurve c;
        c.name = "Reload";
        c.interp = fmt::OcInterp::Linear;
        c.times = {0.0f, 2.0f, 4.0f};
        c.values = {0.0f, 0.5f, 1.0f};
        clip.curves.push_back(c);

        check(scaleClipDuration(clip, 2.0f), "a factor of 2 doubles the whole timeline");
        check(near(clip.duration, 8.0f), "clip duration scales");
        check(near(clip.tracks[0].times[1], 4.0f), "the track's own middle key scales to the same factor");
        check(near(clip.notifies[0].time, 4.0f),
              "THE NOTIFY IS STILL EXACTLY ON THE KEY after the scale -- this is the bug the feature "
              "exists to not have, not merely 'both numbers changed'");
        check(near(clip.notifyDurations[0], 1.0f),
              "the notify's own state-window length is a SPAN and scales too, unlike under a shift");
        check(near(clip.curves[0].times[1], 4.0f), "the curve's key times scale by the same factor");
        check(vecNear(clip.curves[0].values, {0.0f, 0.5f, 1.0f}),
              "a curve's VALUES never move under a retime -- only when things happen, not what they are");
        check(vecNear(clip.tracks[0].values, {0, 0, 0, 1, 0, 0, 2, 0, 0}),
              "a Linear track's own values are untouched by a retime -- only its key TIMES move");
        check(clip.valid(), "the retimed clip is still a valid OcAnimation");
    }
    {
        // CUBICSPLINE TANGENTS RESCALE BY 1/factor, because they are per-second velocities -- see
        // hermiteVelocity's own comment for why. Only the in/out thirds move; the value third does not.
        fmt::OcAnimation clip;
        clip.duration = 2.0f;
        fmt::OcTrack t;
        t.channels = fmt::kOcChannelTranslation;
        t.interp = fmt::OcInterp::CubicSpline;
        t.times = {0.0f, 1.0f};
        t.values = {
            1, 2, 3,    10, 20, 30,   4, 5, 6,     // key0: in / value / out
            7, 8, 9,    40, 50, 60,   11, 12, 13,  // key1: in / value / out
        };
        clip.tracks.push_back(t);

        check(scaleClipDuration(clip, 2.0f), "scale the CubicSpline clip by 2");
        f32 v = 0.0f;
        check(trackReadComponent(clip.tracks[0], 0, fmt::kOcChannelTranslation, 0, TrackKeySlot::InTangent, v) &&
                  near(v, 0.5f),
              "key0's in-tangent.x (was 1) is halved, the inverse of the time scale");
        check(trackReadComponent(clip.tracks[0], 1, fmt::kOcChannelTranslation, 2, TrackKeySlot::OutTangent, v) &&
                  near(v, 6.5f),
              "key1's out-tangent.z (was 13) is halved too");
        check(trackReadComponent(clip.tracks[0], 0, fmt::kOcChannelTranslation, 0, TrackKeySlot::Value, v) &&
                  near(v, 10.0f),
              "key0's VALUE (was 10) is completely untouched -- only the tangent thirds move");
        check(trackReadComponent(clip.tracks[0], 1, fmt::kOcChannelTranslation, 1, TrackKeySlot::Value, v) &&
                  near(v, 50.0f),
              "key1's value likewise");
        check(near(clip.tracks[0].times[1], 2.0f), "and the key TIME itself still scales the ordinary way");
    }
    {
        // BAKEDUNIFORM's sampleRate moves the OPPOSITE way duration does, is rounded rather than
        // truncated, and is clamped to the format's own [1,65535] range.
        fmt::OcAnimation clip;
        clip.storage = fmt::OcAnimStorage::BakedUniform;
        clip.sampleRate = 30;
        check(scaleClipDuration(clip, 2.0f) && clip.sampleRate == 15,
              "doubling the timeline halves the sample rate: the same keys now span twice the time");

        fmt::OcAnimation clampHigh;
        clampHigh.storage = fmt::OcAnimStorage::BakedUniform;
        clampHigh.sampleRate = 1;
        check(scaleClipDuration(clampHigh, 0.00001f) && clampHigh.sampleRate == 65535,
              "a rescaled rate above the format's own 16-bit ceiling clamps to it rather than wrapping");

        fmt::OcAnimation clampLow;
        clampLow.storage = fmt::OcAnimStorage::BakedUniform;
        clampLow.sampleRate = 1;
        check(scaleClipDuration(clampLow, 1000.0f) && clampLow.sampleRate == 1,
              "a rescaled rate that rounds to zero clamps to 1 -- OcAnimation::valid() forbids a zero "
              "sample rate on baked-uniform storage, and a retimed clip that validated before must still "
              "validate after");
    }
    {
        fmt::OcAnimation clip;
        clip.duration = 4.0f;
        check(!scaleClipDuration(clip, 0.0f), "a zero factor is refused -- it would collapse every key onto one instant");
        check(!scaleClipDuration(clip, -1.0f), "a negative factor is refused -- it would reverse key order");
        check(!scaleClipDuration(clip, std::numeric_limits<f32>::quiet_NaN()), "a non-finite factor is refused");
        check(near(clip.duration, 4.0f), "every refused call leaves the clip completely unchanged");
    }

    // ---- shiftClipTime: slides positions, NEVER durations, widens duration to cover what moved ------
    {
        fmt::OcAnimation clip;
        clip.duration = 3.0f;
        fmt::OcTrack t;
        t.channels = fmt::kOcChannelTranslation;
        t.interp = fmt::OcInterp::Linear;
        t.times = {1.0f, 2.0f, 3.0f};
        t.values = {0, 0, 0,  1, 1, 1,  2, 2, 2};
        clip.tracks.push_back(t);
        clip.notifies.push_back(fmt::OcNotify{2.0f, "Hit"});
        clip.notifyDurations = {0.5f};
        fmt::OcCurve c;
        c.name = "C";
        c.times = {1.0f, 2.0f, 3.0f};
        c.values = {0.0f, 1.0f, 2.0f};
        clip.curves.push_back(c);

        check(shiftClipTime(clip, 5.0f), "shift the whole clip 5 seconds later");
        check(near(clip.tracks[0].times[0], 6.0f) && near(clip.tracks[0].times[2], 8.0f), "track keys slide by delta");
        check(near(clip.notifies[0].time, 7.0f), "the notify slides too");
        check(near(clip.notifyDurations[0], 0.5f),
              "a notify DURATION IS A SPAN, NOT A POSITION -- sliding every timestamp later must not "
              "change how long a hit window stays open once it opens");
        check(near(clip.curves[0].times[0], 6.0f), "curve keys slide too");
        check(near(clip.duration, 8.0f), "duration widens by exactly delta when nothing already ran past it");
    }
    {
        // THE SAFETY NET: a track whose last key already sat past the clip's own duration BEFORE the
        // shift (a state OcAnimation::valid() does not forbid) must not have that key go unreachable
        // after a shift that only widened duration by `delta` and nothing more.
        fmt::OcAnimation clip;
        clip.duration = 2.0f;   // already inconsistent with the track below, on purpose
        fmt::OcTrack t;
        t.channels = fmt::kOcChannelTranslation;
        t.interp = fmt::OcInterp::Linear;
        t.times = {0.0f, 3.0f};
        t.values = {0, 0, 0,  1, 1, 1};
        clip.tracks.push_back(t);
        check(shiftClipTime(clip, -1.0f), "shift a second earlier");
        // Naive `duration + delta` gives 2 - 1 = 1.0, which would strand the track's own last key
        // (now at 2.0) past the clip's own reported end.
        check(near(clip.duration, 2.0f),
              "duration never shrinks below whatever the shifted content still reaches, even when the "
              "ordinary duration+delta arithmetic alone would have let it");
    }

    // ---- trimClip: boundary keys preserve the edge pose; notifies clamp; curves degrade to a hold ---
    {
        fmt::OcAnimation clip;
        clip.duration = 4.0f;
        fmt::OcTrack t;
        t.channels = fmt::kOcChannelTranslation;
        t.interp = fmt::OcInterp::Linear;
        t.times = {0.0f, 1.0f, 2.0f, 3.0f, 4.0f};
        t.values = {0, 0, 0,  1, 1, 1,  2, 2, 2,  3, 3, 3,  4, 4, 4};
        clip.tracks.push_back(t);
        // A notify state window [1.5, 2.5] that the trim below neither fully contains nor cuts through.
        clip.notifies.push_back(fmt::OcNotify{1.5f, "Hit"});
        clip.notifyDurations = {1.0f};
        fmt::OcCurve c;
        c.name = "C";
        c.times = {0.0f, 2.0f, 4.0f};
        c.values = {0.0f, 1.0f, 2.0f};
        clip.curves.push_back(c);

        check(trimClip(clip, 0.5f, 3.5f), "trim to [0.5, 3.5], where NEITHER cut lands on an existing key");
        check(near(clip.duration, 3.0f), "the trimmed duration is exactly end-start");
        check(clip.tracks[0].times.size() == 5,
              "two boundary keys were INSERTED where none existed, on top of the three originals kept");
        check(near(clip.tracks[0].times.front(), 0.0f) && near(clip.tracks[0].times.back(), 3.0f),
              "re-anchored to zero: the trimmed span now runs [0, 3]");

        f32 v = 0.0f;
        check(trackReadComponent(clip.tracks[0], 0, fmt::kOcChannelTranslation, 0, TrackKeySlot::Value, v) &&
                  near(v, 0.5f),
              "the FIRST boundary key holds the INTERPOLATED pose the cut point had (halfway between the "
              "original 0 and 1), not a snap to whichever original key happened to survive -- without this "
              "the trimmed clip would visibly jump on its very first frame");
        check(trackReadComponent(clip.tracks[0], clip.tracks[0].times.size() - 1, fmt::kOcChannelTranslation, 0,
                                  TrackKeySlot::Value, v) &&
                  near(v, 3.5f),
              "the SAME guarantee holds at the other cut edge");

        check(clip.notifies.size() == 1 && near(clip.notifies[0].time, 1.0f),
              "the notify inside the trim survives, re-anchored to the new zero");
        check(near(clip.notifyDurations[0], 1.0f),
              "its window length is untouched -- the cut landed outside [1.5,2.5], so nothing needed clamping");

        check(clip.curves[0].times.size() == 1 && near(clip.curves[0].times[0], 1.5f) &&
                  near(clip.curves[0].values[0], 1.0f),
              "only the one curve key inside [0.5,3.5] survives, re-anchored -- curves degrade to a flat "
              "hold at their nearest surviving key rather than getting a track's boundary-key treatment");
    }
    {
        // A NOTIFY WINDOW THE CUT LANDS INSIDE OF must have its OWN length clamped so it cannot reach
        // past the new, shorter duration -- this is the one case scaleClipDuration's "duration and the
        // window grow together" reasoning does not apply to, because a trim can genuinely shorten a
        // window the cut lands inside of.
        fmt::OcAnimation clip;
        clip.duration = 4.0f;
        fmt::OcTrack t;
        t.channels = fmt::kOcChannelTranslation;
        t.interp = fmt::OcInterp::Linear;
        t.times = {0.0f, 4.0f};
        t.values = {0, 0, 0,  1, 1, 1};
        clip.tracks.push_back(t);
        clip.notifies.push_back(fmt::OcNotify{1.0f, "Hit"});
        clip.notifyDurations = {3.0f};   // window would otherwise reach to 4.0
        check(trimClip(clip, 0.0f, 2.0f), "trim to [0,2], cutting the notify's own window off at 2.0");
        check(clip.notifies.size() == 1, "the notify itself still starts inside the kept range");
        check(near(clip.notifyDurations[0], 1.0f),
              "its window is clamped so it ends exactly at the new duration (kept time 1.0 + window 1.0 "
              "= 2.0), not left reaching past a clip that no longer has that much time left in it");
    }
    {
        fmt::OcAnimation clip;
        clip.duration = 4.0f;
        fmt::OcTrack t;
        t.channels = fmt::kOcChannelTranslation;
        t.interp = fmt::OcInterp::Linear;
        t.times = {0.0f, 4.0f};
        t.values = {0, 0, 0,  1, 1, 1};
        clip.tracks.push_back(t);
        check(!trimClip(clip, 2.0f, 2.0f), "a zero-width range leaves nothing to keep, and is refused");
        check(!trimClip(clip, 3.0f, 1.0f), "an inverted range is refused the same way");
    }

    // ---- the bone palette: forks split chains, chains get distinct hues, depth shades within one ----
    //
    // A small biped-shaped rig with a REAL fork below the root: Hips(0) forks into Spine(1),
    // LeftThigh(2) and RightThigh(3); Spine(1) has one child Chest(4); Chest(4) FORKS AGAIN into
    // LeftShoulder(5) and RightShoulder(6); LeftShoulder continues down one unforked arm chain
    // UpperArm(7)->Forearm(8)->Hand(9). The naive "only children of the single skeleton root start a
    // new chain" reading would put Chest, both shoulders and the whole arm chain into ONE "spine"
    // colour -- exactly the "one grey mass" AnimEdit.hpp's own top comment names as the bug this rule
    // exists to avoid.
    {
        fmt::OcSkeleton skeleton;
        auto addBone = [&](i32 parent) {
            fmt::OcBone b;
            b.parent = parent;
            skeleton.bones.push_back(b);
        };
        addBone(fmt::kOcBoneNoParent);  // 0 Hips (the skeleton's own root)
        addBone(0);                     // 1 Spine
        addBone(0);                     // 2 LeftThigh
        addBone(0);                     // 3 RightThigh
        addBone(1);                     // 4 Chest
        addBone(4);                     // 5 LeftShoulder
        addBone(4);                     // 6 RightShoulder
        addBone(5);                     // 7 LeftUpperArm
        addBone(7);                     // 8 LeftForearm
        addBone(8);                     // 9 LeftHand
        check(skeleton.valid(), "the fixture rig is itself a valid OcSkeleton");

        std::vector<u32> counts;
        boneChildCounts(skeleton, counts);
        check(counts[0] == 3, "Hips forks three ways: Spine, LeftThigh, RightThigh");
        check(counts[1] == 1, "Spine has exactly one child -- Chest -- so it does not fork");
        check(counts[4] == 2, "Chest forks two ways: LeftShoulder, RightShoulder");
        check(counts[7] == 1 && counts[8] == 1 && counts[9] == 0, "the arm chain below the shoulder never forks");

        check(boneChainRoot(skeleton, 1) == 1,
              "Spine is a chain ROOT: its own parent is the skeleton root, which always starts a new chain");
        check(boneChainRoot(skeleton, 4) == 1 && boneChainDepth(skeleton, 4) == 1,
              "Chest does NOT start a new chain -- Spine has only one child, so Chest continues Spine's "
              "chain one step deeper");
        check(boneChainRoot(skeleton, 5) == 5 && boneChainDepth(skeleton, 5) == 0,
              "LeftShoulder DOES start its own new chain, because its parent (Chest) forks -- this is "
              "the exact case a naive 'only direct children of the skeleton root' rule would get wrong");
        check(boneChainRoot(skeleton, 6) == 6, "RightShoulder starts its OWN chain too, separate from the left");
        check(boneChainRoot(skeleton, 9) == 5 && boneChainDepth(skeleton, 9) == 3,
              "LeftHand still belongs to the LeftShoulder chain, three unforked steps deeper");

        u32 root = 0, depth = 0;
        boneChainRootAndDepth(skeleton, 7, counts, root, depth);
        check(root == 5 && depth == 1, "the two-argument and five-argument forms of the walk agree");

        const std::vector<BoneColor> paletteA = computeBonePalette(skeleton);
        const std::vector<BoneColor> paletteB = computeBonePalette(skeleton);
        check(paletteA.size() == skeleton.bones.size(), "one colour per bone, in bone order");
        bool deterministic = true;
        for (usize i = 0; i < paletteA.size(); ++i)
            if (paletteA[i].r != paletteB[i].r || paletteA[i].g != paletteB[i].g || paletteA[i].b != paletteB[i].b)
                deterministic = false;
        check(deterministic, "the SAME rig produces the IDENTICAL palette on a second call, every time");

        const BoneColor leftShoulder = boneColorFor(skeleton, 5);
        check(near(leftShoulder.r, paletteA[5].r) && near(leftShoulder.g, paletteA[5].g) &&
                  near(leftShoulder.b, paletteA[5].b),
              "the per-bone call and the bulk palette call agree on the same bone's colour");

        // TWO DIFFERENT CHAINS: the left and right shoulders, adjacent bone indices with adjacent chain
        // roots, must still read as CLEARLY different colours -- the golden-angle hue spacing is the
        // whole point of kBoneHueGoldenTurns.
        const f32 crossChainDistance = colorDistance(paletteA[5], paletteA[6]);
        check(crossChainDistance > 0.3f,
              "the left-arm chain and the right-arm chain are clearly different colours -- a human "
              "looking at the two arms must be able to tell them apart at a glance");

        // ONE CHAIN, DIFFERENT DEPTHS: the shoulder and the hand, three joints apart on the SAME chain,
        // must still be visibly different shades (the lightness triangle-wave), but LESS different from
        // each other than two entirely separate chains are -- chain identity should dominate over depth.
        const f32 withinChainDistance = colorDistance(paletteA[5], paletteA[9]);
        check(withinChainDistance > 0.05f,
              "two joints of the same limb still read as visibly different shades, not one flat colour "
              "for the whole arm");
        check(withinChainDistance < crossChainDistance,
              "but two joints of the SAME limb are still closer to each other than two DIFFERENT limbs "
              "are -- a person should read chain identity before depth");
    }

    // ---- hslToBoneColor: a few known conversions, pinned independently of anything about bones -----
    {
        BoneColor c = hslToBoneColor(0.0f, 1.0f, 0.5f);
        check(near(c.r, 1.0f) && near(c.g, 0.0f) && near(c.b, 0.0f), "hue 0, full saturation: pure red");
        c = hslToBoneColor(120.0f, 1.0f, 0.5f);
        check(near(c.r, 0.0f) && near(c.g, 1.0f) && near(c.b, 0.0f), "hue 120: pure green");
        c = hslToBoneColor(240.0f, 1.0f, 0.5f);
        check(near(c.r, 0.0f) && near(c.g, 0.0f) && near(c.b, 1.0f), "hue 240: pure blue");
        c = hslToBoneColor(0.0f, 0.0f, 0.5f);
        check(near(c.r, 0.5f) && near(c.g, 0.5f) && near(c.b, 0.5f), "zero saturation is grey regardless of hue");
        c = hslToBoneColor(0.0f, 1.0f, 0.0f);
        check(near(c.r, 0.0f) && near(c.g, 0.0f) && near(c.b, 0.0f), "zero lightness is black regardless of hue");
        c = hslToBoneColor(0.0f, 1.0f, 1.0f);
        check(near(c.r, 1.0f) && near(c.g, 1.0f) && near(c.b, 1.0f), "full lightness is white regardless of hue");
        // A HUE OUTSIDE [0,360) must wrap, not clamp or produce a NaN from an unguarded negative fmod.
        c = hslToBoneColor(-360.0f, 1.0f, 0.5f);
        check(near(c.r, 1.0f) && near(c.g, 0.0f) && near(c.b, 0.0f), "a negative hue wraps to the same colour as its positive equivalent");
    }

    std::printf("[INFO ] === %d assertions, %d failed ===\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
