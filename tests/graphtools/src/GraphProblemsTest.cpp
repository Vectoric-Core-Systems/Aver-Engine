// GraphProblemsTest -- the Problems panel's model (sandbox/src/GraphProblems.hpp): parsing what the managed
// diagnostics return, ordering it, the per-node badge map, the toolbar summary and the debounce decision.
// Pure functions, no ImGui, no .NET runtime.
#include "GraphProblems.hpp"

#include "aver/core/Log.hpp"

#include <string>

using namespace aver;
using namespace aver::editor;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("   PASS  {}", what); }
    else { AVER_ERROR("   FAIL  {}", what); ++g_failures; }
}

static void testWireParsing() {
    AVER_INFO("=== the wire form parses, and junk does not hurt ===");
    const std::string wire =
        "E\tbad\tparam\tattr.missing\tTriggerAnim node 'bad' has no param= attribute\n"
        "W\t\t\tvar.unused\tVariable 'x' is declared and never used\n"
        "W\tlonely\t\texec.unreachable\tNode 'lonely' is not reached\r\n"
        "this line has no tabs\n"
        "X\tn\tp\tcode\tunknown severity letter\n"
        "E\tn\tp\n"                                  // too few fields
        "E\tkeep\t\tcode\tmessage\twith\ttabs inside\n"
        "...7 more\n";
    const ProblemList list = parseProblemWire(wire);
    check(list.items.size() == 4, "four well-formed lines survive, got " + std::to_string(list.items.size()));
    check(list.more == 7, "the cut line reports 7 more, got " + std::to_string(list.more));
    if (list.items.size() == 4) {
        const GraphProblem& a = list.items[0];
        check(a.severity == ProblemSeverity::Error && a.nodeId == "bad" && a.pin == "param" && a.code == "attr.missing",
              "the first line's fields land in the right members");
        check(a.message.find("param= attribute") != std::string::npos, "and its message is whole");
        const GraphProblem& b = list.items[1];
        check(b.severity == ProblemSeverity::Warning && b.nodeId.empty() && b.pin.empty() && b.code == "var.unused",
              "empty node and pin stay empty");
        check(list.items[2].message == "Node 'lonely' is not reached", "a CRLF line ending is not part of the message");
        check(list.items[3].message == "message\twith\ttabs inside", "tabs after the fourth field belong to the message");
    }
    check(parseProblemWire("").items.empty(), "empty text is no problems");
}

static void testSortOrder() {
    AVER_INFO("=== errors before warnings, graph-level first, then by node, stable ===");
    std::vector<GraphProblem> v;
    const auto add = [&](ProblemSeverity s, const char* node, const char* msg) {
        GraphProblem p;
        p.severity = s; p.nodeId = node; p.message = msg;
        v.push_back(p);
    };
    add(ProblemSeverity::Warning, "b", "w-b");
    add(ProblemSeverity::Error, "z", "e-z-first");
    add(ProblemSeverity::Error, "a", "e-a");
    add(ProblemSeverity::Warning, "", "w-graph");
    add(ProblemSeverity::Error, "z", "e-z-second");
    add(ProblemSeverity::Error, "", "e-graph");
    sortProblems(v);
    const char* expected[] = {"e-graph", "e-a", "e-z-first", "e-z-second", "w-graph", "w-b"};
    bool same = v.size() == 6;
    for (usize i = 0; same && i < 6; ++i) same = v[i].message == expected[i];
    std::string got;
    for (const auto& p : v) got += p.message + " ";
    check(same, "order is " + got);
}

static void testBadgesAndSummary() {
    AVER_INFO("=== badge map and the toolbar summary ===");
    std::vector<GraphProblem> v(4);
    v[0] = {ProblemSeverity::Warning, "n1", "", "w", "a warning first"};
    v[1] = {ProblemSeverity::Error, "n1", "", "e", "then an error"};
    v[2] = {ProblemSeverity::Error, "n1", "", "e", "a second error"};
    v[3] = {ProblemSeverity::Error, "", "", "e", "about the graph"};
    const auto badges = buildBadgeMap(v);
    check(badges.size() == 1, "only a problem that names a node earns a badge");
    const auto it = badges.find("n1");
    check(it != badges.end() && it->second.errors == 2 && it->second.warnings == 1,
          "n1 counts two errors and a warning");
    check(it != badges.end() && it->second.firstMessage == "then an error",
          "the tooltip leads with the first ERROR even when a warning came first");

    check(problemSummary(countProblems({})) == "no problems", "nothing wrong reads 'no problems'");
    check(problemSummary({1, 0}) == "1 error", "singular error");
    check(problemSummary({2, 1}) == "2 errors, 1 warning", "'2 errors, 1 warning' exactly as the toolbar wants it");
    check(problemSummary({0, 3}) == "3 warnings", "warnings alone");
    const ProblemCounts c = countProblems(v);
    check(c.errors == 3 && c.warnings == 1, "countProblems agrees with the list");
}

static void testDebounce() {
    AVER_INFO("=== the debounce is a pure decision ===");
    check(diagnosticsDue(0, kDiagnosticsNeverRan, 0.0, 0.0), "a graph never diagnosed is diagnosed at once (on open)");
    check(!diagnosticsDue(5, 5, 1.0, 100.0), "nothing changed since the last run: not due, however long ago");
    check(!diagnosticsDue(6, 5, 10.0, 10.2), "edited 0.2 s ago: still typing, not due");
    check(!diagnosticsDue(6, 5, 10.0, 10.49), "0.49 s after the last edit: not due");
    check(diagnosticsDue(6, 5, 10.0, 10.5), "0.5 s after the last edit: due");
    check(diagnosticsDue(9, 5, 10.0, 12.0), "several edits, quiet for 2 s: due");
    check(diagnosticsDue(6, 5, 10.0, 10.2, 0.1), "a shorter interval is honoured");
}

int main() {
    AVER_INFO("======== GraphProblemsTest ========");
    testWireParsing();
    testSortOrder();
    testBadgesAndSummary();
    testDebounce();
    if (g_failures == 0) AVER_INFO("======== GraphProblemsTest: all passed ========");
    else AVER_ERROR("======== GraphProblemsTest: {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
