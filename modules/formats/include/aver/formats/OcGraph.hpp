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

// One declared graph-local variable: `VAR <name> <type> [default]`. Storage is per GraphHost
// INSTANCE at runtime (scripting/csharp/Aver.Graph/GraphVarStore.cs) -- this struct only carries the
// DECLARATION, the same thing a PARAM record carries for an argument.
//
// `type` and `defaultValue` are opaque strings at THIS layer, deliberately, the same division of
// labour LINK's own pin-type-agreement check already follows (see OcGraphData::entryPoints' comment
// for the general pattern): the C# side (scripting/csharp/Aver.Graph/OcGraphParser.cs's VAR-parsing
// block) is the one place that knows PinType is Float/Int/Bool-only, rejects Exec, and falls back an
// unparseable default to the type's zero value. Duplicating that enum and its validation here would
// let the two disagree about what a "valid" VAR looks like; this layer instead accepts whatever is
// syntactically well-formed (a name, a type token, an optional default token) and leaves the semantic
// rules to the layer that actually executes the graph -- exactly what Graph.Validate()'s own
// "duplicate VAR" check already does for uniqueness.
struct OcGraphVariable {
    std::string name;
    std::string type;         // e.g. "float" | "int" | "bool" -- not validated here, see above
    std::string defaultValue; // the literal text after `type`; empty = record carried no 3rd token
};

// One entry in a class graph's COMPONENT TREE: `COMP <id> <Kind> [key=value]...`.
//
// WHAT THIS IS FOR. A graph that carries a CLASS record is a spawnable actor class -- the Aver
// Node analogue of a Blueprint asset. Until this record existed, such a class could describe
// exactly ONE piece of scene content, through `mesh=` on the CLASS line, applied to the class's
// own entity. A first-person character is a body, a held weapon, a muzzle point and a camera --
// four things at four transforms -- and the format could say one of them. COMP is the tree that
// says the rest: each record is one child entity, positioned relative to its parent, carrying
// one kind of scene component.
//
// KIND IS AN OPAQUE STRING HERE, deliberately, exactly as OcGraphVariable::type is. The mapping
// from a kind name to a scene component id (aver::scene::kComponentMeshRenderer and friends)
// lives on the side that actually spawns -- scripting/csharp/Aver.Graph plus the framework's
// component ABI -- and duplicating that table here would let the two disagree about what a valid
// component is. This layer asks only for an id and a kind token.
//
// EVERYTHING ELSE IS A key=value IN extraTokens, VERBATIM AND IN ORDER, which is the same choice
// OcGraphNode::extraTokens makes and for the stronger version of the same reason. A component's
// interesting attributes are kind-specific -- `mesh=` on a Mesh, `fov=` on a Camera, `effect=` on
// Particles -- so a struct with a field per attribute would either be a union of every kind that
// will ever exist or would silently drop the ones it had not heard of. Reading and editing them
// goes through componentAttr/setComponentAttr below, which keeps an edited key in the slot its
// author put it in, so opening a hand-written graph and saving it does not reflow the line.
//
// `parent=` is one of those key=values and not a modelled field, for the same reason -- but it is
// the one this layer validates (see parseOcgraph's post-pass): a parent must name another COMP,
// and the chain must terminate, because a cycle is not a tree and the spawn walk would not return.
struct OcGraphComponent {
    std::string id;    // unique within the graph's components
    std::string kind;  // e.g. "Scene" | "Mesh" | "Light" | "Camera" -- not validated here
    std::vector<std::string> extraTokens;
};

// Reads a `key=value` attribute off a component. Returns an empty view when the key is absent,
// which is indistinguishable from `key=` with an empty value -- a distinction nothing needs, and
// pretending to make it would mean an optional<string> at every call site.
std::string_view componentAttr(const OcGraphComponent& c, std::string_view key);

// Sets `key=value`, REPLACING IN PLACE when the key is already present so the line keeps its
// author's token order, appending at the end otherwise. An empty `value` REMOVES the attribute,
// which is what an editor clearing a field should produce -- writing `key=` instead would leave a
// token whose meaning is "present but blank", and no reader of this format wants that third state.
void setComponentAttr(OcGraphComponent& c, std::string_view key, std::string_view value);

