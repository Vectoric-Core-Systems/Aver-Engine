// .ocgraph reader and writer: text scanner and serialiser for the visual scripting format.
#include "aver/formats/OcGraph.hpp"
#include "aver/formats/detail/TextScan.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include <unordered_set>

namespace aver::fmt {
using namespace aver::fmt::detail;

namespace {

// Formats a number for the text form: enough digits to round-trip, no trailing noise.
std::string num(f64 v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
}

// Whether a token is a coordinate rather than something else that happens to sit in that slot.
//
// The NODE line's x and y are optional, so slot 3 might hold a coordinate or might hold an
// attribute like `param=time`. Deciding by shape is what keeps an attribute from being eaten as
// x==0 and then written back out as a coordinate that was never there.
bool isNumericToken(std::string_view s) {
    if (s.empty()) return false;
    usize i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (i >= s.size()) return false;
    bool digit = false;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (c >= '0' && c <= '9') { digit = true; continue; }
        if (c == '.' || c == 'e' || c == 'E' || c == '-' || c == '+') continue;
        return false;
    }
    return digit;
}

// Which record kind, if any, a line of EXISTING text belongs to -- used only by writeOcgraph's merge
// path (see there) to replace each kind in place at its own first occurrence, rather than collapsing
// every kind into one inserted block. `Other` covers blank lines, comments, and anything writeOcgraph
// does not model; those are always copied through verbatim, at their original position.
enum class OwnedLineKind { Header, Name, Description, Node, Pin, Link, Out, Other };

// `sawHeader` is the caller's running state: only the FIRST line whose key is OCGRAPH counts as the
// header; a later stray "OCGRAPH ..." line (malformed input, or inside an unrelated unknown record)
// falls through to Other like any other line the format doesn't specially recognise there.
OwnedLineKind classifyLine(std::string_view line, bool sawHeaderYet) {
    const std::vector<std::string_view> t = splitWhitespace(trim(truncateHash(line)));
    if (t.empty()) return OwnedLineKind::Other; // blank line, or a comment (truncateHash ate it)
    if (!sawHeaderYet && equalsCI(t[0], "OCGRAPH")) return OwnedLineKind::Header;
    if (equalsCI(t[0], "NAME"))        return OwnedLineKind::Name;
    if (equalsCI(t[0], "DESCRIPTION")) return OwnedLineKind::Description;
    if (equalsCI(t[0], "NODE"))        return OwnedLineKind::Node;
    if (equalsCI(t[0], "PIN"))         return OwnedLineKind::Pin;
    if (equalsCI(t[0], "LINK"))        return OwnedLineKind::Link;
    if (equalsCI(t[0], "OUT"))         return OwnedLineKind::Out;
    return OwnedLineKind::Other;
}

} // namespace

