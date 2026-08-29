// The .ocgraph -> HLSL material compiler. See MaterialGraphHlsl.hpp for why it lives here.
//
// THREE THINGS DECIDE THE SHAPE OF THIS FILE.
//
// 1. IT IS PULL, NOT PUSH. Emission starts at the MaterialOutput node and walks BACKWARDS along the
//    links, emitting a node only when something actually reads it. A topological sort over every
//    node would emit unreachable ones too -- and an author's half-finished experiment left floating
//    on the canvas is not a compile error in any material editor worth using. Dead-code elimination
//    is a property of walking backwards, not a pass.
//
// 2. EVERY EMITTER RETURNS AN EXPRESSION, never void. This is the one lesson taken from
//    scripting/csharp/Aver.Graph/GraphCompiler.cs, whose dataflow half is fused to IL emission --
//    every emitter there returns void and communicates through the CLR evaluation stack, which is
//    why its node cases cannot be composed or tested separately. Here a node emits one SSA line and
//    hands back the name it bound, so composition is function composition and nothing is implicit.
//
// 3. A TYPE IS THE PIN'S ARITY, and promotion is a function of the IR rather than of the pin string.
//    That is what lets a scalar drive a float3 input without the format learning a type lattice:
//    OcGraphPin::type stays the free string it has always been, and the rules live in one place
//    here. See widen() for exactly which promotions are legal and which are refused.
#include "aver/pbr/MaterialGraphHlsl.hpp"

#include "aver/formats/OcGraph.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace aver::pbr {
namespace {

// ---------------------------------------------------------------------------------- types

// A value's ARITY, which is all this compiler needs to know about a type. There is no bool, no int
// and no texture handle: a material graph's values are float vectors, and the one thing that varies
// is how many components they have.
enum class MatType : u32 { Float = 1, Float2 = 2, Float3 = 3, Float4 = 4 };

u32 arity(MatType t) { return static_cast<u32>(t); }
MatType typeOfArity(u32 n) { return static_cast<MatType>(n); }

// The NODE an author would reach for to build a value of this width -- not the same string as the
// HLSL type, because a diagnostic that says "insert a Makefloat3 node" names something that is not
// in the palette, and an author who copies it exactly gets a second error.
const char* makeNodeFor(MatType t) {
    switch (t) {
    case MatType::Float2: return "MakeFloat2";
    case MatType::Float3: return "MakeFloat3";
    case MatType::Float4: return "MakeFloat4";
    case MatType::Float:  break;
    }
    return "MakeFloat2";
}

const char* hlslType(MatType t) {
    switch (t) {
    case MatType::Float:  return "float";
    case MatType::Float2: return "float2";
    case MatType::Float3: return "float3";
    case MatType::Float4: return "float4";
    }
    return "float";
}

bool ciEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

// The declared type of a pin. Anything this build does not recognise reads as a scalar, which is
// the conservative answer: a scalar widens to every other type, so an unfamiliar pin string costs
// an author a promotion rather than a compile error.
MatType typeFromPin(std::string_view s) {
    if (ciEquals(s, "float2")) return MatType::Float2;
    if (ciEquals(s, "float3")) return MatType::Float3;
    if (ciEquals(s, "float4")) return MatType::Float4;
    return MatType::Float;
}

// One resolved value: the HLSL name or expression that holds it, and what it is.
struct Value {
    std::string expr;
    MatType type = MatType::Float;
};

// ---------------------------------------------------------------------------------- literals

// Formats one float so the emitted text is stable and re-parses exactly. %.9g round-trips a
// binary32 and never emits an exponent-free integer that HLSL would read as an int -- hence the
// explicit `.0` repair below, without which `float3(1, 0, 0)` compiles but `pow(2, x)` picks the
// integer overload and changes the result.
std::string floatLit(f64 x) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.9g", x);
    std::string s = buf;
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
        s.find("inf") == std::string::npos && s.find("nan") == std::string::npos) {
        s += ".0";
    }
    return s;
}

f64 parseF64(std::string_view s, f64 fallback) {
    if (s.empty()) return fallback;
    const std::string t(s);
    try {
        usize used = 0;
        const f64 v = std::stod(t, &used);
        return used == 0 ? fallback : v;
    } catch (...) {
        return fallback;
    }
}

