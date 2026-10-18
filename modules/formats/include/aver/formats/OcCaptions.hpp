#pragma once
// .occaptions: subtitles and sound captions as data. A cue names the sounds that raise it, the speaker, and timed
// lines; text and names are localization references. Text only, no engine dependency, in the base Aver.Formats
// target. docs/ACCESSIBILITY.md, docs/formats/FORMAT_SPECS.md.
//
//   OCCAPTIONS 1
//   SPEAKER guard name "@Cap:spk.guard=Guard" colour #7DD3FC
//   CUE intro_01 sound "Audio/VO/intro_01.ocaudio" kind dialogue speaker guard priority 8
//     LINE 0.0 2.4 "@Cap:intro_01_a=Welcome to the station."
//     LINE 2.6 0   "@Cap:intro_01_b=Keep your voice down."      # dur 0 = until the voice ends
//   CUE boom sound "Audio/SFX/explosion_*.ocaudio" kind sound priority 5 radius 4000 cooldown 1.5 hold 1.8 tag "@Cap:fx.explosion=[explosion]"
//
// A malformed record is skipped with a diagnostic naming the line (a typo never blanks a file). Only a missing or
// newer header fails the parse. Unknown records and comments survive writeOcCaptions().
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt {

inline constexpr int kOcCaptionsVersion = 1;

enum class OcCaptionKind : i32 { Dialogue = 0, Sound = 1, Music = 2 };

struct OcCaptionSpeaker {
    std::string id, name;          // name: text or reference
    u32 rgba = 0xFFFFFFFFu;        // 0xRRGGBBAA
};

struct OcCaptionLine {
    f32 start = 0, dur = 0;        // seconds from the cue start; dur 0 = until the voice ends (or the hold time)
    std::string text;              // text or reference
    std::string speaker;           // overrides the cue's speaker when set
};

struct OcCaptionCue {
    std::string id;
    std::vector<std::string> sounds;   // patterns, content-relative
    OcCaptionKind kind = OcCaptionKind::Sound;
    std::string speaker;
    i32 priority = 0;
    f32 radius = 0;                // cm; 0 = always shown
    f32 cooldown = 1.0f;           // s, per cue id
    f32 hold = -1.0f;              // minimum on-screen seconds; < 0 = the kind's default (see captionCueHold)
    std::string tag;               // shorthand for one LINE 0 0 <tag>
    std::vector<OcCaptionLine> lines;
};

struct OcCaptionData {
    int version = 1;
    std::vector<OcCaptionSpeaker> speakers;
    std::vector<OcCaptionCue> cues;    // file order; duplicate ids are kept as authored (the loader's rule replaces)
};

// False only for a missing or unsupported header (err set). Per-record problems go to *diag and the record is skipped.
bool parseOcCaptions(std::string_view text, OcCaptionData& out, std::vector<std::string>* diag = nullptr,
                     std::string* err = nullptr);
bool loadOcCaptions(const std::string& path, OcCaptionData& out, std::vector<std::string>* diag = nullptr,
                    std::string* err = nullptr);

// Record-preserving merge with `existing` (the writeOcinput arrangement): records still present are rewritten only
// when they changed, vanished ones are dropped, comments, blanks and unknown records stay put, new records append.
// Empty `existing`: a fresh file.
std::string writeOcCaptions(const OcCaptionData& d, std::string_view existing = "");

// The cue's lines: the tag as one LINE 0 0 when set, then the explicit LINEs.
std::vector<OcCaptionLine> captionCueLines(const OcCaptionCue& c);
// 1.5 s for sound and music, 0.8 s for dialogue, unless the cue sets one.
f32 captionCueHold(const OcCaptionCue& c);

// Pattern against a played sound path: both lowered, '\' as '/', '*' and '?' inside a segment; the pattern is matched
// against the TAIL of the path (as many trailing segments as the pattern has).
bool captionPatternMatches(std::string_view pattern, std::string_view path);
// Exact (whole normalised path equal, no wildcard involved).
bool captionPatternExact(std::string_view pattern, std::string_view path);

// "#RRGGBB" or "#RRGGBBAA" -> 0xRRGGBBAA. False otherwise.
bool parseCaptionColour(std::string_view s, u32& rgba);
std::string formatCaptionColour(u32 rgba);

} // namespace aver::fmt
