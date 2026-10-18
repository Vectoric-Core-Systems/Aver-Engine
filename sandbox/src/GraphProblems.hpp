#pragma once
// The graph editor's Problems panel, minus the panel: parsing what the managed diagnostics return,
// ordering it, the per-node badge map, the toolbar summary and the decision of WHEN to ask again.
// ImGui-free and header-only so tests/graphtools covers it with no window and no .NET runtime.
//
// The wire form is the one HostBridge.GraphDiagnose writes (scripting/csharp/Aver.Graph/
// GraphDiagnostics.cs): one line per problem, `E` or `W`, then node id, pin, code and message
// separated by tabs; any of the middle three may be empty. When the managed side ran out of buffer the
// last line is `...N more`.
#include "aver/core/Types.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aver::editor {

enum class ProblemSeverity : u8 { Error, Warning };

struct GraphProblem {
    ProblemSeverity severity = ProblemSeverity::Error;
    std::string nodeId;    // empty for a problem about the graph as a whole
    std::string pin;
    std::string code;      // stable, e.g. "attr.missing", "exec.unreachable", "compile"
    std::string message;
};

struct ProblemList {
    std::vector<GraphProblem> items;
    usize more = 0;        // problems the managed side cut off ("...N more")
};

// Parses the wire text. A malformed line is skipped, not fatal: the list is advice, and a half-read one
// beats none.
inline ProblemList parseProblemWire(std::string_view wire) {
    ProblemList out;
    usize pos = 0;
    while (pos < wire.size()) {
        usize nl = wire.find('\n', pos);
        if (nl == std::string_view::npos) nl = wire.size();
        std::string_view line = wire.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.back() == '\r')) line.remove_suffix(1);
        if (line.empty()) continue;

        if (line.substr(0, 3) == "...") {
            usize n = 0;
            for (usize i = 3; i < line.size() && line[i] >= '0' && line[i] <= '9'; ++i)
                n = n * 10 + static_cast<usize>(line[i] - '0');
            out.more += n;
            continue;
        }

        // Up to four tabs; whatever follows the fourth is the message, tabs and all.
        std::string_view fields[5];
        usize found = 0;
        usize start = 0;
        while (found < 4) {
            const usize tab = line.find('\t', start);
            if (tab == std::string_view::npos) break;
            fields[found++] = line.substr(start, tab - start);
            start = tab + 1;
        }
        if (found < 4) continue;
        fields[4] = line.substr(start);
        if (fields[0] != "E" && fields[0] != "W") continue;

        GraphProblem p;
        p.severity = fields[0] == "E" ? ProblemSeverity::Error : ProblemSeverity::Warning;
        p.nodeId = std::string(fields[1]);
        p.pin = std::string(fields[2]);
        p.code = std::string(fields[3]);
        p.message = std::string(fields[4]);
        out.items.push_back(std::move(p));
    }
    return out;
}

// Errors before warnings; inside each, problems about the graph itself before problems about a node,
// then by node id. Stable, so two problems on one node keep the order the validator found them in.
inline void sortProblems(std::vector<GraphProblem>& items) {
    std::stable_sort(items.begin(), items.end(), [](const GraphProblem& a, const GraphProblem& b) {
        if (a.severity != b.severity) return a.severity < b.severity;
        if (a.nodeId.empty() != b.nodeId.empty()) return a.nodeId.empty();
        return a.nodeId < b.nodeId;
    });
}

struct ProblemCounts {
    usize errors = 0;
    usize warnings = 0;
};

inline ProblemCounts countProblems(const std::vector<GraphProblem>& items) {
    ProblemCounts c;
    for (const GraphProblem& p : items) (p.severity == ProblemSeverity::Error ? c.errors : c.warnings)++;
    return c;
}

// "2 errors, 1 warning", "1 error", "no problems" -- the text beside the Validate button.
inline std::string problemSummary(const ProblemCounts& c) {
    const auto part = [](usize n, const char* one, const char* many) {
        return std::to_string(n) + " " + (n == 1 ? one : many);
    };
    if (c.errors == 0 && c.warnings == 0) return "no problems";
    std::string s;
    if (c.errors) s = part(c.errors, "error", "errors");
    if (c.warnings) s += (s.empty() ? "" : ", ") + part(c.warnings, "warning", "warnings");
    return s;
}

// What a node's corner badge shows: how many of each, and the first message for the tooltip.
struct NodeBadge {
    usize errors = 0;
    usize warnings = 0;
    std::string firstMessage;
};

// nodeId -> badge, for every problem that names a node. Errors outrank warnings for the tooltip text.
inline std::unordered_map<std::string, NodeBadge> buildBadgeMap(const std::vector<GraphProblem>& items) {
    std::unordered_map<std::string, NodeBadge> out;
    for (const GraphProblem& p : items) {
        if (p.nodeId.empty()) continue;
        NodeBadge& b = out[p.nodeId];
        const bool firstOfKind = (p.severity == ProblemSeverity::Error ? b.errors : b.warnings) == 0;
        (p.severity == ProblemSeverity::Error ? b.errors : b.warnings)++;
        if (b.firstMessage.empty() || (p.severity == ProblemSeverity::Error && firstOfKind)) b.firstMessage = p.message;
    }
    return out;
}

// ---- when to ask again -----------------------------------------------------------------------------------

inline constexpr double kDiagnosticsDebounceSec = 0.5;
inline constexpr u64 kDiagnosticsNeverRan = ~static_cast<u64>(0);

// The whole debounce rule, as a pure function. `editSerial` counts edits (it moves with every undo
// step); `lastRunSerial` is the value it had when diagnostics last ran, or kDiagnosticsNeverRan. A graph
// that has not been diagnosed at all runs at once (opening it); after that, only when something changed
// and the author has paused typing for the debounce interval.
inline bool diagnosticsDue(u64 editSerial, u64 lastRunSerial, double lastEditAtSec, double nowSec,
                           double debounceSec = kDiagnosticsDebounceSec) {
    if (lastRunSerial == kDiagnosticsNeverRan) return true;
    if (editSerial == lastRunSerial) return false;
    return nowSec - lastEditAtSec >= debounceSec;
}

} // namespace aver::editor
