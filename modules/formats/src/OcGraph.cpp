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
enum class OwnedLineKind { Header, Name, Description, Var, Comp, Node, Pin, Link, Entry, Out, Other };

// `sawHeader` is the caller's running state: only the FIRST line whose key is OCGRAPH counts as the
// header; a later stray "OCGRAPH ..." line (malformed input, or inside an unrelated unknown record)
// falls through to Other like any other line the format doesn't specially recognise there.
OwnedLineKind classifyLine(std::string_view line, bool sawHeaderYet) {
    const std::vector<std::string_view> t = splitWhitespace(trim(truncateHash(line)));
    if (t.empty()) return OwnedLineKind::Other; // blank line, or a comment (truncateHash ate it)
    if (!sawHeaderYet && equalsCI(t[0], "OCGRAPH")) return OwnedLineKind::Header;
    if (equalsCI(t[0], "NAME"))        return OwnedLineKind::Name;
    if (equalsCI(t[0], "DESCRIPTION")) return OwnedLineKind::Description;
    if (equalsCI(t[0], "VAR"))         return OwnedLineKind::Var;
    if (equalsCI(t[0], "COMP"))        return OwnedLineKind::Comp;
    if (equalsCI(t[0], "NODE"))        return OwnedLineKind::Node;
    if (equalsCI(t[0], "PIN"))         return OwnedLineKind::Pin;
    if (equalsCI(t[0], "LINK"))        return OwnedLineKind::Link;
    if (equalsCI(t[0], "ENTRY"))       return OwnedLineKind::Entry;
    if (equalsCI(t[0], "OUT"))         return OwnedLineKind::Out;
    return OwnedLineKind::Other;
}

} // namespace

// Parses a .ocgraph from memory. Unknown records are skipped during parse.
// Validates that all links reference existing nodes and pins.
std::string_view componentAttr(const OcGraphComponent& c, std::string_view key) {
    for (const std::string& tokenText : c.extraTokens) {
        std::string_view t(tokenText);
        const usize eq = t.find('=');
        if (eq == std::string_view::npos) continue;   // a bare token, not an attribute
        if (t.substr(0, eq) == key) return t.substr(eq + 1);
    }
    return {};
}

void setComponentAttr(OcGraphComponent& c, std::string_view key, std::string_view value) {
    for (usize i = 0; i < c.extraTokens.size(); ++i) {
        std::string_view t(c.extraTokens[i]);
        const usize eq = t.find('=');
        if (eq == std::string_view::npos || t.substr(0, eq) != key) continue;
        if (value.empty()) c.extraTokens.erase(c.extraTokens.begin() + static_cast<isize>(i));
        else c.extraTokens[i] = std::string(key) + "=" + std::string(value);
        return;
    }
    if (!value.empty()) c.extraTokens.emplace_back(std::string(key) + "=" + std::string(value));
}