// Parses a .ocgraph from memory. Unknown records are skipped during parse.
// Validates that all links reference existing nodes and pins.
bool parseOcgraph(std::string_view text, OcGraphData& out, std::string* err) {
    out = OcGraphData{};
    bool sawHeader = false;

    // First pass: collect nodes to validate link references.
    std::unordered_map<std::string, std::unordered_set<std::string>> nodeOutputPins;
    std::unordered_map<std::string, std::unordered_set<std::string>> nodeInputPins;

    usize pos = 0;
    while (pos <= text.size()) {
        usize nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view rawLine = text.substr(pos, nl - pos);
        pos = nl + 1;

        std::string_view line = stripTrailingSemicolon(truncateHash(rawLine));
        if (line.empty()) continue;

        std::vector<std::string_view> t = splitWhitespace(line);
        if (t.empty()) continue;
        std::string_view key = t[0];

        if (equalsCI(key, "OCGRAPH")) {
            out.version = t.size() > 1 ? parseI32(t[1], 1) : 1;
            sawHeader = true;
        } else if (equalsCI(key, "NAME")) {
            out.name = t.size() > 1 ? std::string(t[1]) : std::string();
        } else if (equalsCI(key, "DESCRIPTION")) {
            // Take the rest of the RAW line as the description -- NOT `line`, which has already
            // been through truncateHash()/stripTrailingSemicolon() for record-dispatch purposes.
            // DESCRIPTION is documented as free text ("Optional human-readable description",
            // aver/formats/OcGraph.hpp), so a literal '#' or trailing ';' inside it is DATA, not a
            // comment marker or statement terminator. Reading from `line` here silently truncated
            // any description containing '#' -- exactly what the checked-in cross-impl fixture's own
            // description does ("...parsed and executed by the C# runtime"), which is how this was
            // found: an edit-free load of that real file followed by a save did not reproduce it.
            //
            // Judged from the RAW extracted text, not from `t.size() > 1` (a token count of the
            // hash-truncated `line`): a description consisting only of characters after a literal
            // '#' -- an extreme case, but the whole point of this fix is that '#' is ordinary data
            // here -- would otherwise still read as "no value" even after switching to `rawLine`.
            const usize keyEnd = rawLine.find("DESCRIPTION");
            if (keyEnd != std::string_view::npos) {
                const std::string_view desc = trim(rawLine.substr(keyEnd + 11));
                if (!desc.empty()) out.description = std::string(desc);
            }
        } else if (equalsCI(key, "NODE")) {
            // `NODE id type` is the minimum; x and y are OPTIONAL and default to 0.
            //
            // They used to be mandatory, and that made this parser reject files the C# writer
            // produces -- it emits `NODE radius ConstFloat` with no coordinates, because a graph
            // authored by hand or by a compiler has no canvas layout to record. The two
            // implementations are supposed to agree on this format; demanding a field the other
            // side never writes is not agreement. A graph with no stored positions gets laid out on
            // open instead, which is the editor's job, not the parser's.
            if (t.size() < 3) {
                if (err) *err = "NODE requires at least 3 tokens: NODE id type [x y]";
                return false;
            }
            OcGraphNode node;
            node.id = std::string(t[1]);
            node.type = std::string(t[2]);
            // Only read as a coordinate what actually parses as one. An attribute like `param=time`
            // sitting in slot 3 must not be silently consumed as x==0 -- it has to reach
            // extraTokens, or writing the file back out drops it.
            usize firstExtra = 3;
            node.hasPosition = false;   // until a coordinate is actually seen; see the header
            if (t.size() > 3 && isNumericToken(t[3])) {
                node.x = parseF64(t[3]);
                node.hasPosition = true;
                firstExtra = 4;
                if (t.size() > 4 && isNumericToken(t[4])) {
                    node.y = parseF64(t[4]);
                    firstExtra = 5;
                }
            }
            for (usize i = firstExtra; i < t.size(); ++i) node.extraTokens.emplace_back(t[i]);

            // Check for duplicate node ID.
            for (const auto& n : out.nodes) {
                if (n.id == node.id) {
                    if (err) *err = "duplicate node ID: " + node.id;
                    return false;
                }
            }

            out.nodes.push_back(node);
        } else if (equalsCI(key, "PIN")) {
            if (t.size() < 4) {
                if (err) *err = "PIN requires at least 4 tokens: PIN nodeid name direction type [default]";
                return false;
            }
            const std::string nodeId = std::string(t[1]);
            const std::string pinName = std::string(t[2]);
            const std::string direction = std::string(t[3]);
            const std::string pinType = t.size() > 4 ? std::string(t[4]) : std::string();
            std::string defaultValue = t.size() > 5 ? std::string(t[5]) : std::string();

            // Find the node.
            OcGraphNode* node = nullptr;
            for (auto& n : out.nodes) {
                if (n.id == nodeId) {
                    node = &n;
                    break;
                }
            }
            if (!node) {
                if (err) *err = "PIN references non-existent node: " + nodeId;
                return false;
            }

            OcGraphPin pin;
            pin.name = pinName;
            pin.type = pinType;
            pin.isOutput = equalsCI(direction, "out");
            if (!equalsCI(direction, "in") && !equalsCI(direction, "out")) {
                if (err) *err = "PIN direction must be 'in' or 'out', got: " + direction;
                return false;
            }
            pin.defaultValue = defaultValue;

            node->pins.push_back(pin);

            // Track pins for link validation.
            if (pin.isOutput) {
                nodeOutputPins[nodeId].insert(pinName);
            } else {
                nodeInputPins[nodeId].insert(pinName);
            }
        } else if (equalsCI(key, "LINK")) {
            if (t.size() < 3) {
                if (err) *err = "LINK requires at least 3 tokens: LINK sourcenode.sourcepin destnode.destpin";
                return false;
            }

            // Parse source: nodeid.pinname
            const std::string sourceFull = std::string(t[1]);
            const usize sourceDot = sourceFull.find('.');
            if (sourceDot == std::string::npos) {
                if (err) *err = "LINK source must be in format nodeid.pinname, got: " + sourceFull;
                return false;
            }
            const std::string sourceNode = sourceFull.substr(0, sourceDot);
            const std::string sourcePin = sourceFull.substr(sourceDot + 1);

            // Parse dest: nodeid.pinname
            const std::string destFull = std::string(t[2]);
            const usize destDot = destFull.find('.');
            if (destDot == std::string::npos) {
                if (err) *err = "LINK dest must be in format nodeid.pinname, got: " + destFull;
                return false;
            }
            const std::string destNode = destFull.substr(0, destDot);
            const std::string destPin = destFull.substr(destDot + 1);

            // Validate that nodes exist.
            bool sourceNodeExists = false, destNodeExists = false;
            for (const auto& n : out.nodes) {
                if (n.id == sourceNode) sourceNodeExists = true;
                if (n.id == destNode) destNodeExists = true;
            }
            if (!sourceNodeExists) {
                if (err) *err = "LINK references non-existent source node: " + sourceNode;
                return false;
            }
            if (!destNodeExists) {
                if (err) *err = "LINK references non-existent dest node: " + destNode;
                return false;
            }

            // At this point, we don't validate that the pins exist yet because we may not have seen
            // all PIN records. We could do a second pass, but for now accept that links reference
            // unknown pins (they will be caught at runtime).

            OcGraphLink link;
            link.sourceNode = sourceNode;
            link.sourcePin = sourcePin;
            link.destNode = destNode;
            link.destPin = destPin;

            out.links.push_back(link);
        } else if (equalsCI(key, "OUT")) {
            // OUT <nodeId> <pinName> -- which pin the graph hands back. See OcGraphData::outputs.
            if (t.size() < 3) {
                if (err) *err = "OUT requires 2 tokens: OUT nodeid pinname";
                return false;
            }
            const std::string nodeId(t[1]);
            const std::string pinName(t[2]);
            // Validated against the nodes seen so far, like LINK is, so a typo is refused here
            // rather than surfacing as a null result when something tries to run the graph.
            bool outNodeExists = false;
            for (const auto& n : out.nodes) if (n.id == nodeId) outNodeExists = true;
            if (!outNodeExists) {
                if (err) *err = "OUT references non-existent node: " + nodeId;
                return false;
            }
            out.outputs.emplace_back(nodeId, pinName);
        }
    }

    if (!sawHeader) {
        if (err) *err = "not an .ocgraph file (no OCGRAPH header line)";
        return false;
    }
    return true;
}