// One user-defined FUNCTION: `FUNC <name> [pure]`, plus its arguments and returns as separate
// `FUNCIN <func> <pin> <type>` / `FUNCOUT <func> <pin> <type>` records.
//
// WHAT A FUNCTION IS HERE. A named, callable subgraph living in the SAME file as the event graph,
// compiled by the C# runtime into its own method and reached by a direct call -- so it can recurse,
// which is the one thing an inlined macro can never do. See scripting/csharp/Aver.Graph/Graph.cs's
// GraphFunction for the semantics; this layer owns only the grammar.
//
// THE BODY IS NOT MODELLED HERE, and does not need to be. A function's nodes are ordinary NODE/PIN/
// LINK records carrying a `func=<name>` attribute naming their owner -- which rides in the same
// OcGraphNode::extraTokens that already carries `param=`/`field=`/`class=`/`var=`, so preserving a
// function body across a load and save needed no change to this reader at all. Only the DECLARATION
// is modelled, and only because the editor has to create and edit one; a record C++ can merely copy
// verbatim is a record the editor cannot author. That is the same argument that moved VAR, COMP and
// COMMENT out of the unknown-record passthrough before it.
//
// PURITY IS A FLAG ON THE RECORD, not something derived from the body, and the reason is in
// GraphFunction's own comment: a function whose body is only a call to another function is pure or
// impure transitively, which no rule that inspects the body's node types can see.
struct OcGraphFunctionPin {
    std::string name;
    std::string type;   // "float" | "int" | "bool" -- opaque here, exactly as OcGraphVariable::type is
};

struct OcGraphFunction {
    std::string name;
    bool pure = false;
    std::vector<OcGraphFunctionPin> inputs;    // in declaration order; the emitted method's arguments
    std::vector<OcGraphFunctionPin> outputs;   // in declaration order
};

// One COMMENT BOX: a titled, coloured rectangle drawn behind the nodes, grouping them by whatever
// the author says it groups them by. `COMMENT <id> <x> <y> <w> <h> <r> <g> <b> <text...>`.
//
// PURELY EDITOR FURNITURE, and unlike every other record here that is the whole point. Nothing
// compiles it, nothing executes it, and a graph stripped of every COMMENT line runs identically --
// which is exactly why it belongs in the FORMAT rather than in a sidecar file the editor keeps to
// itself. A note about why three nodes are wired the way they are is worth as much as the wiring,
// and a note that lives somewhere other than the file it explains is a note that goes stale the
// first time the graph is copied, renamed or committed by anyone who does not know it exists.
//
// THE TEXT IS THE REST OF THE LINE, taken from the RAW line exactly as DESCRIPTION takes its own
// (see parseOcgraph) -- an author writing "# of active spawners" inside a comment box means those
// words, not a trailing source comment. That choice closes the record on the right: there is no
// room after the text for a future `key=value`, so anything this record grows later has to arrive
// as a sibling record rather than a tenth token. That is a deliberate trade and the reason the
// COLOUR is here NOW, spelled out as three required numbers rather than left for later -- colour
// is most of what makes a comment box worth having (a red region and a green region say something
// a title alone does not), and the grammar above has no way to add it afterwards.
//
// The eight numbers are all REQUIRED, so the text always begins at token nine. Reading them
// optionally -- the way NODE reads its x/y -- would make `COMMENT c1 Spawning logic` ambiguous
// between a box at an unknown position and a box at x=Spawning; a record whose own author cannot
// tell which is which is worse than one that is slightly tedious to hand-write.
struct OcGraphComment {
    std::string id;              // unique among comments; not related to node ids
    f64 x = 0.0, y = 0.0;        // canvas position of the top-left corner
    f64 w = 320.0, h = 180.0;    // canvas size
    i32 r = 60, g = 70, b = 90;  // 0..255 tint; the box is drawn translucent over the grid
    std::string text;            // free text, may be empty (an untitled box is legal)
};

// WHAT KIND OF GRAPH A .ocgraph IS.
//
// One extension, several unrelated languages. A gameplay graph is compiled to IL by
// scripting/csharp/Aver.Graph/GraphCompiler.cs and its nodes call the framework; a material graph is
// compiled to HLSL by C++ and its nodes are arithmetic on a surface. They share a grammar -- nodes,
// pins, links -- and nothing else: neither compiler can do anything sane with the other's nodes.
//
// Until this record existed the only thing distinguishing them was which code happened to open the
// file, and the engine has two places that open EVERY .ocgraph under a project without being asked:
// GameApp's class sweep and HostBridge.DeclareGraphClasses. Both would have reached a material graph
// and tried to compile it as gameplay -- not a hypothetical, since the material work landing next
// puts material graphs in the same Content tree.
//
// ABSENT MEANS GAMEPLAY, AND AN UNRECOGNISED NAME MEANS NEITHER. That asymmetry is the point. Every
// .ocgraph written before this record is a gameplay graph and must keep working untouched, so a
// missing DOMAIN cannot be an error. But a file that says `DOMAIN sound` is telling this build it is
// something this build has never heard of, and the safe reading of that is "not mine" -- so it maps
// to Unknown and every consumer skips it, rather than falling back to gameplay and compiling a graph
// whose author explicitly said it was not one.
enum class OcGraphDomain {
    Gameplay,   // no DOMAIN record, or `DOMAIN gameplay`: nodes call the framework, compiled to IL
    Material,   // `DOMAIN material`: nodes shade a surface, compiled to HLSL
    Unknown,    // a DOMAIN this build does not know -- deliberately NOT treated as gameplay
};