// Splits a comma-separated literal ("0.1,0.3,0.36") into components. A missing component is 0, and
// a SINGLE component splats -- so `1` on a float3 pin is white, which is what an author typing one
// number into a colour means.
std::vector<f64> parseComponents(std::string_view s, u32 want) {
    std::vector<f64> out;
    usize b = 0;
    while (b <= s.size() && out.size() < want) {
        const usize e = std::min(s.find(',', b), s.size());
        out.push_back(parseF64(s.substr(b, e - b), 0.0));
        if (e >= s.size()) break;
        b = e + 1;
    }
    // THE COPY IS LOAD-BEARING. `out.assign(want, out[0])` reads a reference INTO the vector it is
    // about to reallocate: one component means capacity 1, three wanted means a new buffer, and the
    // old one -- which is where out[0] lives -- is freed while being read from. It does not crash.
    // It corrupts the heap, and the NEXT allocation anywhere in the process deadlocks on the heap
    // lock, which presents as a hang with no CPU, no exception and nothing in the event log. Cost an
    // hour to find; the fix is one named local.
    if (out.size() == 1 && want > 1) {
        const f64 only = out[0];
        out.assign(want, only);
    }
    while (out.size() < want) out.push_back(0.0);
    return out;
}

// A vector literal of `want` components from a comma list.
std::string vectorLit(std::string_view text, MatType want) {
    const std::vector<f64> c = parseComponents(text, arity(want));
    if (want == MatType::Float) return floatLit(c[0]);
    std::string s = hlslType(want);
    s += "(";
    for (usize i = 0; i < c.size(); ++i) { if (i) s += ", "; s += floatLit(c[i]); }
    s += ")";
    return s;
}

// ---------------------------------------------------------------------------------- the emitter

// Every node type this build understands, and what it emits. Adding one means adding a case to
// emitNode below AND a palette entry in sandbox/src/GraphNodeDefs.hpp -- the test asserts the two
// agree, so neither can drift alone.
struct Emitter {
    const fmt::OcGraphData& g;
    std::string out;                              // accumulated statements
    std::string err;                              // first failure wins; emission stops at it
    std::vector<std::string> stack;               // node ids being resolved, for cycle detection
    std::map<std::string, Value> done;            // "node\0pin" -> already-emitted value
    u32 nextTemp = 0;

    explicit Emitter(const fmt::OcGraphData& graph) : g(graph) {}

    bool failed() const { return !err.empty(); }

    void fail(const std::string& message) {
        if (err.empty()) err = message;
    }

    const fmt::OcGraphNode* findNode(std::string_view id) const {
        for (const fmt::OcGraphNode& n : g.nodes)
            if (ciEquals(n.id, id)) return &n;
        return nullptr;
    }

    static const fmt::OcGraphPin* findPin(const fmt::OcGraphNode& n, std::string_view pin, bool output) {
        for (const fmt::OcGraphPin& p : n.pins)
            if (p.isOutput == output && ciEquals(p.name, pin)) return &p;
        return nullptr;
    }

    // The link feeding one input pin, or null. The FIRST such link: canConnectPins already refuses a
    // second, and a hand-written file with two is taking the one it wrote first rather than being
    // rejected -- the same forgiveness the reader shows a duplicate NAME record.
    const fmt::OcGraphLink* linkInto(std::string_view node, std::string_view pin) const {
        for (const fmt::OcGraphLink& l : g.links)
            if (ciEquals(l.destNode, node) && ciEquals(l.destPin, pin)) return &l;
        return nullptr;
    }

