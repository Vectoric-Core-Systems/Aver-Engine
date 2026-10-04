// WHAT THE USER ASKED AverSR TO DO, AS OPPOSED TO WHAT IT IS DOING RIGHT NOW. optimisation-wave-2's
// U2 gives AverSR a per-rung default (QualityLadder.hpp's ladder::averSrLevel) that a CLI flag, a
// project manifest and the user's own Display preference can each outrank (Scalability.hpp's
// resolveAverSrLevel is that precedence chain) -- but "the user's own Display preference" is not
// simply one of the four sr::Quality levels: it also has to remember Auto (follow whatever the
// resolved chain says today, which can change as a project's Overall rung changes) and Manual (the
// user dragged the Render Scale slider to a number no named level produces), neither of which
// aver::sr::Quality itself has room to express. AverSrChoice is that superset.
//
// A PURE FUNCTION HEADER, DELIBERATELY, for InputOwnership.hpp's exact reason (see that header's
// own top comment): no ImGui, no SandboxApp state, no AVER_WARN call, no globals, no Engine& -- plain
// values in, plain values out. That is what makes the migration rule and the two small derived
// questions below (userLevelFor, renderScaleToPersist) headless unit tests
// (tests/editor/src/AverSrChoiceTest.cpp) in a codebase where almost nothing about the editor can be
// tested at all. Every ImGui call, every editor::prefString/setPrefString call and every AVER_INFO/
// AVER_CRITICAL stays in SandboxApp.cpp; only the DECISIONS move here.
#pragma once
#include "aver/core/Types.hpp"

#include <string_view>

namespace aver::editor {

// Auto and Manual are the two states aver::sr::Quality cannot represent on its own -- see this
// header's own top comment. Off/Quality/Balanced/Performance deliberately share aver::sr::Quality's
// OWN numbering plus one (their AverSrChoice value is userLevelFor's result plus 2 -- see that
// function below), so a future level added to one enum is not silently misread as a different level
// on the other; nothing here casts between the two numberings directly.
enum class AverSrChoice : u32 { Auto, Off, Quality, Balanced, Performance, Manual };

// Case-insensitive, matching every other name parser in this codebase (EditorConsole.hpp's
// parseQuality, aver::sr::parseQuality). Never partial: "qual" does not match "quality".
inline bool parseAverSrChoice(std::string_view tok, AverSrChoice& out) {
    // No <cctype>/<algorithm> dependency: six short ASCII names, checked by hand the same way
    // InputOwnership.hpp's neighbours in this directory keep their own parsers dependency-light.
    auto ciEquals = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            char ca = a[i], cb = b[i];
            if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
            if (ca != cb) return false;
        }
        return true;
    };
    if (ciEquals(tok, "auto"))        { out = AverSrChoice::Auto;        return true; }
    if (ciEquals(tok, "off"))         { out = AverSrChoice::Off;         return true; }
    if (ciEquals(tok, "quality"))     { out = AverSrChoice::Quality;     return true; }
    if (ciEquals(tok, "balanced"))    { out = AverSrChoice::Balanced;    return true; }
    if (ciEquals(tok, "performance")) { out = AverSrChoice::Performance; return true; }
    if (ciEquals(tok, "manual"))      { out = AverSrChoice::Manual;      return true; }
    return false;
}

inline const char* averSrChoiceName(AverSrChoice c) {
    switch (c) {
        case AverSrChoice::Auto:        return "Auto";
        case AverSrChoice::Off:         return "Off";
        case AverSrChoice::Quality:     return "Quality";
        case AverSrChoice::Balanced:    return "Balanced";
        case AverSrChoice::Performance: return "Performance";
        case AverSrChoice::Manual:      return "Manual";
    }
    return "?";
}