bool parseOcgraph(std::string_view text, OcGraphData& out, std::string* err) {
    out = OcGraphData{};
    bool sawHeader = false;

    // First pass: collect nodes to validate link references.
    // PIN records are BUFFERED rather than applied where they are read, for the same reason the
    // ENTRY/OUT/LINK checks at the bottom of this function happen there: a record may name a node
    // that appears later in the file. Applying them after the loop costs one pass and makes record
    // order irrelevant, which is what the format actually promises -- the writer reorders records
    // freely, so any rule this reader infers from position is a rule the writer will break.
    //
    // The order of pins WITHIN a node is still the file's order, because this vector preserves it.
    // That matters: an explicit PIN record suppresses a node's default pins entirely, so the file
    // is the only thing that says what order they draw in.
    std::vector<std::pair<std::string, OcGraphPin>> pendingPins;

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
        } else if (equalsCI(key, "VAR")) {
            // VAR <name> <type> [default] -- declares one graph-local persistent variable. See
            // OcGraphVariable's own comment (OcGraph.hpp) for why type/default are opaque strings at
            // this layer -- semantic validation (type must be Float/Int/Bool, duplicate names, an
            // unparseable default) is the C# side's job, the same division LINK/ENTRY already follow
            // for their own semantic rules. Structurally this layer asks only for a name and a type;
            // an unnamed or type-less VAR line is refused the same way a too-short NODE/PIN/LINK line
            // already is, rather than silently producing a variable nothing can address.
            if (t.size() < 3) {
                if (err) *err = "VAR requires a name and a type: VAR name type [default]";
                return false;
            }
            OcGraphVariable var;
            var.name = std::string(t[1]);
            var.type = std::string(t[2]);
            if (t.size() > 3) var.defaultValue = std::string(t[3]);
            out.variables.push_back(std::move(var));
        } else if (equalsCI(key, "COMP")) {
            // COMP <id> <Kind> [key=value]... -- one entry in a class graph's component tree. See
            // OcGraphComponent (OcGraph.hpp) for why the kind is an opaque token and every
            // attribute rides in extraTokens rather than becoming a field here.
            if (t.size() < 3) {
                if (err) *err = "COMP requires an id and a kind: COMP id Kind [key=value]...";
                return false;
            }
            OcGraphComponent comp;
            comp.id = std::string(t[1]);
            comp.kind = std::string(t[2]);
            for (usize i = 3; i < t.size(); ++i) comp.extraTokens.emplace_back(t[i]);
            for (const auto& c : out.components) {
                if (c.id == comp.id) {
                    if (err) *err = "duplicate component ID: " + comp.id;
                    return false;
                }
            }
            out.components.push_back(std::move(comp));
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

            OcGraphPin pin;
            pin.name = pinName;
            pin.type = pinType;
            pin.isOutput = equalsCI(direction, "out");
            if (!equalsCI(direction, "in") && !equalsCI(direction, "out")) {
                if (err) *err = "PIN direction must be 'in' or 'out', got: " + direction;
                return false;
            }
            pin.defaultValue = defaultValue;

            // Attached to its node after the whole file is read -- see pendingPins above. The
            // structural checks (token count, direction spelling) still happen HERE, on the line
            // that is wrong, because they need nothing but the line itself.
            pendingPins.emplace_back(nodeId, pin);
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

            // Node existence is checked after the loop, not here. This used to be an inline check
            // against the nodes seen SO FAR, and it is the same forward-reference bug ENTRY and OUT
            // were already fixed for -- it just outlived that fix by one record type. It refused
            // AN_FPCharacter.ocgraph, the FirstPerson template's main graph, which rejoins its jump
            // branch into `fireGate` three lines before `NODE fireGate` appears. The C# runtime
            // parses that file, so the template RAN and only the editor could not open it, which is
            // why it shipped in 0.3.0.
            //
            // Pins are not validated at all, here or later: a link may name a default pin this
            // reader never sees, because default pins come from the node TYPE and only the C# side
            // knows the type table.

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
            // NOT validated here -- see the deferred check after the parse loop for why the node it
            // names is allowed to appear later in the file than this record does.
            out.outputs.emplace_back(nodeId, pinName);
        } else if (equalsCI(key, "ENTRY")) {
            // ENTRY <nodeId> <eventName> -- see OcGraphData::entryPoints for what this means and why
            // it is a separate record. Validated the same way OUT is validated above (the named node
            // must exist) and no further: whether an eventName is declared twice is a semantic graph
            // rule left to the C# runtime's Graph.Validate(), matching how LINK's pin-type agreement
            // is also left to a higher layer.
            if (t.size() < 3) {
                if (err) *err = "ENTRY requires 2 tokens: ENTRY nodeid eventname";
                return false;
            }
            const std::string nodeId(t[1]);
            const std::string eventName(t[2]);
            // NOT validated here either -- deferred, immediately below the loop.
            out.entryPoints.emplace_back(nodeId, eventName);
        }
    }

    // ---- deferred reference checks -----------------------------------------------------------------
    // ENTRY AND OUT NAME THEIR NODE, AND THE NODE IS ALLOWED TO COME LATER IN THE FILE. These were
    // checked inline, against the nodes seen SO FAR, which quietly made this reader stricter than the
    // C# one it is supposed to agree with: OcGraphParser defers the same checks until the whole file is
    // read, so a graph written `ENTRY tick OnTick` / `NODE tick OnTick` -- the order every graph in this
    // repo uses, because it reads as a heading followed by its node -- parsed fine in the runtime and
    // was REFUSED here.
    //
    // The cost of that divergence was not theoretical: it made the C++ node editor unable to open
    // test-content/AN_Playable's three graphs, GraphDemo's IdleMotion.ocgraph and every graph of the
    // FirstPerson template -- that is, essentially every real graph in existence. The format
    // documentation had recorded it (docs/formats/FORMAT_SPECS.md 10a.3, "Forward-reference ordering")
    // as a known gap rather than a bug, which is why it survived: writing it down made it look decided.
    //
    // Checking after the loop is what the two readers already had in common everywhere else, and it
    // keeps the error itself -- a typo'd node id is still refused, with the same message, just once the
    // file is known to be complete.
    // A component's parent= must name another component, and following parents must reach a
    // component with none. Both halves matter and neither is the other: a typo'd parent silently
    // reparents a gun to the world origin, and a cycle -- two components naming each other -- is
    // not a tree at all, so the spawn walk that builds child entities from this would not return.
    // Refusing here is the only place that check can live once and be believed by both readers.
    for (const auto& c : out.components) {
        const std::string_view parent = componentAttr(c, "parent");
        if (parent.empty()) continue;
        bool exists = false;
        for (const auto& o : out.components) if (o.id == parent) { exists = true; break; }
        if (!exists) {
            if (err) *err = "COMP '" + c.id + "' names a non-existent parent: " + std::string(parent);
            return false;
        }
    }
    for (const auto& start : out.components) {
        // Bounded by the component count: a chain longer than that has revisited something, which
        // is a cycle -- cheaper and simpler than carrying a visited set per start, and this runs
        // over the handful of components an actor has, not over a graph's nodes.
        std::string_view at = componentAttr(start, "parent");
        for (usize hops = 0; !at.empty(); ++hops) {
            if (hops > out.components.size()) {
                if (err) *err = "COMP '" + start.id + "' is part of a parent cycle";
                return false;
            }
            std::string_view next;
            for (const auto& o : out.components) if (o.id == at) { next = componentAttr(o, "parent"); break; }
            at = next;
        }
    }

    for (const auto& pp : pendingPins) {
        OcGraphNode* node = nullptr;
        for (auto& n : out.nodes) if (n.id == pp.first) { node = &n; break; }
        if (!node) {
            if (err) *err = "PIN references non-existent node: " + pp.first;
            return false;
        }
        node->pins.push_back(pp.second);
    }
    for (const auto& l : out.links) {
        bool sourceExists = false, destExists = false;
        for (const auto& n : out.nodes) {
            if (n.id == l.sourceNode) sourceExists = true;
            if (n.id == l.destNode) destExists = true;
        }
        if (!sourceExists) {
            if (err) *err = "LINK references non-existent source node: " + l.sourceNode;
            return false;
        }
        if (!destExists) {
            if (err) *err = "LINK references non-existent dest node: " + l.destNode;
            return false;
        }
    }
    for (const auto& e : out.entryPoints) {
        bool exists = false;
        for (const auto& n : out.nodes) if (n.id == e.first) { exists = true; break; }
        if (!exists) {
            if (err) *err = "ENTRY references non-existent node: " + e.first;
            return false;
        }
    }
    for (const auto& o : out.outputs) {
        bool exists = false;
        for (const auto& n : out.nodes) if (n.id == o.first) { exists = true; break; }
        if (!exists) {
            if (err) *err = "OUT references non-existent node: " + o.first;
            return false;
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
    // ONE ENTRY PER RECORD, not one string per KIND, because the merge path below matches existing
    // lines to records INDIVIDUALLY -- see it for why. `key` is whatever identifies this record in a
    // line of existing text; `used` is set once a line has claimed it, so a record is emitted exactly
    // once even if the file names it twice.
    struct Rec { std::string key; std::string text; bool used = false; };
    std::vector<Rec> varRecs, compRecs, nodeRecs, pinRecs, linkRecs, entryRecs, outRecs;
    std::string varBlock, compBlock, nodeBlock, pinBlock, linkBlock, entryBlock, outBlock;
    // VAR right after NAME/DESCRIPTION, ahead of NODE -- matching where a graph author naturally
    // writes it (declare what the graph remembers, then the nodes that read/write it) and where the
    // checked-in cross-language fixture (tests/formats/src/OcGraphTest.cpp's own VAR test) puts it.
    for (const OcGraphVariable& v : g.variables) {
        std::string line = "VAR " + v.name + " " + v.type;
        if (!v.defaultValue.empty()) line += " " + v.defaultValue;
        line += "\n";
        varRecs.push_back({v.name, line, false});
    }
    // COMP after VAR and before NODE: what the actor IS, then what it remembers, then what it does.
    // The line is regenerated as `COMP id Kind` plus every attribute token verbatim, in the order
    // the file had them -- see OcGraphComponent, and OcGraphNode::extraTokens for the identical
    // reasoning about why reordering them would be a change to a file nobody asked to change.
    for (const OcGraphComponent& comp : g.components) {
        std::string line = "COMP " + comp.id + " " + comp.kind;
        for (const std::string& extra : comp.extraTokens) line += " " + extra;
        line += "\n";
        compRecs.push_back({comp.id, line, false});
    }
    for (const OcGraphNode& node : g.nodes) {
        // Coordinates only if the node actually had them, then every token this implementation did
        // not interpret, in its original order. Emitting `0 0` for a node that never carried a
        // position would change a file merely by opening and saving it; dropping the extras would
        // strip the C# side's `param=`/`field=` attributes and silently break the graph.
        std::string line = "NODE " + node.id + " " + node.type;
        if (node.hasPosition) line += " " + num(node.x) + " " + num(node.y);
        for (const std::string& extra : node.extraTokens) line += " " + extra;
        line += "\n";
        nodeRecs.push_back({node.id, line, false});
    }
    for (const OcGraphNode& node : g.nodes) {
        for (const OcGraphPin& pin : node.pins) {
            std::string line = "PIN " + node.id + " " + pin.name + " ";
            line += pin.isOutput ? "out" : "in";
            line += " " + pin.type;
            if (!pin.defaultValue.empty()) line += " " + pin.defaultValue;
            line += "\n";
            pinRecs.push_back({node.id + " " + pin.name, line, false});
        }
    }
    for (const OcGraphLink& link : g.links) {
        const std::string src = link.sourceNode + "." + link.sourcePin;
        const std::string dst = link.destNode + "." + link.destPin;
        linkRecs.push_back({src + " " + dst, "LINK " + src + " " + dst + "\n", false});
    }
    // Entry points right after links (they describe how the wires above get set in motion) and
    // BEFORE outputs, which stay last -- see the OUT loop's own comment just below for why outputs
    // keep the final position they always had.
    for (const auto& e : g.entryPoints) {
        entryRecs.push_back({e.first + " " + e.second, "ENTRY " + e.first + " " + e.second + "\n", false});
    }
    // Outputs LAST, because they read as the conclusion of the graph -- a human scanning the file
    // looks for them where a return statement would be.
    for (const auto& o : g.outputs) {
        outRecs.push_back({o.first + " " + o.second, "OUT " + o.first + " " + o.second + "\n", false});
    }
    // The per-KIND blocks the fresh-write branch below emits are simply those records concatenated,
    // so there is exactly one place that knows how a record is formatted.
    for (const Rec& r : varRecs)   varBlock   += r.text;
    for (const Rec& r : compRecs)  compBlock  += r.text;
    for (const Rec& r : nodeRecs)  nodeBlock  += r.text;
    for (const Rec& r : pinRecs)   pinBlock   += r.text;
    for (const Rec& r : linkRecs)  linkBlock  += r.text;
    for (const Rec& r : entryRecs) entryBlock += r.text;
    for (const Rec& r : outRecs)   outBlock   += r.text;

    // If no existing content, build from scratch with header, comment, and formatting: a blank line
    // ahead of each non-empty section, so a freshly-written file reads in visually separated blocks.
    if (trim(existing).empty()) {
        std::string out = "OCGRAPH " + std::to_string(g.version > 0 ? g.version : 1) + "\n";
        out += "# Visual scripting graph, written by the Aver Engine editor.\n";
        out += nameLine;
        out += descLine;
        if (!varBlock.empty())  { out += "\n"; out += varBlock; }
        if (!compBlock.empty()) { out += "\n"; out += compBlock; }
        if (!nodeBlock.empty()) { out += "\n"; out += nodeBlock; }
        if (!pinBlock.empty())  { out += "\n"; out += pinBlock; }
        if (!linkBlock.empty()) { out += "\n"; out += linkBlock; }
        if (!entryBlock.empty()) { out += "\n"; out += entryBlock; }
        if (!outBlock.empty())  { out += "\n"; out += outBlock; }
        return out;
    }

    // Otherwise, merge: replace each record KIND in place, at its own first occurrence in `existing`,
    // and copy everything else -- comments, blank lines, and any record this format does not model --
    // through untouched, at its original position.
    //
    // This replaces each kind independently rather than collapsing
    // NAME/DESCRIPTION/NODE/PIN/LINK/ENTRY/OUT into one block dropped at the first owned line found
    // (the previous strategy here). That single-
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

    // EACH RECORD IS REPLACED AT ITS OWN LINE, not each KIND at its kind's first line.
    //
    // The previous rule emitted a kind's WHOLE block wherever that kind first appeared and dropped
    // every later line of it. On a file whose records are interleaved -- which is every hand-written
    // graph in this repo, because a graph is written as commented sections, each with the nodes,
    // links and entries that belong to it -- that hauled every NODE in the file up to the first one
    // and left the rest of the file to close up behind them. Opening AN_Playable's Rules.ocgraph and
    // saving it unchanged moved `NODE tick OnTick` six lines up, away from the comment written
    // directly above it to explain it. Nothing was lost, and that is the trap: the file still parsed,
    // still ran, and no longer said what its author meant.
    //
    // Matching per record keeps every line where its author put it. A record whose line is gone from
    // `g` (deleted in the editor) simply is not re-emitted, and a record with no line at all (newly
    // added) falls through to the append pass below.
    const auto keyOf = [](OwnedLineKind k, std::string_view line) -> std::string {
        const std::vector<std::string_view> t = splitWhitespace(trim(truncateHash(line)));
        const auto tok = [&](usize i) { return i < t.size() ? std::string(t[i]) : std::string(); };
        switch (k) {
        case OwnedLineKind::Var:
        case OwnedLineKind::Comp:
        case OwnedLineKind::Node:  return tok(1);
        case OwnedLineKind::Pin:
        case OwnedLineKind::Link:
        case OwnedLineKind::Entry:
        case OwnedLineKind::Out:   return tok(1) + " " + tok(2);
        default:                   return std::string();
        }
    };
    // First UNUSED record with this key. Unused, not merely first, so a file that names the same
    // record twice consumes it once and drops the duplicate rather than emitting it twice.
    const auto claim = [](std::vector<Rec>& recs, const std::string& key, std::string& sink) {
        for (Rec& r : recs) {
            if (r.used || r.key != key) continue;
            r.used = true;
            sink += r.text;
            return;
        }
        // No record answers to this key: it was deleted. Emitting nothing is the deletion.
    };

    bool placedName = false, placedDesc = false;
    std::string out;
    out.reserve(existing.size() + varBlock.size() + compBlock.size() + nodeBlock.size() + pinBlock.size() + linkBlock.size()
                + entryBlock.size() + outBlock.size() + 64);

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
        case OwnedLineKind::Var:   claim(varRecs,   keyOf(kinds[i], lines[i]), out); break;
        case OwnedLineKind::Comp:  claim(compRecs,  keyOf(kinds[i], lines[i]), out); break;
        case OwnedLineKind::Node:  claim(nodeRecs,  keyOf(kinds[i], lines[i]), out); break;
        case OwnedLineKind::Pin:   claim(pinRecs,   keyOf(kinds[i], lines[i]), out); break;
        case OwnedLineKind::Link:  claim(linkRecs,  keyOf(kinds[i], lines[i]), out); break;
        case OwnedLineKind::Entry: claim(entryRecs, keyOf(kinds[i], lines[i]), out); break;
        case OwnedLineKind::Out:   claim(outRecs,   keyOf(kinds[i], lines[i]), out); break;
        case OwnedLineKind::Other:
            out += lines[i];
            out += '\n';
            break;
        }
    }

    // Whatever no line claimed: records ADDED since this file was written, plus whole kinds the file
    // never had. Appended in the same order the fresh-write branch uses, each group preceded by a
    // blank line so it does not run into whatever came before. There is no better position available
    // -- the file says nothing about where a record it has never seen belongs.
    const auto appendUnused = [&out](const std::vector<Rec>& recs) {
        std::string block;
        for (const Rec& r : recs) if (!r.used) block += r.text;
        if (!block.empty()) { out += "\n"; out += block; }
    };
    if (!placedName) out += nameLine;
    if (!placedDesc && !descLine.empty()) out += descLine;
    appendUnused(varRecs);
    appendUnused(compRecs);
    appendUnused(nodeRecs);
    appendUnused(pinRecs);
    appendUnused(linkRecs);
    appendUnused(entryRecs);
    appendUnused(outRecs);

    // KEEP THE FILE'S OWN LINE ENDINGS. Records are regenerated with "\n" while unknown lines are
    // copied verbatim, so a CRLF file came back with CRLF on the lines this writer did not touch and
    // LF on every line it did -- a save that silently rewrote the line endings of exactly the records
    // the author had been editing, and left the file mixed. Every graph in this repo happens to be
    // LF, which is why it went unnoticed; a graph written by a Windows editor is not.
    //
    // Decided by what `existing` actually uses rather than by platform: this is the file's property,
    // not the machine's. A file with no CRLF at all is left exactly as built.
    if (existing.find("\r\n") != std::string_view::npos) {
        std::string crlf;
        crlf.reserve(out.size() + out.size() / 32);
        for (usize i = 0; i < out.size(); ++i) {
            if (out[i] == '\n' && (i == 0 || out[i - 1] != '\r')) crlf += '\r';
            crlf += out[i];
        }
        return crlf;
    }
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