    std::string temp(std::string_view node, std::string_view pin) {
        std::string s = "n";
        s += std::to_string(nextTemp++);
        s += "_";
        for (const char c : node) s += (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                       (c >= '0' && c <= '9') ? c : '_';
        s += "_";
        for (const char c : pin) s += (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                      (c >= '0' && c <= '9') ? c : '_';
        return s;
    }

    // Binds `expr` to a fresh SSA name and returns it. One statement per line, indented for a
    // switch arm.
    Value bind(std::string_view node, std::string_view pin, MatType type, const std::string& expr) {
        const std::string name = temp(node, pin);
        out += "        ";
        out += hlslType(type);
        out += " ";
        out += name;
        out += " = ";
        out += expr;
        out += ";\n";
        return Value{name, type};
    }

    // Promotes a value to `want`, or fails naming the node that asked.
    //
    // A SCALAR SPLATS AND A WIDER VECTOR TRUNCATES; a float2 does NOT become a float3. The
    // asymmetry is deliberate: splatting a scalar and dropping components are both things an author
    // writes on purpose in HLSL every day, whereas inventing a third component out of a float2 is
    // the compiler guessing, and it would guess zero -- turning a UV wired into a colour into
    // something silently blue-free rather than something the editor refused to connect.
    std::string widen(const Value& v, MatType want, std::string_view atNode) {
        if (v.type == want) return v.expr;
        if (v.type == MatType::Float) {
            // A scalar cast to a vector splats in HLSL, which is exactly the intent.
            return std::string("((") + hlslType(want) + ")(" + v.expr + "))";
        }
        if (arity(v.type) > arity(want)) {
            static const char* kSwizzle[5] = {"", ".x", ".xy", ".xyz", ""};
            return "(" + v.expr + ")" + kSwizzle[arity(want)];
        }
        fail("node '" + std::string(atNode) + "': cannot widen a " + hlslType(v.type) + " to a " +
             hlslType(want) + " -- only a scalar splats. Insert a " + makeNodeFor(want) +
             " node and say what the extra component should be.");
        return v.expr;
    }

    // A `key=value` attribute off the NODE line, or empty. The same extraTokens the gameplay
    // catalog already carries `param=`/`class=` in -- a material node that needs to name something
    // (which texture slot, which swizzle) says it here rather than on a pin, because it is a
    // property of the node and not a value that can be computed.
    std::string_view nodeAttr(const fmt::OcGraphNode& n, std::string_view key) const {
        for (const std::string& tok : n.extraTokens) {
            const usize eq = tok.find('=');
            if (eq == std::string::npos) continue;
            if (ciEquals(std::string_view(tok).substr(0, eq), key))
                return std::string_view(tok).substr(eq + 1);
        }
        return {};
    }

    // The UV a sampling node reads: whatever is wired into its `uv` pin, or THE SURFACE'S OWN when
    // nothing is. Unlinked meaning (0,0) -- what every other input pin means -- would make an
    // unwired texture node sample one texel and return a flat colour, which looks like a broken
    // texture rather than like the obvious default. Every material editor makes this same
    // exception, and it is the only place in this emitter where an unlinked pin is not a literal.
    Value uvInput(const fmt::OcGraphNode& n) {
        if (linkInto(n.id, "uv")) return input(n, "uv", MatType::Float2);
        return Value{"uv", MatType::Float2};
    }

    // The value on one INPUT pin: whatever is linked into it, promoted; else the pin's own literal;
    // else zero.
    Value input(const fmt::OcGraphNode& n, std::string_view pin, MatType want) {
        if (const fmt::OcGraphLink* l = linkInto(n.id, pin)) {
            const Value v = resolve(l->sourceNode, l->sourcePin);
            if (failed()) return Value{"0.0", want};
            return Value{widen(v, want, n.id), want};
        }
        const fmt::OcGraphPin* p = findPin(n, pin, false);
        const std::string_view lit = p ? std::string_view(p->defaultValue) : std::string_view();
        return Value{vectorLit(lit, want), want};
    }

    // The value on one OUTPUT pin, emitting the node that produces it if it has not been emitted.
    Value resolve(std::string_view nodeId, std::string_view pin) {
        const std::string key = std::string(nodeId) + '\0' + std::string(pin);
        if (const auto it = done.find(key); it != done.end()) return it->second;

        const fmt::OcGraphNode* n = findNode(nodeId);
        if (!n) {
            fail("a link names node '" + std::string(nodeId) + "', which the graph does not declare");
            return Value{"0.0", MatType::Float};
        }
        for (const std::string& s : stack) {
            if (ciEquals(s, nodeId)) {
                fail("data cycle through node '" + std::string(nodeId) +
                     "': a material is evaluated once per pixel, so a value cannot depend on itself");
                return Value{"0.0", MatType::Float};
            }
        }
        // A BACKSTOP UNDER THE CYCLE CHECK ABOVE. The cycle check catches a value that depends on
        // itself; this catches a graph that is merely absurdly deep, which would otherwise overrun
        // the C++ stack rather than being reported. A material a person drew is a handful of nodes
        // deep; anything past this is a generated or corrupt file.
        if (stack.size() > 256) {
            fail("node '" + std::string(nodeId) + "': this graph nests more than 256 levels deep, "
                 "which no hand-authored material does -- refusing rather than overrunning the stack");
            return Value{"0.0", MatType::Float};
        }
        stack.emplace_back(nodeId);
        const Value v = emitNode(*n, pin);
        stack.pop_back();
        if (!failed()) done[key] = v;
        return v;
    }

    // ------------------------------------------------------------------ one node

    // The declared type of an output pin, defaulting to scalar.
    static MatType outType(const fmt::OcGraphNode& n, std::string_view pin) {
        const fmt::OcGraphPin* p = findPin(n, pin, true);
        return p ? typeFromPin(p->type) : MatType::Float;
    }

    // The widest of a node's linked inputs, which is what a GENERIC operator returns. An input that
    // is only a literal does not count towards the width: `Multiply` with 0.5 typed into b is a
    // scale, and letting the literal decide would make it a scalar multiply of a colour.
    MatType widestInput(const fmt::OcGraphNode& n, std::initializer_list<const char*> pins) {
        u32 w = 1;
        for (const char* p : pins) {
            const fmt::OcGraphLink* l = linkInto(n.id, p);
            if (!l) continue;
            const fmt::OcGraphNode* src = findNode(l->sourceNode);
            if (!src) continue;
            w = std::max(w, arity(outType(*src, l->sourcePin)));
        }
        return typeOfArity(w);
    }

    // A binary operator over the widest of its two inputs.
    Value binary(const fmt::OcGraphNode& n, std::string_view pin, const char* op) {
        const MatType t = widestInput(n, {"a", "b"});
        const Value a = input(n, "a", t);
        const Value b = input(n, "b", t);
        return bind(n.id, pin, t, "(" + a.expr + ") " + op + " (" + b.expr + ")");
    }

    // A call over the widest of the named inputs, all promoted to it.
    Value call(const fmt::OcGraphNode& n, std::string_view pin, const char* fn,
               std::initializer_list<const char*> pins) {
        const MatType t = widestInput(n, pins);
        std::string e = fn;
        e += "(";
        bool first = true;
        for (const char* p : pins) {
            if (!first) e += ", ";
            first = false;
            e += input(n, p, t).expr;
        }
        e += ")";
        return bind(n.id, pin, t, e);
    }

    Value emitNode(const fmt::OcGraphNode& n, std::string_view pin) {
        const std::string& ty = n.type;

        // -- literals: the value rides on the OUTPUT pin's default, the idiom ConstFloat already
        //    established in the gameplay catalog. --
        if (ciEquals(ty, "ConstFloat") || ciEquals(ty, "ConstFloat2") ||
            ciEquals(ty, "ConstFloat3") || ciEquals(ty, "ConstFloat4")) {
            const MatType t = ciEquals(ty, "ConstFloat")  ? MatType::Float
                            : ciEquals(ty, "ConstFloat2") ? MatType::Float2
                            : ciEquals(ty, "ConstFloat3") ? MatType::Float3
                                                          : MatType::Float4;
            const fmt::OcGraphPin* p = findPin(n, pin, true);
            const std::string_view lit = p ? std::string_view(p->defaultValue) : std::string_view();
            return bind(n.id, pin, t, vectorLit(lit, t));
        }

        // -- what the renderer knows about this pixel. These read the parameters averEvalMaterial
        //    was handed; nothing else in a material graph reaches outside itself. --
        if (ciEquals(ty, "UV"))            return bind(n.id, pin, MatType::Float2, "uv");
        if (ciEquals(ty, "WorldPosition")) return bind(n.id, pin, MatType::Float3, "v.wpos");
        if (ciEquals(ty, "WorldNormal"))   return bind(n.id, pin, MatType::Float3, "v.N");
        if (ciEquals(ty, "ViewDirection")) return bind(n.id, pin, MatType::Float3, "v.V");

        // -- arithmetic, generic over width --
        if (ciEquals(ty, "Add"))      return binary(n, pin, "+");
        if (ciEquals(ty, "Subtract")) return binary(n, pin, "-");
        if (ciEquals(ty, "Multiply")) return binary(n, pin, "*");
        if (ciEquals(ty, "Divide"))   return binary(n, pin, "/");

        if (ciEquals(ty, "Lerp"))     return call(n, pin, "lerp", {"a", "b", "t"});
        if (ciEquals(ty, "Clamp"))    return call(n, pin, "clamp", {"x", "lo", "hi"});
        if (ciEquals(ty, "Min"))      return call(n, pin, "min", {"a", "b"});
        if (ciEquals(ty, "Max"))      return call(n, pin, "max", {"a", "b"});
        if (ciEquals(ty, "Power"))    return call(n, pin, "pow", {"a", "b"});
        if (ciEquals(ty, "Saturate")) return call(n, pin, "saturate", {"x"});
        if (ciEquals(ty, "Abs"))      return call(n, pin, "abs", {"x"});
        if (ciEquals(ty, "Frac"))     return call(n, pin, "frac", {"x"});
        if (ciEquals(ty, "Floor"))    return call(n, pin, "floor", {"x"});
        if (ciEquals(ty, "Sin"))      return call(n, pin, "sin", {"x"});
        if (ciEquals(ty, "Cos"))      return call(n, pin, "cos", {"x"});
        if (ciEquals(ty, "Normalize")) return call(n, pin, "normalize", {"x"});

        if (ciEquals(ty, "OneMinus")) {
            const MatType t = widestInput(n, {"x"});
            return bind(n.id, pin, t, "1.0 - (" + input(n, "x", t).expr + ")");
        }

        // -- reductions: these return a SCALAR whatever their inputs are, so they cannot go through
        //    call() above, which returns the widest input type. --
        if (ciEquals(ty, "Dot")) {
            const MatType t = widestInput(n, {"a", "b"});
            const Value a = input(n, "a", t);
            const Value b = input(n, "b", t);
            return bind(n.id, pin, MatType::Float, "dot(" + a.expr + ", " + b.expr + ")");
        }
        if (ciEquals(ty, "Length")) {
            const MatType t = widestInput(n, {"x"});
            return bind(n.id, pin, MatType::Float, "length(" + input(n, "x", t).expr + ")");
        }

        // -- assembling and taking apart --
        if (ciEquals(ty, "MakeFloat2") || ciEquals(ty, "MakeFloat3") || ciEquals(ty, "MakeFloat4")) {
            const MatType t = ciEquals(ty, "MakeFloat2") ? MatType::Float2
                            : ciEquals(ty, "MakeFloat3") ? MatType::Float3
                                                         : MatType::Float4;
            static const char* kNames[4] = {"x", "y", "z", "w"};
            std::string e = hlslType(t);
            e += "(";
            for (u32 i = 0; i < arity(t); ++i) {
                if (i) e += ", ";
                e += input(n, kNames[i], MatType::Float).expr;
            }
            e += ")";
            return bind(n.id, pin, t, e);
        }
        if (ciEquals(ty, "Split")) {
            // One statement per component asked for, so a Split whose z is unread emits no z.
            const MatType t = widestInput(n, {"x"});
            const Value in = input(n, "x", t);
            const char comp = pin.empty() ? 'x' : static_cast<char>(pin[0] | 32);
            if (comp != 'x' && comp != 'y' && comp != 'z' && comp != 'w') {
                fail("node '" + n.id + "': Split has no output pin '" + std::string(pin) +
                     "' -- its outputs are x, y, z and w");
                return Value{"0.0", MatType::Float};
            }
            const u32 index = comp == 'x' ? 0u : comp == 'y' ? 1u : comp == 'z' ? 2u : 3u;
            if (index >= arity(t)) {
                fail("node '" + n.id + "': Split cannot take ." + std::string(1, comp) + " from a " +
                     hlslType(t));
                return Value{"0.0", MatType::Float};
            }
            return bind(n.id, pin, MatType::Float, "(" + in.expr + ")." + std::string(1, comp));
        }

        // -- more of the renderer's own knowledge --
        if (ciEquals(ty, "CameraPosition")) return bind(n.id, pin, MatType::Float3, "gCamPos.xyz");
        // The object's origin in world space: row 3 of its transform. Useful for anything that
        // should vary per INSTANCE rather than per pixel -- a per-object colour, a phase offset --
        // which a graph has no other way to reach.
        if (ciEquals(ty, "ObjectPosition")) return bind(n.id, pin, MatType::Float3, "gWorld[3].xyz");

        // -- more arithmetic --
        if (ciEquals(ty, "Sqrt"))  return call(n, pin, "sqrt", {"x"});
        if (ciEquals(ty, "Ceil"))  return call(n, pin, "ceil", {"x"});
        if (ciEquals(ty, "Sign"))  return call(n, pin, "sign", {"x"});
        if (ciEquals(ty, "Exp"))   return call(n, pin, "exp", {"x"});
        if (ciEquals(ty, "Log"))   return call(n, pin, "log", {"x"});
        if (ciEquals(ty, "Tan"))   return call(n, pin, "tan", {"x"});
        if (ciEquals(ty, "Modulo")) return call(n, pin, "fmod", {"a", "b"});
        if (ciEquals(ty, "Step"))   return call(n, pin, "step", {"edge", "x"});
        if (ciEquals(ty, "Smoothstep")) return call(n, pin, "smoothstep", {"edge0", "edge1", "x"});

        // Remap a range onto another. Emitted as arithmetic rather than as a call because HLSL has
        // no intrinsic for it, and every author writes this by hand eventually.
        if (ciEquals(ty, "Remap")) {
            const MatType t = widestInput(n, {"x", "inMin", "inMax", "outMin", "outMax"});
            const Value x  = input(n, "x", t);
            const Value i0 = input(n, "inMin", t);
            const Value i1 = input(n, "inMax", t);
            const Value o0 = input(n, "outMin", t);
            const Value o1 = input(n, "outMax", t);
            return bind(n.id, pin, t,
                        "(" + o0.expr + ") + (((" + x.expr + ") - (" + i0.expr + ")) / max((" +
                        i1.expr + ") - (" + i0.expr + "), 1e-6)) * ((" + o1.expr + ") - (" +
                        o0.expr + "))");
        }

        // -- more vector work --
        if (ciEquals(ty, "Cross")) {
            const Value a = input(n, "a", MatType::Float3);
            const Value b = input(n, "b", MatType::Float3);
            return bind(n.id, pin, MatType::Float3, "cross(" + a.expr + ", " + b.expr + ")");
        }
        if (ciEquals(ty, "Reflect")) {
            const Value i = input(n, "i", MatType::Float3);
            const Value nn = input(n, "n", MatType::Float3);
            return bind(n.id, pin, MatType::Float3, "reflect(" + i.expr + ", " + nn.expr + ")");
        }
        if (ciEquals(ty, "Distance")) {
            const MatType t = widestInput(n, {"a", "b"});
            const Value a = input(n, "a", t);
            const Value b = input(n, "b", t);
            return bind(n.id, pin, MatType::Float, "distance(" + a.expr + ", " + b.expr + ")");
        }
        if (ciEquals(ty, "BlendNormals")) {
            const Value a = input(n, "a", MatType::Float3);
            const Value b = input(n, "b", MatType::Float3);
            return bind(n.id, pin, MatType::Float3, "averBlendNormals(" + a.expr + ", " + b.expr + ")");
        }

        // An arbitrary component selection, named by a `mask=` attribute. The one node whose OUTPUT
        // WIDTH is decided by an attribute rather than by its pins, which is exactly why the mask is
        // validated here rather than trusted: a typo would otherwise reach dxc as a swizzle on the
        // wrong arity and fail with a message about a generated identifier nobody wrote.
        if (ciEquals(ty, "Swizzle")) {
            const std::string_view mask = nodeAttr(n, "mask");
            if (mask.empty() || mask.size() > 4) {
                fail("node '" + n.id + "': Swizzle needs a mask= of one to four components, e.g. "
                     "mask=xyz or mask=rrr");
                return Value{"0.0", MatType::Float};
            }
            const MatType in = widestInput(n, {"x"});
            std::string sw;
            for (const char raw : mask) {
                const char c = static_cast<char>(raw | 32);
                const u32 index = c == 'x' || c == 'r' ? 0u : c == 'y' || c == 'g' ? 1u
                                : c == 'z' || c == 'b' ? 2u : c == 'w' || c == 'a' ? 3u : 4u;
                if (index >= 4u) {
                    fail("node '" + n.id + "': Swizzle mask '" + std::string(mask) +
                         "' has a component that is not x/y/z/w or r/g/b/a");
                    return Value{"0.0", MatType::Float};
                }
                if (index >= arity(in)) {
                    fail("node '" + n.id + "': Swizzle mask '" + std::string(mask) + "' reads a "
                         "component a " + hlslType(in) + " does not have");
                    return Value{"0.0", MatType::Float};
                }
                sw += "xyzw"[index];
            }
            return bind(n.id, pin, typeOfArity(static_cast<u32>(mask.size())),
                        "(" + input(n, "x", in).expr + ")." + sw);
        }

        // -- UV --
        if (ciEquals(ty, "TilingOffset")) {
            const Value t = input(n, "tiling", MatType::Float2);
            const Value o = input(n, "offset", MatType::Float2);
            return bind(n.id, pin, MatType::Float2,
                        "(" + uvInput(n).expr + ") * (" + t.expr + ") + (" + o.expr + ")");
        }
        if (ciEquals(ty, "Rotator")) {
            const Value c = input(n, "centre", MatType::Float2);
            const Value a = input(n, "angle", MatType::Float);
            return bind(n.id, pin, MatType::Float2,
                        "averRotateUv(" + uvInput(n).expr + ", " + c.expr + ", " + a.expr + ")");
        }

        // -- procedural --
        if (ciEquals(ty, "Noise")) {
            const Value s = input(n, "scale", MatType::Float);
            return bind(n.id, pin, MatType::Float,
                        "averValueNoise((" + uvInput(n).expr + ") * (" + s.expr + "))");
        }
        if (ciEquals(ty, "Checker")) {
            const Value s = input(n, "scale", MatType::Float);
            return bind(n.id, pin, MatType::Float,
                        "averChecker((" + uvInput(n).expr + ") * (" + s.expr + "))");
        }

        // -- the map this material already declares, at any UV --
        if (ciEquals(ty, "SampleTexture")) {
            static const char* kSlotNames[8] = {"basecolor", "metalrough", "normal", "occlusion",
                                                "emissive", "layer1basecolor", "layer1metalrough",
                                                "layer1normal"};
            const std::string_view want = nodeAttr(n, "slot");
            u32 slot = 8;
            for (u32 i = 0; i < 8; ++i) if (ciEquals(want, kSlotNames[i])) { slot = i; break; }
            if (slot == 8) {
                std::string known;
                for (u32 i = 0; i < 8; ++i) { if (i) known += ", "; known += kSlotNames[i]; }
                fail("node '" + n.id + "': SampleTexture needs a slot= naming one of the material's "
                     "own texture slots (" + known + "); it said '" + std::string(want) + "'");
                return Value{"0.0", MatType::Float};
            }
            // ONE SAMPLE, THREE OUTPUT PINS, AND THE FETCH IS MEMOISED ON THE NODE.
            //
            // resolve()'s own memo is keyed by (node, PIN), which is right for every other node --
            // two pins of a Split are two different values -- but wrong here: .rgb and .a are two
            // views of ONE texture fetch, and keying the fetch per pin emitted `averSampleSlot(...)`
            // twice for a material reading albedo colour and albedo alpha, which is two real texture
            // reads at runtime for one texel. This comment used to claim the opposite; the test that
            // counts the calls found it, which is exactly what a test that reads the emitted text is
            // for. The synthetic pin name below cannot collide with a real one -- no .ocgraph pin
            // starts with '$' -- so the fetch is bound once and every output pin swizzles that name.
            const std::string fetchKey = n.id + '\0' + "$fetch";
            Value fetch;
            if (const auto it = done.find(fetchKey); it != done.end()) {
                fetch = it->second;
            } else {
                fetch = bind(n.id, "fetch", MatType::Float4,
                             "averSampleSlot(" + std::to_string(slot) + "u, " + uvInput(n).expr + ")");
                done[fetchKey] = fetch;
            }
            if (ciEquals(pin, "a"))    return bind(n.id, pin, MatType::Float,  fetch.expr + ".a");
            if (ciEquals(pin, "rgba")) return Value{fetch.expr, MatType::Float4};
            return bind(n.id, pin, MatType::Float3, fetch.expr + ".rgb");
        }

        // -- utility --
        // Grazing-angle falloff, from the geometric normal and the view vector the renderer handed
        // in. Deliberately NOT from the normal-mapped one: this runs before a graph has decided what
        // the normal is, and reading a normal the same graph is still computing is a dependency the
        // emitter cannot order.
        if (ciEquals(ty, "Fresnel")) {
            const Value p = input(n, "power", MatType::Float);
            return bind(n.id, pin, MatType::Float,
                        "pow(saturate(1.0 - saturate(dot(v.N, v.V))), " + p.expr + ")");
        }
        // A branchless select. lerp+step rather than `?:` because the condition here is a VALUE
        // comparison over vectors of any width, and HLSL's ternary on a vector condition selects
        // per component only where both arms are the same width -- which the widening below
        // guarantees, but the lerp form guarantees it without depending on that reading.
        if (ciEquals(ty, "If")) {
            const MatType t = widestInput(n, {"ifTrue", "ifFalse"});
            const Value a = input(n, "a", MatType::Float);
            const Value b = input(n, "b", MatType::Float);
            const Value yes = input(n, "ifTrue", t);
            const Value no  = input(n, "ifFalse", t);
            return bind(n.id, pin, t,
                        "lerp(" + no.expr + ", " + yes.expr + ", step(" + b.expr + ", " + a.expr + "))");
        }

        // A NODE TYPE THIS BUILD HAS NO EMITTER FOR IS AN ERROR, not something to skip. Skipping it
        // would leave every value downstream reading zero, and the material would render as a black
        // surface with nothing anywhere saying why.
        fail("node '" + n.id + "': no material emitter for node type '" + n.type +
             "'. Either it is a gameplay node in a material graph, or this build is older than the "
             "graph.");
        return Value{"0.0", MatType::Float};
    }
};

// Which AverAuthored field each MaterialOutput input pin drives, and how wide it is.
struct OutputField {
    const char* pin;
    const char* field;
    MatType type;
};

constexpr OutputField kOutputFields[] = {
    {"BaseColor",   "baseColor",   MatType::Float3},
    {"Metallic",    "metallic",    MatType::Float},
    {"Roughness",   "roughness",   MatType::Float},
    {"Normal",      "normalTS",    MatType::Float3},
    {"Emissive",    "emissive",    MatType::Float3},
    {"Occlusion",   "occlusion",   MatType::Float},
    {"Opacity",     "opacity",     MatType::Float},
    {"AlphaCutoff", "alphaCutoff", MatType::Float},
    {"SubsurfaceWeight", "subsurfaceWeight", MatType::Float},
    {"SubsurfaceRadius", "subsurfaceRadius", MatType::Float},
    {"Ior",          "ior",          MatType::Float},
    {"Transmission", "transmission", MatType::Float},
};

} // namespace

MaterialGraphBody compileMaterialGraph(const fmt::OcGraphData& g) {
    MaterialGraphBody r;

    if (fmt::ocGraphDomainOf(g) != fmt::OcGraphDomain::Material) {
        r.error = "this graph's DOMAIN is '" +
                  (g.domain.empty() ? std::string("gameplay (no DOMAIN record)") : g.domain) +
                  "', not 'material' -- nothing here can be compiled into a shader";
        return r;
    }

    // Exactly one sink. Two would each be a complete description of the surface, and nothing in the
    // file would say which one wins.
    const fmt::OcGraphNode* out = nullptr;
    for (const fmt::OcGraphNode& n : g.nodes) {
        if (!ciEquals(n.type, "MaterialOutput")) continue;
        if (out) {
            r.error = "two MaterialOutput nodes ('" + out->id + "' and '" + n.id +
                      "') -- a material graph describes one surface, so it has one output";
            return r;
        }
        out = &n;
    }
    if (!out) {
        r.error = "no MaterialOutput node -- a material graph needs the one node that says what the "
                  "surface IS; without it nothing is connected to anything the renderer reads";
        return r;
    }

    Emitter e(g);

    // ONLY THE FIELDS THE AUTHOR ACTUALLY DROVE ARE WRITTEN, and that is the whole reason a graph
    // can be partial. Everything else keeps what averStockAuthored() already put there -- the b2
    // factors and the material's own maps -- so a graph that says nothing but "base colour is red"
    // still has this material's roughness, its normal map and its alpha. Driven means LINKED, or a
    // literal typed into the pin; MaterialOutput's pins carry no defaults in the palette precisely
    // so that a non-empty one is unambiguously something a person wrote.
    u32 driven = 0;
    for (const OutputField& f : kOutputFields) {
        const fmt::OcGraphLink* link = e.linkInto(out->id, f.pin);
        const fmt::OcGraphPin* p = Emitter::findPin(*out, f.pin, false);
        const bool typed = p && !p->defaultValue.empty();
        if (!link && !typed) continue;

        const Value v = e.input(*out, f.pin, f.type);
        if (e.failed()) { r.error = e.err; return r; }
        e.out += "        a.";
        e.out += f.field;
        e.out += " = ";
        e.out += v.expr;
        e.out += ";\n";
        ++driven;
    }

    if (driven == 0) {
        r.error = "the MaterialOutput node has nothing connected to it, so this graph would compile "
                  "to a material identical to the stock one -- connect at least one of its inputs";
        return r;
    }

    r.ok = true;
    r.hlsl = e.out;
    return r;
}

std::string materialGraphHlsl(const std::vector<MaterialGraphEntry>& entries) {
    std::string s;
    s += "// ---- generated from .ocgraph material graphs; do not edit ----\n";
    s += "AverSurface averEvalMaterial(AverVertex v, AverLight l) {\n";
    s += "    float2 uv = averSurfaceUV(v);\n";
    s += "    AverAuthored a = averStockAuthored(uv, v.N);\n";
    // A UNIFORM SWITCH, not a chain of ifs: gMaterialGraphId is a constant across the whole draw,
    // so every lane takes the same arm and the cost is the arm's own, not the sum of all of them.
    s += "    switch (gMaterialGraphId) {\n";
    for (const MaterialGraphEntry& e : entries) {
        if (e.id == 0) continue;   // 0 is "no graph"; an entry claiming it would shadow the default
        s += "    // ";
        s += e.name.empty() ? std::string("<unnamed>") : e.name;
        s += "\n    case ";
        s += std::to_string(e.id);
        s += ": {\n";
        s += e.hlsl;
        s += "        break;\n    }\n";
    }
    s += "    default: break;   // no graph: exactly the stock material\n";
    s += "    }\n";
    s += "    return averBuildSurface(v, l, a, uv);\n";
    s += "}\n";
    return s;
}

} // namespace aver::pbr
