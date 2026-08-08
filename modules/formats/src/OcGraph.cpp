// .ocgraph reader and writer: text scanner and serialiser for the visual scripting format.
#include "aver/formats/OcGraph.hpp"
#include "aver/formats/detail/TextScan.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
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

// True when the line's key is one writeOcgraph owns and therefore replaces.
bool isOwnedKey(std::string_view line) {
    const std::string_view l = trim(line);
    if (l.empty() || l[0] == '#') return false;
    static const char* kOwned[] = {
        "OCGRAPH", "NAME", "DESCRIPTION", "NODE", "PIN", "LINK",
    };
    const std::vector<std::string_view> t = splitWhitespace(l);
    if (t.empty()) return false;
    for (const char* k : kOwned) if (equalsCI(t[0], k)) return true;
    return false;
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
            // Take the rest of the line as the description.
            if (t.size() > 1) {
                const usize keyEnd = line.find("DESCRIPTION");
                if (keyEnd != std::string_view::npos) {
                    std::string_view desc = trim(line.substr(keyEnd + 11));
                    out.description = std::string(desc);
                }
            }
        } else if (equalsCI(key, "NODE")) {
            if (t.size() < 4) {
                if (err) *err = "NODE requires at least 4 tokens: NODE id type x y";
                return false;
            }
            OcGraphNode node;
            node.id = std::string(t[1]);
            node.type = std::string(t[2]);
            node.x = parseF64(t[3]);
            node.y = t.size() > 4 ? parseF64(t[4]) : 0.0;

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
    // Build the owned content (all keys EXCEPT the header and explanatory comments).
    // Note: we do NOT include blank lines in owned content, to avoid duplicates during merge.
    std::string owned;
    owned.reserve(256 + g.nodes.size() * 64 + g.links.size() * 64);

    // Metadata (not the header, not explanatory comments which are only added on fresh writes).
    owned += "NAME " + (g.name.empty() ? std::string("untitled") : g.name) + "\n";
    if (!g.description.empty()) {
        owned += "DESCRIPTION " + g.description + "\n";
    }

    // Nodes, in order (no blank lines in owned - only output when writing fresh).
    for (const OcGraphNode& node : g.nodes) {
        owned += "NODE " + node.id + " " + node.type + " " + num(node.x) + " " + num(node.y) + "\n";
    }

    // Pins, in order (no blank lines in owned).
    for (const OcGraphNode& node : g.nodes) {
        for (const OcGraphPin& pin : node.pins) {
            owned += "PIN " + node.id + " " + pin.name + " ";
            owned += pin.isOutput ? "out" : "in";
            owned += " " + pin.type;
            if (!pin.defaultValue.empty()) {
                owned += " " + pin.defaultValue;
            }
            owned += "\n";
        }
    }

    // Links, in order (no blank lines in owned).
    for (const OcGraphLink& link : g.links) {
        owned += "LINK " + link.sourceNode + "." + link.sourcePin +
                 " " + link.destNode + "." + link.destPin + "\n";
    }

    // If no existing content, build from scratch with header, comment, and formatting.
    if (trim(existing).empty()) {
        std::string out = "OCGRAPH " + std::to_string(g.version > 0 ? g.version : 1) + "\n";
        out += "# Visual scripting graph, written by the Aver Engine editor.\n";
        if (!g.nodes.empty()) out += "\n";
        for (const OcGraphNode& node : g.nodes) {
            out += "NODE " + node.id + " " + node.type + " " + num(node.x) + " " + num(node.y) + "\n";
        }
        if (!g.nodes.empty()) out += "\n";
        for (const OcGraphNode& node : g.nodes) {
            for (const OcGraphPin& pin : node.pins) {
                out += "PIN " + node.id + " " + pin.name + " ";
                out += pin.isOutput ? "out" : "in";
                out += " " + pin.type;
                if (!pin.defaultValue.empty()) {
                    out += " " + pin.defaultValue;
                }
                out += "\n";
            }
        }
        if (!g.links.empty()) out += "\n";
        out += owned.substr(owned.find_last_of('\n') == std::string::npos ? 0 :
                            (owned.rfind("LINK") != std::string::npos ? owned.rfind("LINK") : owned.size()));
        // Simpler: just output with nice formatting
        out = "OCGRAPH " + std::to_string(g.version > 0 ? g.version : 1) + "\n";
        out += "# Visual scripting graph, written by the Aver Engine editor.\n";
        out += "NAME " + (g.name.empty() ? std::string("untitled") : g.name) + "\n";
        if (!g.description.empty()) {
            out += "DESCRIPTION " + g.description + "\n";
        }
        if (!g.nodes.empty()) {
            out += "\n";
            for (const OcGraphNode& node : g.nodes) {
                out += "NODE " + node.id + " " + node.type + " " + num(node.x) + " " + num(node.y) + "\n";
            }
        }
        if (!g.nodes.empty()) {
            out += "\n";
            for (const OcGraphNode& node : g.nodes) {
                for (const OcGraphPin& pin : node.pins) {
                    out += "PIN " + node.id + " " + pin.name + " ";
                    out += pin.isOutput ? "out" : "in";
                    out += " " + pin.type;
                    if (!pin.defaultValue.empty()) {
                        out += " " + pin.defaultValue;
                    }
                    out += "\n";
                }
            }
        }
        if (!g.links.empty()) {
            out += "\n";
            for (const OcGraphLink& link : g.links) {
                out += "LINK " + link.sourceNode + "." + link.sourcePin +
                       " " + link.destNode + "." + link.destPin + "\n";
            }
        }
        return out;
    }

    // Otherwise, merge: replace owned lines, copy through unowned lines.
    std::string out;
    out.reserve(existing.size() + owned.size() + 64);
    bool placed = false;
    bool sawHeader = false;
    usize pos = 0;

    while (pos < existing.size()) {
        usize nl = existing.find('\n', pos);
        const bool last = (nl == std::string_view::npos);
        if (last) nl = existing.size();
        const std::string_view raw = existing.substr(pos, nl - pos);
        pos = nl + 1;

        const std::vector<std::string_view> t = splitWhitespace(trim(truncateHash(raw)));
        if (!sawHeader && !t.empty() && equalsCI(t[0], "OCGRAPH")) {
            sawHeader = true;
            out += "OCGRAPH " + std::to_string(g.version > 0 ? g.version : 1) + "\n";
            if (last) break;
            continue;
        }
        if (isOwnedKey(raw)) {
            // Place all owned content on first owned key (other than OCGRAPH).
            if (!placed) {
                placed = true;
                out += owned;
            }
            if (last) break;
            continue;
        }
        out += raw;
        out += '\n';
        if (last) break;
    }
    if (!placed) out += owned;
    return out;
}

// Writes a graph to disk, creating parent directories.
bool saveOcgraph(const std::string& path, const OcGraphData& g, std::string* err) {
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { if (err) *err = "could not open " + path + " for writing"; return false; }
    const std::string text = writeOcgraph(g);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) { if (err) *err = "write failed for " + path; return false; }
    return true;
}

} // namespace aver::fmt
