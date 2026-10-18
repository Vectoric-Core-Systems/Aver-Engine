// GraphDebugModelTest -- the graph debugger's editor-side model (sandbox/src/GraphDebugModel.hpp):
// breakpoint keys, the breakpoint set and how it persists, value formatting, the values the managed hooks
// report (and when a label goes stale or is dropped), and the session flags SandboxApp pushes from.
// Pure functions, no ImGui, no .NET runtime.
#include "GraphDebugModel.hpp"

#include "aver/core/Log.hpp"

#include <cmath>
#include <limits>
#include <string>

using namespace aver;
using namespace aver::editor;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("   PASS  {}", what); }
    else { AVER_ERROR("   FAIL  {}", what); ++g_failures; }
}

static void testKeys() {
    AVER_INFO("=== a breakpoint key is graph, separator, node ===");
    const std::string k = encodeBreakKey("Player", "sv1");
    check(k == std::string("Player") + '\x1f' + "sv1", "encoded with the unit separator");
    std::string g, n;
    check(decodeBreakKey(k, g, n) && g == "Player" && n == "sv1", "and decodes back");
    check(!decodeBreakKey("nosep", g, n), "no separator is not a key");
    check(!decodeBreakKey(std::string("\x1f") + "node", g, n), "an empty graph is not a key");
    check(!decodeBreakKey(std::string("graph") + '\x1f', g, n), "an empty node is not a key");
    BreakEvent ev;
    check(parseBreakEvent(k, ev) && ev.graph == "Player" && ev.node == "sv1", "a TakeBreak payload parses as an event");
    check(!parseBreakEvent("", ev), "an empty payload is no event");
}

static void testBreakpointSet() {
    AVER_INFO("=== toggling, disabling and the wire form ===");
    DebugBreakpoints bp;
    const u64 r0 = bp.revision();
    check(bp.toggle("G", "a"), "toggle adds (returns true: it exists afterwards)");
    check(bp.has("G", "a") && bp.enabled("G", "a"), "and it is enabled");
    check(bp.revision() != r0, "the revision moved");
    bp.toggle("G", "b");
    bp.toggle("H", "c");
    check(bp.count() == 3, "three breakpoints across two graphs");

    check(bp.setEnabled("G", "b", false), "Disable Breakpoint finds it");
    check(bp.has("G", "b") && !bp.enabled("G", "b"), "disabled is not forgotten");
    check(!bp.setEnabled("G", "missing", true), "a breakpoint that is not there cannot be enabled");

    const std::string wire = bp.wire();
    check(wire == encodeBreakKey("G", "a") + "\n" + encodeBreakKey("H", "c") + "\n",
          "the wire form carries ENABLED breakpoints only, one per line, in a stable order");

    const u64 r1 = bp.revision();
    check(!bp.toggle("G", "a"), "toggling again removes it (returns false)");
    check(bp.revision() != r1 && !bp.has("G", "a"), "gone, and the revision moved");
    bp.setEnabled("G", "b", false);   // already disabled: no change
    const u64 r2 = bp.revision();
    bp.setEnabled("G", "b", false);
    check(bp.revision() == r2, "setting a state it already has is not a change");

    bp.remove("H", "c");
    check(!bp.has("H", "c") && bp.count() == 1, "remove drops a deleted node's breakpoint");
    bp.clearGraph("G");
    check(bp.count() == 0 && bp.wire().empty(), "clearGraph empties a graph");
}

static void testPersistence() {
    AVER_INFO("=== a graph's breakpoints survive as one preference value ===");
    DebugBreakpoints a;
    a.toggle("G", "sv1");
    a.toggle("G", "gate_2");
    a.toggle("G", "off");
    a.setEnabled("G", "off", false);
    a.toggle("Other", "x");
    const std::string saved = a.serializeGraph("G");
    check(saved.find('\n') == std::string::npos, "no newline (the prefs store refuses one)");
    check(a.serializeGraph("Nothing").empty(), "a graph with none serialises to nothing");

    DebugBreakpoints b;
    b.loadGraph("G", saved);
    check(b.has("G", "sv1") && b.enabled("G", "sv1"), "an enabled breakpoint comes back enabled");
    check(b.has("G", "gate_2") && b.enabled("G", "gate_2"), "so does another");
    check(b.has("G", "off") && !b.enabled("G", "off"), "a disabled one comes back disabled");
    check(!b.has("Other", "x"), "and only that graph's");

    b.loadGraph("G", "1:ok 9:bad nocolon 0: 1:fine");
    check(b.has("G", "ok") && b.has("G", "fine") && !b.has("G", "bad") && !b.has("G", "sv1"),
          "junk entries are skipped and the load REPLACES the graph's set");
    b.loadGraph("G", "");
    check(!b.has("G", "ok"), "an empty value clears it");
}