// THE ONE-TIME MIGRATION (3.3 A), off the two pre-Auto prefs this build already wrote every session
// (display.aversr, a raw 0..3 float mirroring the live sr::Quality; display.renderScale, the live
// device render scale): a person who never touched a fresh editor.ini gets Auto, matching U2's own
// "AverSR is on by default at every Overall rung" decision; a person who HAD picked a named level
// keeps exactly that level, explicitly, rather than being silently defaulted into Auto's own
// per-rung table; a person who had only dragged the Render Scale slider (so the level mirror never
// moved off its own default, but the scale itself is not 1) keeps that as Manual, not as a rounded-off
// named level nobody chose.
//
// `hasChoiceKey`/`choice` are ALWAYS the first thing consulted, and win outright the moment the key is
// present -- migration only ever runs on the ABSENT-key path, so a session that has already written
// display.aversrChoice once (Auto included) never re-derives it from the two legacy floats again, even
// if those floats keep being written every session as informational mirrors (3.3 A's own "still
// written" rule). A key present but unparsable (hand-edited, or written by a future build with a
// seventh choice this one does not know) reads as Auto rather than crashing this function's caller
// into guessing -- Auto is always a safe, working choice, never a silent Off.
inline AverSrChoice migrateAverSrChoice(bool hasChoiceKey, std::string_view choice,
                                        f32 legacyLevel, f32 legacyRenderScale) {
    if (hasChoiceKey) {
        AverSrChoice c;
        if (parseAverSrChoice(choice, c)) return c;
        return AverSrChoice::Auto;   // a garbage key never crashes the caller into guessing
    }
    // display.aversr was written from static_cast<int>(sr::Quality) every session (SandboxApp.cpp's
    // saveEditorPreferences, pre-Auto) -- 0 is indistinguishable from "never touched" (see this
    // project's optimisation-wave-2 plan, 10.1 item 1, for why 0 must NOT be read as an explicit
    // Off), so only a level ABOVE Off (> 0.5, clear of float noise) counts as an explicit prior
    // choice here.
    if (legacyLevel > 0.5f) {
        const int lvl = legacyLevel < 1.0f ? 1 : (legacyLevel > 3.0f ? 3 : static_cast<int>(legacyLevel));
        switch (lvl) {
            case 1:  return AverSrChoice::Quality;
            case 2:  return AverSrChoice::Balanced;
            default: return AverSrChoice::Performance;
        }
    }
    if (legacyRenderScale != 1.0f) return AverSrChoice::Manual;
    return AverSrChoice::Auto;
}

// -1 for the two choices with no single sr::Quality level of their own (Auto's level is resolved
// per-frame from the rung/manifest/ladder chain; Manual has no NAMED level at all, only a raw scale)
// -- the exact "-1 = absent" sentinel resolveAverSrLevel's own userLevel parameter expects
// (Scalability.hpp), so this function's result is fed there with no translation at the call site.
// Off/Quality/Balanced/Performance return aver::sr::Quality's OWN numbering (0..3) byte for byte --
// see this header's own top comment on why the two enums still never cast into one another directly.
inline int userLevelFor(AverSrChoice c) {
    switch (c) {
        case AverSrChoice::Off:         return 0;
        case AverSrChoice::Quality:     return 1;
        case AverSrChoice::Balanced:    return 2;
        case AverSrChoice::Performance: return 3;
        default:                        return -1;   // Auto, Manual
    }
}

// display.renderScale is a MIRROR, not a controller (loading it back only ever matters for Manual --
// every named level re-derives its own canonical scale through aver::sr::renderScaleFor on load, see
// SandboxApp.cpp's own load comment). Persisting the live scale for a named level or for Auto would
// have exactly the bug this project's optimisation-wave-2 plan's 10.1/section-3.3-A corrections
// describe: Auto would persist whatever fraction the rung it landed on that session happened to
// resolve to, and the NEXT session's crash-cookie render-scale block would apply that stale fraction
// BEFORE Auto ever got a chance to re-derive it from that session's own (possibly different) rung.
inline f32 renderScaleToPersist(AverSrChoice c, f32 liveScale) {
    return c == AverSrChoice::Manual ? liveScale : 1.0f;
}

} // namespace aver::editor
