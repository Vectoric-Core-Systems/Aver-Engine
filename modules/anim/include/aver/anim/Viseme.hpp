#pragma once
// .ocvis -- an authored lip-sync track (timed phoneme or viseme events), the one envelope function the
// runtime player and the baker share, and bakeVisemeTrack, which turns a track into an ordinary curve-only
// .ocanim so montages, state machines and sequencer animation tracks can play speech with no new playback
// code. No speech recognition here. Design: docs/plans/wave9/MORPH_TARGETS.md 2.6.
//
//   OCVIS 1
//   SET phoneme            # phoneme | viseme: which symbol set EVENT lines use (default phoneme)
//   EVENT 0.00 0.12 sil
//   EVENT 0.12 0.31 HH 0.8 # start end symbol [intensity 0..1, default 1]
//
// Events need not be sorted (they are sorted on load); end > start; at most 65536 events; unknown records
// are ignored; '#' starts a comment.
#include "aver/anim/FaceRig.hpp"
#include "aver/formats/OcAnim.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::anim {

inline constexpr int kVisemeTrackVersion = 1;
inline constexpr usize kVisemeMaxEvents = 65536;

enum class VisemeSet : u8 { Phoneme = 0, Viseme = 1 };

struct VisemeEvent {
    f32 start = 0.0f, end = 0.0f;
    std::string symbol;            // as authored: a phoneme (HH) or a viseme (viseme_aa, aa)
    f32 intensity = 1.0f;
    // Resolved by resolveVisemeEvents, never serialised: the viseme pose id (fnv1a64 of "viseme_aa").
    // 0 = unresolved or unknown symbol, and evalVisemeWeights skips it.
    u64 pose = 0;
};

struct VisemeTrack {
    VisemeSet set = VisemeSet::Phoneme;
    std::vector<VisemeEvent> events;     // ascending by start

    f32 length() const;                  // the last event's end, 0 when empty
    bool valid(std::string* why = nullptr) const;
};

std::string writeVisemeTrack(const VisemeTrack& t);
// Sorts the events. A malformed EVENT line is skipped and listed in *diag; only a missing or newer header,
// or more than kVisemeMaxEvents events, fails.
bool parseVisemeTrack(std::string_view text, VisemeTrack& out, std::vector<std::string>* diag = nullptr,
                      std::string* why = nullptr);
bool saveVisemeTrack(const std::string& path, const VisemeTrack& t, std::string* why = nullptr);
bool loadVisemeTrack(const std::string& path, VisemeTrack& out, std::vector<std::string>* diag = nullptr,
                     std::string* why = nullptr);

// Fills each event's `pose` from its symbol (the rig's phoneme overrides, ARPAbet, a viseme name). An
// unknown symbol leaves it 0 and is listed once in *unknown. `visemeOnly` = the track's SET viseme.
u32 resolveVisemeEvents(std::vector<VisemeEvent>& events, const FaceRigAsset* rig, bool visemeOnly,
                        std::vector<std::string>* unknown = nullptr);

// One viseme pose weight.
struct VisemeSample { u64 pose; f32 weight; };

// The lip-sync envelope at time t (seconds), the single function the runtime player and the baker both call.
// For an event [s, e] with e' = max(e, s + minHold):
//   rise = smoothstep((t - (s - lead)) / (lead + attack)),  fall = 1 - smoothstep((t - e') / release),
//   weight = clamp01(intensity * rise * fall).
// Per viseme pose the largest weight over the events wins; when the sum exceeds maxTotal every weight is scaled
// down so it equals maxTotal. `out` is replaced, sorted by pose id, and holds only weights above zero.
void evalVisemeWeights(const std::vector<VisemeEvent>& events, const LipSyncParams& params, f32 t,
                       std::vector<VisemeSample>& out);

// Samples a track into pose.<viseme> Linear curves at `fps`: duration = the last event's end + release, a
// curve-only clip (no tracks, flags 0). Keys a straight line between their neighbours reproduces within 1e-3
// are dropped. `rig` supplies phoneme overrides and the lip-sync parameters (null = built-in).
fmt::OcAnimation bakeVisemeTrack(const VisemeTrack& track, const FaceRigAsset* rig, f32 fps = 30.0f);

} // namespace aver::anim
