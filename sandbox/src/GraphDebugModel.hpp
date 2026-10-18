#pragma once
// The graph debugger's editor-side model, minus ImGui and minus the bridge: breakpoint keys and the set
// of them, the values the managed hooks report for wires, and the one process-wide session the editor
// tabs and SandboxApp share. ImGui-free and header-only so tests/graphtools covers it.
//
// What the managed half does (scripting/csharp/Aver.Graph/GraphDebug.cs): a breakpoint latches when its
// node runs, and the host pauses the Play session at the END of that frame -- it cannot stop mid-tick.
// Values arrive as `node.pin=value;` and are the last value each data input was pulled with.
#include "aver/core/Types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aver::editor {

// ---- breakpoint keys ---------------------------------------------------------------------------------------

// A graph NAME and a node id around one unit separator -- the form the bridge takes and returns. Names
// and ids are whitespace-free tokens in the format, so the separator cannot occur inside either.
inline constexpr char kBreakKeySep = '\x1f';

inline std::string encodeBreakKey(std::string_view graph, std::string_view node) {
    std::string k(graph);
    k += kBreakKeySep;
    k += node;
    return k;
}

// False for a key without a separator or with either half empty.
inline bool decodeBreakKey(std::string_view key, std::string& graph, std::string& node) {
    const usize at = key.find(kBreakKeySep);
    if (at == std::string_view::npos || at == 0 || at + 1 >= key.size()) return false;
    graph = std::string(key.substr(0, at));
    node = std::string(key.substr(at + 1));
    return true;
}

// ---- the breakpoint set ------------------------------------------------------------------------------------

// Every breakpoint the author has set, across graphs. A breakpoint can be disabled without being
// forgotten ("Disable breakpoint" in the node menu): a node on an OnTick chain breaks again every frame
// after Resume, so the author needs a way to let it run without losing where they were looking.
class DebugBreakpoints {
public:
    // Adds an enabled breakpoint, or removes the one that is there. True when it exists afterwards.
    bool toggle(const std::string& graph, const std::string& node) {
        auto& m = byGraph_[graph];
        const auto it = m.find(node);
        if (it != m.end()) {
            m.erase(it);
            if (m.empty()) byGraph_.erase(graph);
            ++revision_;
            return false;
        }
        m[node] = true;
        ++revision_;
        return true;
    }
    bool has(const std::string& graph, const std::string& node) const {
        const auto g = byGraph_.find(graph);
        return g != byGraph_.end() && g->second.count(node) != 0;
    }
    bool enabled(const std::string& graph, const std::string& node) const {
        const auto g = byGraph_.find(graph);
        if (g == byGraph_.end()) return false;
        const auto n = g->second.find(node);
        return n != g->second.end() && n->second;
    }
    // False when there is no such breakpoint.
    bool setEnabled(const std::string& graph, const std::string& node, bool on) {
        const auto g = byGraph_.find(graph);
        if (g == byGraph_.end()) return false;
        const auto n = g->second.find(node);
        if (n == g->second.end()) return false;
        if (n->second != on) { n->second = on; ++revision_; }
        return true;
    }
    void clearGraph(const std::string& graph) {
        if (byGraph_.erase(graph) != 0) ++revision_;
    }
    // Forgets a node's breakpoint, for when the node is deleted or renamed away.
    void remove(const std::string& graph, const std::string& node) {
        const auto g = byGraph_.find(graph);
        if (g == byGraph_.end() || g->second.erase(node) == 0) return;
        if (g->second.empty()) byGraph_.erase(g);
        ++revision_;
    }
    usize count() const {
        usize n = 0;
        for (const auto& g : byGraph_) n += g.second.size();
        return n;
    }
    // Ticks on every change, so the host knows when the managed set is out of date.
    u64 revision() const { return revision_; }

    // The managed side's input: one `graph<US>node` line per ENABLED breakpoint, in a stable order.
    std::string wire() const {
        std::string out;
        for (const auto& g : byGraph_)
            for (const auto& n : g.second)
                if (n.second) { out += encodeBreakKey(g.first, n.first); out += '\n'; }
        return out;
    }

    // One graph's set as a single preference value: `1:nodeA 0:nodeB` (state, colon, id; a space between).
    // Node ids are whitespace-free in this format, which is what makes a space safe here.
    std::string serializeGraph(const std::string& graph) const {
        const auto g = byGraph_.find(graph);
        if (g == byGraph_.end()) return {};
        std::string out;
        for (const auto& n : g->second) {
            if (!out.empty()) out += ' ';
            out += n.second ? "1:" : "0:";
            out += n.first;
        }
        return out;
    }
    // Replaces one graph's set from serializeGraph's text; anything malformed is skipped.
    void loadGraph(const std::string& graph, std::string_view text) {
        std::map<std::string, bool> next;
        usize pos = 0;
        while (pos < text.size()) {
            usize sp = text.find(' ', pos);
            if (sp == std::string_view::npos) sp = text.size();
            const std::string_view tok = text.substr(pos, sp - pos);
            pos = sp + 1;
            if (tok.size() > 2 && tok[1] == ':' && (tok[0] == '0' || tok[0] == '1'))
                next[std::string(tok.substr(2))] = tok[0] == '1';
        }
        if (next.empty()) byGraph_.erase(graph);
        else byGraph_[graph] = std::move(next);
        ++revision_;
    }

private:
    std::map<std::string, std::map<std::string, bool>> byGraph_;   // graph -> node -> enabled
    u64 revision_ = 0;
};

