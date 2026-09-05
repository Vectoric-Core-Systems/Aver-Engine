// .ocinput reader and writer. See OcInput.hpp for the round-trip contract, why key names are
// unvalidated opaque tokens, and the exact EnhancedInput.cs lines each field mirrors.
//
// THE GRAMMAR, IN FULL (one record per line; `#` starts a comment; one trailing `;` tolerated,
// matching every other OC-dialect text format in this tree -- see TextScan.hpp):
//
//   OCINPUT 1                                     -- header, required, exactly once, version 1
//   NAME <text to end of line>                    -- optional authoring label
//   ACTION <name> digital|axis1|axis2             -- declares one action; OcInputValueType
//   BIND <action> key <KeyName> [scale <f32>] [component x|y|z]
//   BIND <action> mousex        [scale <f32>] [component x|y|z]
//   BIND <action> mousey        [scale <f32>] [component x|y|z]
//   BIND <action> wheel         [scale <f32>] [component x|y|z]
//                                                  -- one binding, feeding a declared ACTION by name
//   CONTEXT <name> priority <i32>                 -- optional; this scheme's runtime name + AddContext priority
//
// `scale` defaults to 1.0 and `component` to x (0) when omitted -- BindKey's own defaults
// (EnhancedInput.cs:104) -- and writeOcinput omits either segment whenever it holds that default,
// the same sparse-write convention .ocmat's optional PARAMs and .ocparticle's NAME/TEX use: a
// hand-authored file only spells out what departs from the ordinary case.
//
// A COMPLETE WORKED EXAMPLE (a first-person "on foot" scheme: WASD-style forward/back on one axis
// component, a mouse-button Fire, registered at priority 0):
//
//   OCINPUT 1
//   NAME DefaultControls
//
//   ACTION Move axis2
//   ACTION Fire digital
//
//   BIND Move key W component y
//   BIND Move key S scale -1 component y
//   BIND Fire key MouseLeft
//
//   CONTEXT OnFoot priority 0
//
// WHY BIND NAMES ITS ACTION EXPLICITLY, RATHER THAN "THE MOST RECENTLY DECLARED ACTION". An earlier
// shape of this format had BIND implicitly belong to whichever ACTION line preceded it, the way
// .ocbeam's MATERIAL/NODE/BEAM sections work by StartsWith-mode (recon `ocbeam`, see
// docs/formats/FORMAT_SPECS.md 4.1). That makes a BIND's meaning depend on FILE ORDER, so moving one
// line -- or an editor re-serialising records in a different order -- silently rewires which action a
// binding feeds. OcGraph.hpp's PIN record avoids exactly this by naming its node explicitly instead
// of meaning "the previous NODE line" (`PIN <nodeId> <pinName> ...`); BIND follows PIN's precedent
// instead of .ocbeam's, and as a consequence a BIND is also allowed to FORWARD-reference an ACTION
// declared later in the file -- the same tolerance OcGraph.hpp documents for LINK/ENTRY -- because
// nothing about this grammar needs "declared before used" once names, not position, are the link.
#include "aver/formats/OcInput.hpp"
#include "aver/formats/detail/TextScan.hpp"
#include "aver/platform/FileSystem.hpp"

#include <charconv>
#include <cstdio>
#include <vector>