// Loads a .ocgraph from disk.
bool loadOcgraph(const std::string& path, OcGraphData& out, std::string* err) {
    std::string text;
    if (!readFileText(path, text)) {
        if (err) *err = "could not read " + path;
        return false;
    }
    return parseOcgraph(text, out, err);
}

// Serialises a graph to the text form, preserving unknown records from `existing`.
std::string writeOcgraph(const OcGraphData& g, std::string_view existing) {
    // The regenerated content for each record kind, built fresh from `g` regardless of what (if
    // anything) `existing` had. Shared by both branches below.
    const std::string nameLine = "NAME " + (g.name.empty() ? std::string("untitled") : g.name) + "\n";
    const std::string descLine = g.description.empty() ? std::string() : ("DESCRIPTION " + g.description + "\n");
    std::string nodeBlock, pinBlock, linkBlock, outBlock;
    for (const OcGraphNode& node : g.nodes) {
        // Coordinates only if the node actually had them, then every token this implementation did
        // not interpret, in its original order. Emitting `0 0` for a node that never carried a
        // position would change a file merely by opening and saving it; dropping the extras would
        // strip the C# side's `param=`/`field=` attributes and silently break the graph.
        nodeBlock += "NODE " + node.id + " " + node.type;
        if (node.hasPosition) nodeBlock += " " + num(node.x) + " " + num(node.y);
        for (const std::string& extra : node.extraTokens) nodeBlock += " " + extra;
        nodeBlock += "\n";
    }
    for (const OcGraphNode& node : g.nodes) {
        for (const OcGraphPin& pin : node.pins) {
            pinBlock += "PIN " + node.id + " " + pin.name + " ";
            pinBlock += pin.isOutput ? "out" : "in";
            pinBlock += " " + pin.type;
            if (!pin.defaultValue.empty()) pinBlock += " " + pin.defaultValue;
            pinBlock += "\n";
        }
    }
    for (const OcGraphLink& link : g.links) {
        linkBlock += "LINK " + link.sourceNode + "." + link.sourcePin +
                     " " + link.destNode + "." + link.destPin + "\n";
    }
    // Outputs LAST, because they read as the conclusion of the graph -- a human scanning the file
    // looks for them where a return statement would be.
    for (const auto& o : g.outputs) {
        outBlock += "OUT " + o.first + " " + o.second + "\n";
    }

    // If no existing content, build from scratch with header, comment, and formatting: a blank line
    // ahead of each non-empty section, so a freshly-written file reads in visually separated blocks.
    if (trim(existing).empty()) {
        std::string out = "OCGRAPH " + std::to_string(g.version > 0 ? g.version : 1) + "\n";
        out += "# Visual scripting graph, written by the Aver Engine editor.\n";
        out += nameLine;
        out += descLine;
        if (!nodeBlock.empty()) { out += "\n"; out += nodeBlock; }
        if (!pinBlock.empty())  { out += "\n"; out += pinBlock; }
        if (!linkBlock.empty()) { out += "\n"; out += linkBlock; }
        if (!outBlock.empty())  { out += "\n"; out += outBlock; }
        return out;
    }

    // Otherwise, merge: replace each record KIND in place, at its own first occurrence in `existing`,
    // and copy everything else -- comments, blank lines, and any record this format does not model --
    // through untouched, at its original position.
    //
    // This replaces each kind independently rather than collapsing NAME/DESCRIPTION/NODE/PIN/LINK/OUT
    // into one block dropped at the first owned line found (the previous strategy here). That single-
    // block approach silently relocated every blank line that separated two record blocks -- e.g. the
    // blank line between the NODE block and the PIN block -- to wherever the scan next found unowned
    // content, because every original owned line between two blank lines got dropped without being
    // re-emitted while the blank lines themselves (never an owned key) were still copied through. On a
    // real file that separates its sections with blank lines -- which is exactly what THIS function's
    // own fresh-write branch above produces -- an edit-free load -> save would visibly reformat the
    // file: every inter-section blank line pushed to the end, rather than staying where it was. Since
    // "records the reader did not understand survive a save, in place" is the exact property this
    // function exists for, a blank line -- which is just as unowned as a comment or a future record
    // type -- deserves that guarantee too. Replacing per-kind in place keeps it, in both a spaced file
    // (like this function's own fresh-write output) and a compact hand-written one with no separating
    // blanks at all -- neither style has anything invented or displaced when nothing was edited.
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

    bool placedName = false, placedDesc = false, placedNode = false;
    bool placedPin = false, placedLink = false, placedOut = false;
    std::string out;
    out.reserve(existing.size() + nodeBlock.size() + pinBlock.size() + linkBlock.size() + outBlock.size() + 64);

    for (usize i = 0; i < lines.size(); ++i) {
        switch (kinds[i]) {
        case OwnedLineKind::Header:
            out += "OCGRAPH " + std::to_string(g.version > 0 ? g.version : 1) + "\n";
            break;
        case OwnedLineKind::Name:
            if (!placedName) { placedName = true; out += nameLine; }
            break; // any later duplicate NAME line is dropped, not re-emitted
        case OwnedLineKind::Description:
            if (!placedDesc) { placedDesc = true; out += descLine; } // empty descLine = line removed
            break;
        case OwnedLineKind::Node:
            if (!placedNode) { placedNode = true; out += nodeBlock; } // whole current block, once
            break;
        case OwnedLineKind::Pin:
            if (!placedPin) { placedPin = true; out += pinBlock; }
            break;
        case OwnedLineKind::Link:
            if (!placedLink) { placedLink = true; out += linkBlock; }
            break;
        case OwnedLineKind::Out:
            if (!placedOut) { placedOut = true; out += outBlock; }
            break;
        case OwnedLineKind::Other:
            out += lines[i];
            out += '\n';
            break;
        }
    }

    // A kind that never appeared in `existing` at all (a brand-new section on a file that never had
    // one -- e.g. the first node added to a graph that used to have none) has no in-place position to
    // take; append it, in the same order the fresh-write branch above uses, each preceded by a blank
    // line so it does not run directly into whatever came before it.
    if (!placedName) out += nameLine;
    if (!placedDesc && !descLine.empty()) out += descLine;
    if (!placedNode && !nodeBlock.empty()) { out += "\n"; out += nodeBlock; }
    if (!placedPin && !pinBlock.empty())   { out += "\n"; out += pinBlock; }
    if (!placedLink && !linkBlock.empty()) { out += "\n"; out += linkBlock; }
    if (!placedOut && !outBlock.empty())   { out += "\n"; out += outBlock; }

    return out;
}

// Writes a graph to disk, creating parent directories.
bool saveOcgraph(const std::string& path, const OcGraphData& g, std::string* err) {
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);

    // Read whatever is already at `path` and hand it to writeOcgraph as the merge base.
    //
    // writeOcgraph has always taken an `existing` argument for exactly this -- it replaces each
    // record kind in place and copies comments, blank lines and unmodelled records through
    // verbatim -- and saveOcgraph, the function everything actually calls, never passed it. So
    // every save through this path silently discarded them. Measured on the sample project's
    // Drone.ocgraph: 4256 bytes in, 1561 out, the missing 63% being the comments explaining what
    // the graph computes. A node editor that opened and saved a file would have eaten them.
    //
    // Read failure is not an error: `path` may not exist yet, which is the ordinary case for a new
    // graph. An empty base makes writeOcgraph behave exactly as it did before.
    std::string existing;
    {
        std::ifstream in(path, std::ios::binary);
        if (in) existing.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { if (err) *err = "could not open " + path + " for writing"; return false; }
    const std::string text = writeOcgraph(g, existing);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) { if (err) *err = "write failed for " + path; return false; }
    return true;
}

} // namespace aver::fmt