// ---- values on wires ---------------------------------------------------------------------------------------

// A float/int/bool the hooks widened to double, shown as a short label: `0.5`, `12`, `true`. The pin's
// declared type (not the number) decides the form, since 1.0 is both a float and a bool.
inline std::string formatDebugValue(double v, std::string_view pinType) {
    if (pinType == "bool") return v != 0.0 ? "true" : "false";
    if (std::isnan(v)) return "NaN";
    if (std::isinf(v)) return v < 0 ? "-inf" : "inf";
    char buf[48];
    if (pinType == "int") {
        std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(std::llround(v)));
    } else {
        std::snprintf(buf, sizeof buf, "%.4g", v == 0.0 ? 0.0 : v);   // folds -0 into 0
    }
    return buf;
}

struct DebugValue {
    double value = 0.0;
    double seenAt = 0.0;      // the last update that listed it
    double changedAt = 0.0;   // the last update where it differed from the one before
};

// The last value on each data input of ONE graph, as the bridge reports them, with ages so the canvas
// can dim a label that has stopped changing and drop one the managed side no longer lists.
class DebugValues {
public:
    // Reads `node.pin=value;` repeated. The pin is what follows the LAST dot (pin names have none, node
    // ids might); the value is what follows the last '='. A malformed entry is skipped.
    void update(std::string_view text, double nowSec) {
        usize pos = 0;
        while (pos < text.size()) {
            usize semi = text.find(';', pos);
            if (semi == std::string_view::npos) semi = text.size();
            const std::string_view entry = text.substr(pos, semi - pos);
            pos = semi + 1;

            const usize eq = entry.rfind('=');
            if (eq == std::string_view::npos || eq == 0) continue;
            const std::string_view key = entry.substr(0, eq);
            const usize dot = key.rfind('.');
            if (dot == std::string_view::npos || dot == 0 || dot + 1 >= key.size()) continue;
            const std::string valueText(entry.substr(eq + 1));
            char* end = nullptr;
            const double v = std::strtod(valueText.c_str(), &end);
            if (end == valueText.c_str()) continue;

            auto& pins = values_[std::string(key.substr(0, dot))];
            const std::string pin(key.substr(dot + 1));
            const auto it = pins.find(pin);
            if (it == pins.end()) {
                pins[pin] = DebugValue{v, nowSec, nowSec};
                continue;
            }
            DebugValue& slot = it->second;
            const bool same = slot.value == v || (std::isnan(slot.value) && std::isnan(v));
            if (!same) slot.changedAt = nowSec;
            slot.value = v;
            slot.seenAt = nowSec;
        }
    }
    const DebugValue* find(const std::string& node, const std::string& pin) const {
        const auto n = values_.find(node);
        if (n == values_.end()) return nullptr;
        const auto p = n->second.find(pin);
        return p == n->second.end() ? nullptr : &p->second;
    }
    // Drops every entry the last `maxAgeSec` of updates did not list. Returns how many went.
    usize expire(double nowSec, double maxAgeSec) {
        usize dropped = 0;
        for (auto n = values_.begin(); n != values_.end();) {
            for (auto p = n->second.begin(); p != n->second.end();) {
                if (nowSec - p->second.seenAt > maxAgeSec) { p = n->second.erase(p); ++dropped; }
                else ++p;
            }
            n = n->second.empty() ? values_.erase(n) : std::next(n);
        }
        return dropped;
    }
    // A value that has not changed for `afterSec` reads as stale -- drawn dimmer, not hidden.
    static bool stale(const DebugValue& v, double nowSec, double afterSec = 2.0) { return nowSec - v.changedAt > afterSec; }
    void clear() { values_.clear(); }
    usize size() const {
        usize n = 0;
        for (const auto& g : values_) n += g.second.size();
        return n;
    }

private:
    std::unordered_map<std::string, std::unordered_map<std::string, DebugValue>> values_;   // node -> pin -> value
};

// ---- the session -------------------------------------------------------------------------------------------

struct BreakEvent {
    std::string graph;
    std::string node;
};

// Everything the editor tabs and SandboxApp share about the debugger. One per process: breakpoints are
// the author's, not any tab's, and the host polls a single latched break.
struct DebugSession {
    bool armed = false;                     // graphs compiled from now on carry the hooks
    DebugBreakpoints breakpoints;
    std::optional<BreakEvent> lastBreak;    // the most recent break, until Play resumes past it
    bool armedPushed = false;               // whether `armed` has reached the bridge
    u64 pushedRevision = ~static_cast<u64>(0);   // the breakpoint revision the bridge last received

    bool needsArmPush() const { return !armedPushed; }
    bool needsBreakpointPush() const { return pushedRevision != breakpoints.revision(); }
    void markArmPushed() { armedPushed = true; }
    void markBreakpointsPushed() { pushedRevision = breakpoints.revision(); }
    void setArmed(bool on) {
        if (armed == on) return;
        armed = on;
        armedPushed = false;
        if (!on) lastBreak.reset();
    }
};

inline DebugSession& debugSession() {
    static DebugSession s;
    return s;
}

// The bridge's TakeBreak text, as an event; false for empty or malformed.
inline bool parseBreakEvent(std::string_view wire, BreakEvent& out) {
    return decodeBreakKey(wire, out.graph, out.node);
}

} // namespace aver::editor