static void testFormatting() {
    AVER_INFO("=== values are labelled by the pin's type, not by the number ===");
    check(formatDebugValue(0.5, "float") == "0.5", "0.5");
    check(formatDebugValue(1234.5678, "float") == "1235", "four significant digits");
    check(formatDebugValue(0.00012345, "float") == "0.0001234" || formatDebugValue(0.00012345, "float") == "0.0001235",
          "small floats keep their digits");
    check(formatDebugValue(-0.0, "float") == "0", "negative zero reads 0");
    check(formatDebugValue(12.0, "int") == "12", "an int");
    check(formatDebugValue(-7.0, "int") == "-7", "a negative int");
    check(formatDebugValue(1.0, "bool") == "true" && formatDebugValue(0.0, "bool") == "false", "bool is true/false");
    check(formatDebugValue(1.0, "float") == "1", "1.0 as a float is 1, not true");
    check(formatDebugValue(std::numeric_limits<double>::quiet_NaN(), "float") == "NaN", "NaN");
    check(formatDebugValue(std::numeric_limits<double>::infinity(), "float") == "inf", "infinity");
    check(formatDebugValue(-std::numeric_limits<double>::infinity(), "float") == "-inf", "negative infinity");
}

static void testValues() {
    AVER_INFO("=== values from the hooks: parsing, change times, staleness, expiry ===");
    DebugValues v;
    v.update("n1.a=1.5;n1.b=2;n2.x=NaN;bad;=3;n3.=4;.p=5;n4.q=;", 10.0);
    check(v.size() == 3, "three well-formed entries, junk skipped, got " + std::to_string(v.size()));
    const DebugValue* a = v.find("n1", "a");
    check(a && a->value == 1.5 && a->seenAt == 10.0 && a->changedAt == 10.0, "n1.a is 1.5, seen and changed at 10");
    check(v.find("n2", "x") && std::isnan(v.find("n2", "x")->value), "NaN parses as NaN");
    check(!v.find("n1", "zzz") && !v.find("nope", "a"), "unknown pins and nodes are absent");

    // A node id may itself contain a dot; the pin is what follows the LAST one.
    v.update("a.b.c=3;", 10.5);
    check(v.find("a.b", "c") && v.find("a.b", "c")->value == 3.0, "a dotted node id splits at the last dot");

    v.update("n1.a=1.5;n1.b=9;n2.x=NaN;", 12.0);
    check(v.find("n1", "a")->changedAt == 10.0 && v.find("n1", "a")->seenAt == 12.0, "an unchanged value keeps its change time");
    check(v.find("n1", "b")->changedAt == 12.0 && v.find("n1", "b")->value == 9.0, "a changed value takes the new one and time");
    check(v.find("n2", "x")->changedAt == 10.0, "NaN to NaN is not a change");

    const DebugValue& b = *v.find("n1", "b");
    check(!DebugValues::stale(b, 13.0), "a value changed 1 s ago is fresh");
    check(DebugValues::stale(b, 14.5), "and 2.5 s ago is stale (dimmed, not hidden)");

    // n1.a, n1.b, n2.x were listed at 12; a.b.c last at 10.5.
    check(v.expire(13.0, 5.0) == 0, "nothing is old enough yet");
    check(v.expire(16.0, 5.0) == 1 && !v.find("a.b", "c"), "the entry unlisted since 10.5 expires, the rest stay");
    check(v.size() == 3, "three remain");
    v.clear();
    check(v.size() == 0, "clear empties it");
}

static void testSession() {
    AVER_INFO("=== the session knows what has reached the bridge ===");
    DebugSession s;
    check(s.needsArmPush() && s.needsBreakpointPush(), "a new session has pushed nothing");
    s.markArmPushed();
    s.markBreakpointsPushed();
    check(!s.needsArmPush() && !s.needsBreakpointPush(), "pushed");
    s.breakpoints.toggle("G", "n");
    check(s.needsBreakpointPush(), "a new breakpoint needs a push");
    s.markBreakpointsPushed();
    s.setArmed(true);
    check(s.armed && s.needsArmPush(), "arming needs a push");
    s.markArmPushed();
    s.setArmed(true);
    check(!s.needsArmPush(), "arming when armed is nothing");
    s.lastBreak = BreakEvent{"G", "n"};
    s.setArmed(false);
    check(!s.armed && s.needsArmPush() && !s.lastBreak.has_value(), "disarming needs a push and forgets the last break");
}

int main() {
    AVER_INFO("======== GraphDebugModelTest ========");
    testKeys();
    testBreakpointSet();
    testPersistence();
    testFormatting();
    testValues();
    testSession();
    if (g_failures == 0) AVER_INFO("======== GraphDebugModelTest: all passed ========");
    else AVER_ERROR("======== GraphDebugModelTest: {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