namespace aver::fmt {
using namespace aver::fmt::detail;

namespace {

// Formats a number for the text form: enough digits to round-trip a typical authored value, no
// trailing noise. Identical to OcParticle.cpp/OcMat.cpp/OcGraph.cpp's own num(), for the same reason
// stated there: the writer's output has to be something a human can read AND something the strict
// reader below can reparse back to (within a float's own precision) the value that produced it.
std::string num(f32 v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.6g", static_cast<f64>(v));
    return buf;
}

// STRICT parsing helpers, identical in shape to OcParticle.cpp's strictF32/strictI32 (see that
// file's header comment for why this module's numeric parsing is sometimes strict and sometimes
// tolerant): the whole token must be a valid number, or the caller's field is left untouched and the
// record is rejected. This format has no hand-edited legacy history to be lenient toward -- like
// .ocparticle, it is authored fresh by a human or an editor, so a mistyped number should fail loudly
// rather than silently becoming 0.
bool strictF32(std::string_view s, f32& out) {
    s = trim(s);
    if (s.empty()) return false;
    f64 v{};
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return false;
    out = static_cast<f32>(v);
    return true;
}
bool strictI32(std::string_view s, i32& out) {
    s = trim(s);
    if (s.empty()) return false;
    i32 v{};
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return false;
    out = v;
    return true;
}

const char* valueTypeWord(OcInputValueType t) {
    switch (t) {
        case OcInputValueType::Axis1D:  return "axis1";
        case OcInputValueType::Axis2D:  return "axis2";
        case OcInputValueType::Digital: default: return "digital";
    }
}
const char* sourceWord(OcInputSource s) {
    switch (s) {
        case OcInputSource::MouseX:     return "mousex";
        case OcInputSource::MouseY:     return "mousey";
        case OcInputSource::MouseWheel: return "wheel";
        case OcInputSource::Key:        default: return "key";
    }
}
// 0=X 1=Y 2=Z (OcInputBinding::component's own comment). Any OTHER in-memory value -- reachable only
// by code constructing a binding by hand, never by this parser -- is written as "x" defensively, the
// same default-case fallback valueTypeWord/sourceWord use for a C++ enum value that cannot actually
// occur through parsing but a raw int field technically permits.
const char* componentWord(i32 c) {
    switch (c) {
        case 1: return "y";
        case 2: return "z";
        default: return "x";
    }
}

// Renders one binding/action as its owned line. `scale`/`component` are omitted from a BIND line
// when they hold BindKey's own defaults (1.0 / X) -- see this file's header comment for why a
// hand-authored file should not have to spell out the ordinary case.
std::string bindLine(const OcInputBinding& b) {
    std::string s = "BIND ";
    s += b.action; s += ' '; s += sourceWord(b.source);
    if (b.source == OcInputSource::Key) { s += ' '; s += b.key; }
    if (b.scale != 1.0f) { s += " scale "; s += num(b.scale); }
    if (b.component != 0) { s += " component "; s += componentWord(b.component); }
    s += '\n';
    return s;
}
std::string actionLine(const OcInputAction& a) {
    return "ACTION " + a.name + " " + valueTypeWord(a.type) + "\n";
}

// Which record kind, if any, a line of EXISTING text belongs to -- used only by writeOcinput's merge
// path, to replace each kind in place at its own first occurrence rather than collapsing everything
// into one inserted block. `Other` covers blank lines, comments, and anything this format does not
// model; those are always copied through verbatim, at their original position. Identical shape and
// reasoning to OcParticle.cpp's own OwnedLineKind/classifyLine -- see there for why a whole-block
// replace (rather than per-line surgery) is the right strategy for a hand-edited text format.
enum class OwnedLineKind { Header, Name, Action, Bind, Context, Other };

OwnedLineKind classifyLine(std::string_view line, bool sawHeaderYet) {
    const std::vector<std::string_view> t = splitWhitespace(trim(truncateHash(line)));
    if (t.empty()) return OwnedLineKind::Other;   // blank line, or a comment (truncateHash ate it)
    if (!sawHeaderYet && equalsCI(t[0], "OCINPUT")) return OwnedLineKind::Header;
    if (equalsCI(t[0], "NAME"))    return OwnedLineKind::Name;
    if (equalsCI(t[0], "ACTION"))  return OwnedLineKind::Action;
    if (equalsCI(t[0], "BIND"))    return OwnedLineKind::Bind;
    if (equalsCI(t[0], "CONTEXT")) return OwnedLineKind::Context;
    return OwnedLineKind::Other;
}

} // namespace

// Parses .ocinput text into an OcInputData. Returns false with `err` set. See this file's header
// comment for the full grammar and OcInput.hpp for the strictness contract.
bool parseOcinput(std::string_view text, OcInputData& outData, std::string* err) {
    // RESET UP FRONT, PARSE INTO A LOCAL, COMMIT ONLY ON SUCCESS -- the fix OcParticle.cpp's own
    // parseOcparticle states at length: a caller who ignores the return value must see this format's
    // own defaults, never a half-scheme mixing real records with defaults for whatever came after the
    // line that failed.
    outData = OcInputData{};
    OcInputData out{};

    // BOM handling, identical to OcProject.cpp's parseOcproject: a file saved by an editor that
    // stamps a UTF-8 BOM must not have its first real line (the OCINPUT header) rejected because of
    // three invisible bytes in front of it.
    if (text.size() >= 3 && static_cast<u8>(text[0]) == 0xEF &&
        static_cast<u8>(text[1]) == 0xBB && static_cast<u8>(text[2]) == 0xBF)
        text = text.substr(3);

    bool sawHeader = false;

    usize pos = 0;
    while (pos <= text.size()) {
        usize nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        const std::string_view rawLine = text.substr(pos, nl - pos);
        pos = nl + 1;

        const std::string_view line = stripTrailingSemicolon(truncateHash(rawLine));
        if (line.empty()) continue;

        const std::vector<std::string_view> t = splitWhitespace(line);
        if (t.empty()) continue;
        const std::string_view key = t[0];
        // The trimmed remainder of the line after `key`, for NAME's free-text value. Identical to
        // OcParticle.cpp's own `after` lambda (trim is applied at the call site, not here, matching
        // that file exactly).
        const auto after = [&line](std::string_view tok) {
            return line.substr(static_cast<usize>(tok.data() - line.data()) + tok.size());
        };

        if (equalsCI(key, "OCINPUT")) {
            i32 version = 0;
            if (t.size() < 2 || !strictI32(t[1], version) || version != 1) {
                if (err) *err = "unsupported or missing OCINPUT version (want 'OCINPUT 1')";
                return false;
            }
            sawHeader = true;
        } else if (equalsCI(key, "NAME")) {
            out.name = std::string(trim(after(key)));
        } else if (equalsCI(key, "ACTION")) {
            if (t.size() != 3) {
                if (err) *err = "ACTION requires exactly 2 fields: ACTION name digital|axis1|axis2";
                return false;
            }
            OcInputAction a;
            a.name = std::string(t[1]);
            if (equalsCI(t[2], "digital"))     a.type = OcInputValueType::Digital;
            else if (equalsCI(t[2], "axis1"))  a.type = OcInputValueType::Axis1D;
            else if (equalsCI(t[2], "axis2"))  a.type = OcInputValueType::Axis2D;
            else {
                if (err) *err = "unknown ACTION type '" + std::string(t[2]) + "' (want digital, axis1 or axis2)";
                return false;
            }
            out.actions.push_back(std::move(a));
        } else if (equalsCI(key, "BIND")) {
            if (t.size() < 3) {
                if (err) *err = "BIND requires at least: BIND action key|mousex|mousey|wheel";
                return false;
            }
            OcInputBinding b;
            b.action = std::string(t[1]);
            usize next = 0;   // set in every branch below that does not return first
            if (equalsCI(t[2], "key")) {
                // The key name is a BARE token (not a keyword-value pair like scale/component below)
                // because, unlike scale/component, it has no default -- a Key source binding that did
                // not say which key would bind nothing, so this is a required 4th field, not optional.
                if (t.size() < 4) {
                    if (err) *err = "BIND key requires a key name: BIND action key <KeyName>";
                    return false;
                }
                b.source = OcInputSource::Key;
                b.key = std::string(t[3]);
                next = 4;
            } else if (equalsCI(t[2], "mousex")) { b.source = OcInputSource::MouseX;     next = 3; }
            else if (equalsCI(t[2], "mousey"))   { b.source = OcInputSource::MouseY;     next = 3; }
            else if (equalsCI(t[2], "wheel"))    { b.source = OcInputSource::MouseWheel; next = 3; }
            else {
                if (err) *err = "unknown BIND source '" + std::string(t[2]) + "' (want key, mousex, mousey or wheel)";
                return false;
            }
            // Everything from `next` on is `keyword value` pairs (scale/component), in either order,
            // each optional -- so an odd count means one of them is missing its value.
            if ((t.size() - next) % 2 != 0) {
                if (err) *err = "BIND has a trailing keyword with no value";
                return false;
            }
            for (usize i = next; i < t.size(); i += 2) {
                const std::string_view kw = t[i], val = t[i + 1];
                if (equalsCI(kw, "scale")) {
                    if (!strictF32(val, b.scale)) {
                        if (err) *err = "BIND scale has a malformed number";
                        return false;
                    }
                } else if (equalsCI(kw, "component")) {
                    if (equalsCI(val, "x"))      b.component = 0;
                    else if (equalsCI(val, "y")) b.component = 1;
                    else if (equalsCI(val, "z")) b.component = 2;
                    else {
                        if (err) *err = "BIND component must be x, y or z, got '" + std::string(val) + "'";
                        return false;
                    }
                } else {
                    if (err) *err = "unknown BIND attribute '" + std::string(kw) + "' (want scale or component)";
                    return false;
                }
            }
            out.bindings.push_back(std::move(b));
        } else if (equalsCI(key, "CONTEXT")) {
            if (t.size() != 4 || !equalsCI(t[2], "priority")) {
                if (err) *err = "CONTEXT requires exactly: CONTEXT name priority <n>";
                return false;
            }
            i32 pr = 0;
            if (!strictI32(t[3], pr)) {
                if (err) *err = "CONTEXT priority has a malformed number";
                return false;
            }
            out.contextName = std::string(t[1]);
            out.contextPriority = pr;
        }
        // Any other key: forward-compat, unknown record. Skipped here, preserved verbatim by
        // writeOcinput's merge below.
    }

    if (!sawHeader) {
        if (err) *err = "not an .ocinput file: no OCINPUT header line";
        return false;
    }

    // EVERY BIND MUST NAME A DECLARED ACTION, CHECKED HERE RATHER THAN INLINE, because BIND is
    // allowed to forward-reference an ACTION declared later in the file (see this file's header
    // comment on why BIND names its action explicitly) -- so the only point at which "does this
    // action exist" can be answered is once the whole file has been read, the same reason
    // OcGraph.cpp validates LINK/COMP-parent references in a post-pass rather than inline.
    for (const OcInputBinding& b : out.bindings) {
        bool found = false;
        for (const OcInputAction& a : out.actions) {
            if (a.name == b.action) { found = true; break; }
        }
        if (!found) {
            if (err) *err = "BIND names action '" + b.action + "', which this file never declares with ACTION";
            return false;
        }
    }

    // The single commit point. Everything above wrote into the local `out`, so a caller who ignored
    // the bool on a malformed file still has the struct they came in with (OcInputData{}, from the
    // reset at the top) rather than a half-parsed scheme.
    outData = std::move(out);
    return true;
}

// Loads an .ocinput file. A missing or unreadable file is an error, never a default scheme.
bool loadOcinput(const std::string& path, OcInputData& out, std::string* err) {
    std::string text;
    if (!readFileText(path, text)) {
        if (err) *err = "could not read " + path;
        return false;
    }
    return parseOcinput(text, out, err);
}

// Renders a scheme as .ocinput text, merging into `existing` (see OcInput.hpp).
std::string writeOcinput(const OcInputData& d, std::string_view existing) {
    const std::string nameLine = d.name.empty() ? std::string() : ("NAME " + d.name + "\n");

    // ACTION and BIND are regenerated as ONE block each, in `d.actions`/`d.bindings` order -- not
    // per-record, unlike OcGraph.cpp's NODE/PIN merge (which keys each record by its own stable id
    // so an unrelated edit does not reshuffle every node). OcInput has no id finer than the action
    // name a BIND already carries, and a scheme is authored as a whole (an editor panel for it would
    // show every action and binding together, not one at a time) -- so the OcParticle.cpp strategy
    // (regenerate the whole owned region fresh, replace it at its first original position) is the
    // right-sized choice here, not the more elaborate per-id one.
    std::string actionBlock;
    for (const OcInputAction& a : d.actions) actionBlock += actionLine(a);
    std::string bindBlock;
    for (const OcInputBinding& b : d.bindings) bindBlock += bindLine(b);

    const std::string contextLine = d.contextName.empty() ? std::string() :
        ("CONTEXT " + d.contextName + " priority " + std::to_string(d.contextPriority) + "\n");

    // If no existing content, build from scratch: header, comment, NAME, then the action block, then
    // the bind block, then CONTEXT -- matching this file's header comment's worked example and
    // .ocparticle's own fresh-write layout (a blank line ahead of each group).
    if (trim(existing).empty()) {
        std::string out = "OCINPUT 1\n";
        out += "# Input binding scheme, written by the Aver Engine editor.\n";
        out += nameLine;
        out += "\n";
        out += actionBlock;
        out += "\n";
        out += bindBlock;
        if (!contextLine.empty()) { out += "\n"; out += contextLine; }
        return out;
    }

    // Otherwise, merge: replace each record KIND in place, at its own first occurrence in
    // `existing`, and copy everything else -- comments, blank lines, and any record this format does
    // not model -- through untouched, at its original position. Identical strategy to
    // OcParticle.cpp's writeOcparticle; see its comment for why a whole-block replace is the right
    // shape for a hand-edited text format like this one.
    std::vector<std::string_view> lines;
    {
        usize p = 0;
        while (p < existing.size()) {
            usize nl = existing.find('\n', p);
            const bool last = (nl == std::string_view::npos);
            if (last) nl = existing.size();
            lines.push_back(existing.substr(p, nl - p));
            p = nl + 1;
            if (last) break;
        }
    }

    std::vector<OwnedLineKind> kinds(lines.size(), OwnedLineKind::Other);
    {
        bool sawHeader = false;
        for (usize i = 0; i < lines.size(); ++i) {
            kinds[i] = classifyLine(lines[i], sawHeader);
            if (kinds[i] == OwnedLineKind::Header) sawHeader = true;
        }
    }

    bool placedName = false, placedAction = false, placedBind = false, placedContext = false;

    std::string out;
    out.reserve(existing.size() + nameLine.size() + actionBlock.size() + bindBlock.size() +
                contextLine.size() + 64);

    for (usize i = 0; i < lines.size(); ++i) {
        switch (kinds[i]) {
        case OwnedLineKind::Header:
            out += "OCINPUT 1\n";
            break;
        case OwnedLineKind::Name:
            if (!placedName) { placedName = true; out += nameLine; }   // empty nameLine = line removed
            break;                                                     // any later duplicate is dropped
        case OwnedLineKind::Action:
            // EVERY ACTION line in `existing` collapses into the ONE regenerated block, placed at the
            // position of the FIRST one; any later ACTION line in the original is dropped here (it is
            // already represented, in order, inside actionBlock).
            if (!placedAction) { placedAction = true; out += actionBlock; }
            break;
        case OwnedLineKind::Bind:
            if (!placedBind) { placedBind = true; out += bindBlock; }
            break;
        case OwnedLineKind::Context:
            if (!placedContext) { placedContext = true; out += contextLine; }   // empty = line removed
            break;
        case OwnedLineKind::Other:
            out += lines[i];
            out += '\n';
            break;
        }
    }

    // A kind that never appeared in `existing` at all has no in-place position to take; append it,
    // each preceded by a blank line so it does not run directly into whatever came before it --
    // matching OcGraph.cpp/OcParticle.cpp's own tail-append fallback.
    if (!placedName && !nameLine.empty())      { out += "\n"; out += nameLine; }
    if (!placedAction && !actionBlock.empty()) { out += "\n"; out += actionBlock; }
    if (!placedBind && !bindBlock.empty())     { out += "\n"; out += bindBlock; }
    if (!placedContext && !contextLine.empty()) { out += "\n"; out += contextLine; }

    return out;
}

} // namespace aver::fmt
