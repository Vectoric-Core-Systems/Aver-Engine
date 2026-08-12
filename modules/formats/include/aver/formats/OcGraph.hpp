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
    // Whether x/y should be written. A graph written by hand or emitted by a compiler has no canvas
    // layout, and re-saving one must not invent `0 0` for it -- that would make a load/save round
    // trip change the file, which this format's tests forbid.
    //
    // DEFAULTS TRUE, and that direction matters. Only the parser sets it false, for a line that
    // genuinely carried no coordinates. Defaulting it false instead would mean any code building a
    // node in memory had to remember an extra flag or watch its positions silently vanish on
    // write -- which is exactly what happened when this was first written the other way round.
    bool hasPosition = true;
    std::vector<OcGraphPin> pins;

    // Trailing tokens on the NODE line that this implementation does not interpret, kept verbatim
    // and written back out in order.
    //
    // NOT decoration. The C# side (scripting/csharp/Aver.Graph/OcGraphParser.cs) puts key=value
    // attributes here -- `param=` naming a declared PARAM, `field=` naming a scene field -- and a
    // Param node without its `param=` does not merely lose a hint, it stops compiling. Without this
    // vector a C++ rewrite silently drops those attributes, so opening a graph in the node editor
    // and saving it would break the graph while reporting success.
    //
    // The whole-file unknown-RECORD preservation elsewhere in this format does not cover this case:
    // NODE is a record C++ owns, so its line is regenerated rather than passed through, and
    // everything on it that C++ did not model was being thrown away.
    std::vector<std::string> extraTokens;
};

// One link connecting an output pin to an input pin.
//
// EXEC LINKS, AND WHY THIS STRUCT DID NOT NEED A NEW FIELD TO GET THEM. A link is a DATA link or an
// EXEC (control-flow) link purely by the TYPE of the two pins it connects -- "exec" is just another
// pin type string, exactly like "float"/"int"/"bool"/"string" (OcGraphPin::type), so a link between
// two exec-typed pins already IS an exec link, with zero changes to this struct and zero changes to
// LINK's own grammar (still `LINK srcnode.srcpin destnode.destpin`, unchanged).
//
// Reusing the existing typed-pin mechanism, rather than adding an exec-vs-data flag to OcGraphLink or
// a second record kind (an `XLINK`), is what makes an OLDER FILE'S BEHAVIOUR GUARANTEED UNCHANGED:
// nothing in a pre-existing .ocgraph declares a pin of type "exec" (the string carried no special
// meaning before this), so parseOcgraph produces links identical to what it always produced for such
// a file, and every consumer that only ever dealt in float/int/bool/string pins keeps working exactly
// as it did -- there is no new branch in this reader that an old file can even reach.
//
// The ONLY code that needs to know "exec" is special is code that walks CONTROL FLOW. Concretely:
// the editor's link rule (aver::editor::canConnectPins, GraphEditorGeometry.cpp) already refuses to
// connect two differently-typed pins -- which for free means "an exec pin only connects to another
// exec pin, never to a data pin" (the task's own requirement), with no additional check written for
// it. The compiler (scripting/csharp/Aver.Graph/GraphCompiler.cs) is the one place that actually
// interprets an exec link as "run this next" rather than "read this value" -- see its own PUSH VS
// PULL comment for how it tells the two apart while walking the same OcGraphLink list this struct
// describes.
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

    // Which pins the graph HANDS BACK when it runs: `OUT <nodeId> <pinName>`, in order.
    //
    // THE FORMAT HAD NO WAY TO SAY THIS, and the gap only showed when a second implementation tried
    // to execute a graph rather than draw one. Nodes and links describe the dataflow; nothing said
    // which value the caller actually wanted, and "the node with no outgoing link" is not the same
    // question -- a graph can end in several such nodes, or in one whose result is a side effect.
    //
    // The C# runtime invented an OUT record for its own reader before this existed, so the two sides
    // agreed on nodes and links and silently disagreed about what a graph RETURNS. Writing it down
    // here is what makes them one format instead of two with the same name.
    std::vector<std::pair<std::string, std::string>> outputs;   // {nodeId, pinName}

    // Which node begins the PUSH/exec chain for a named event: `ENTRY <nodeId> <eventName>`, e.g.
    // `ENTRY tick_seq OnTick`. This is to CONTROL FLOW what `outputs` above is to DATA FLOW -- a graph
    // says not just what it computes but where it starts RUNNING and on what trigger -- and it exists
    // as its own record for the same reason `outputs` does: nothing about a node's own TYPE says
    // whether it is "the" beginning of a chain (a Sequence node behaves identically whether or not
    // something calls it), so the format has to say so out loud, in one place, rather than the two
    // implementations guessing at a convention and silently disagreeing -- exactly the trap the
    // `outputs` comment above documents and the one this field exists to not repeat for entry points.
    //
    // BACKWARD COMPATIBILITY IS BY CONSTRUCTION, THE SAME WAY `outputs` IS. An .ocgraph written before
    // ENTRY existed has no ENTRY lines; parseOcgraph never populates this vector for such a file --
    // there is nothing in the grammar that would make it try -- so entryPoints is empty, and
    // GraphCompiler.Compile() (the pre-existing PULL/dataflow compiler, driven entirely by `outputs`)
    // runs exactly as it always has. CompileEntryPoint() -- the new PUSH/exec compiler -- is a
    // SEPARATE method nothing calls unless a caller asks for one specific declared event by name. A
    // graph can therefore be pure dataflow (the only kind that existed before this change), pure exec
    // (no `outputs` at all, only ENTRY-triggered side effects), or both at once; the two halves do not
    // interact, and neither one's absence is an error.
    //
    // Two ENTRY records must not name the same eventName (ambiguous -- which node handles it?). That
    // is checked by the C# runtime's Graph.Validate(), not by this reader, for the same division of
    // labour LINK already follows: this layer accepts whatever is syntactically well-formed (the named
    // node exists) and leaves semantic graph rules -- pin type agreement on links, event-name
    // uniqueness here -- to the layer that actually executes the graph.
    std::vector<std::pair<std::string, std::string>> entryPoints;   // {nodeId, eventName}
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