// A complete visual scripting graph.
struct OcGraphData {
    int version = 1;
    std::string name;        // Graph name; empty = "untitled"
    std::string description; // Optional human-readable description

    // Declared via a top-level `DOMAIN <name>` record; empty when the file has none, which is every
    // graph written before the record existed and means Gameplay. See OcGraphDomain above for what
    // the distinction is for and why an UNKNOWN name is not the same as an absent one.
    //
    // A STRING HERE, an enum only at the point of use, for the reason OcGraphVariable::type and
    // OcGraphComponent::kind are strings too: this layer owns the grammar, not the vocabulary. A file
    // naming a domain this build has never heard of round-trips through a load and save with its name
    // intact -- an older editor opening a newer project must not quietly rewrite `DOMAIN vfx` into
    // something it does prefer -- while ocGraphDomainOf() below is where a consumer asks the only
    // question it actually has, which is "is this one mine".
    std::string domain;

    std::vector<OcGraphNode> nodes;
    std::vector<OcGraphLink> links;

    // Declared via top-level `VAR <name> <type> [default]` records, in file order. See
    // OcGraphVariable's own comment for what each field means and why type/default are opaque
    // strings here.
    //
    // A RECORD MODELLED HERE, NOT AN UNKNOWN ONE -- and that is a change, not the original design.
    // VAR used to be entirely invisible to this reader: it fell through classifyLine into
    // OwnedLineKind::Other and rode through a save unread, unmodified, the same path a comment or a
    // future record type this format has never heard of takes (see tests/formats/src/OcGraphTest.cpp's
    // testVarRecordsSurviveRoundTrip, whose own comment used to say exactly that -- read it for the
    // history if this field's presence here is confusing). That was sufficient for the editor to not
    // silently DESTROY a graph's variables on save, but it meant nothing in C++ could ever answer "what
    // variables does this graph declare" -- which is the one question a Variables panel (declare/
    // rename/retype/delete, and a var= picker on GetVar/SetVar) cannot avoid asking. Modelling VAR here
    // is what makes that panel possible; classifyLine below gained its own `OwnedLineKind::Var` case
    // and writeOcgraph gained its own placedVar/varBlock pair, the identical shape ENTRY and OUT
    // already use, so an existing VAR-bearing file still round-trips byte-for-byte -- verified by the
    // very same testVarRecordsSurviveRoundTrip, now asserting real fields on the parsed struct instead
    // of only asserting the raw text survived.
    std::vector<OcGraphVariable> variables;

    // Declared via top-level `COMP <id> <Kind> [key=value]...` records, in file order. See
    // OcGraphComponent above for what a component IS and why its attributes are opaque tokens.
    //
    // FILE ORDER IS NOT TREE ORDER and this vector does not sort itself into one. A component may
    // name a parent declared below it -- the same forward reference LINK and ENTRY already allow --
    // so anything walking this as a tree resolves parents by id rather than assuming a parent
    // precedes its children.
    std::vector<OcGraphComponent> components;

    // Declared via top-level `FUNC` / `FUNCIN` / `FUNCOUT` records, in file order. See
    // OcGraphFunction for what one IS and why only the declaration lives here while the body rides
    // in ordinary nodes tagged `func=`.
    std::vector<OcGraphFunction> functions;

    // Comment boxes, in file order. See OcGraphComment for what one IS and why it carries its
    // colour in the record. Modelled here rather than left in the unknown-record passthrough for
    // one reason: the editor has to MOVE and RESIZE them, and a record it can only copy verbatim
    // is a record it cannot edit -- the same argument that moved VAR out of `Other` above.
    std::vector<OcGraphComment> comments;

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

// Which domain a graph belongs to. Case-insensitive; an ABSENT record is Gameplay and an
// unrecognised name is Unknown -- see OcGraphDomain for why those two are not the same answer.
OcGraphDomain ocGraphDomainOf(const OcGraphData& g);

// The canonical lower-case spelling to WRITE for a domain. Unknown has no spelling of its own (a
// graph that carries one carries its author's text verbatim in OcGraphData::domain), so it returns
// an empty view and a caller writing one uses the string it parsed.
std::string_view ocGraphDomainName(OcGraphDomain d);

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
