#pragma once
// .ocgraph — the visual scripting graph format. Nodes, pins, and links.
// Text format, OC dialect, with support for unknown records (by design, not accidental).
// A graph can be parsed and later rewritten while preserving unrecognised records verbatim.
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt {

// One input or output pin on a node.
struct OcGraphPin {
    std::string name;        // Pin name (e.g., "value", "position")
    std::string type;        // Type name (e.g., "float", "int", "bool", "string")
    bool isOutput = false;   // true for output pins, false for input pins
    std::string defaultValue; // Default value for input pins; empty = no default
};

// One node in the graph.
struct OcGraphNode {
    std::string id;          // Unique node identifier
    std::string type;        // Node type name (e.g., "Add", "Multiply", "Constant")
    f64 x = 0.0, y = 0.0;   // Editor position (centimetres or units, TBD)
    std::vector<OcGraphPin> pins;
};

// One link connecting an output pin to an input pin.
struct OcGraphLink {
    std::string sourceNode;  // ID of the source node
    std::string sourcePin;   // Name of the output pin on the source node
    std::string destNode;    // ID of the destination node
    std::string destPin;     // Name of the input pin on the destination node
};

// A complete visual scripting graph.
struct OcGraphData {
    int version = 1;
    std::string name;        // Graph name; empty = "untitled"
    std::string description; // Optional human-readable description

    std::vector<OcGraphNode> nodes;
    std::vector<OcGraphLink> links;
};

// Parses a graph from memory. Unknown records are ignored during parse but preserved during rewrite.
// Returns false and sets *err if parsing fails (e.g., invalid header, link to non-existent node).
bool parseOcgraph(std::string_view text, OcGraphData& out, std::string* err = nullptr);

// Loads a graph from disk. Returns false and sets *err if file cannot be read.
bool loadOcgraph(const std::string& path, OcGraphData& out, std::string* err = nullptr);

// Serialises a graph to text form, preserving unknown records from the original.
// Pass an empty string to write a fresh graph. Round-trips through parseOcgraph.
std::string writeOcgraph(const OcGraphData& g, std::string_view existing = "");

// Writes a graph to disk, creating parent directories.
bool saveOcgraph(const std::string& path, const OcGraphData& g, std::string* err = nullptr);

} // namespace aver::fmt
