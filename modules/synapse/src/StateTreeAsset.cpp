#include "aver/synapse/StateTreeAsset.hpp"

#include "aver/synapse/TextRecords.hpp"
#include "aver/platform/FileSystem.hpp"

#include <algorithm>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>

namespace aver::synapse {
namespace {

std::string lowerStr(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool isLeafName(std::string_view s) {
    if (s.empty()) return false;
    for (usize i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        const bool rest = alpha || (c >= '0' && c <= '9') || c == '_';
        if (i == 0 ? !alpha : !rest) return false;
    }
    return true;
}

bool isKeyName(std::string_view s) {
    if (s.empty()) return false;
    for (usize i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        if (!(alpha || (c >= '0' && c <= '9' && i > 0))) return false;
    }
    return true;
}

std::vector<std::string> splitPath(std::string_view p) {
    std::vector<std::string> r;
    usize i = 0;
    while (i <= p.size()) {
        usize j = p.find('.', i);
        if (j == std::string_view::npos) j = p.size();
        r.emplace_back(p.substr(i, j - i));
        i = j + 1;
    }
    return r;
}

std::string trimmed(std::string_view s) {
    usize b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return std::string(s.substr(b, e - b));
}

// ---- parser -------------------------------------------------------------------------------------------

struct Parser {
    std::string name, err;
    StateTreeAsset a;

    bool fail(u32 line, const std::string& msg) {
        err = name + ":" + std::to_string(line) + ": " + msg;
        return false;
    }

    // `r.args[leafArg]` is the leaf name; the kv pairs are p0..p3 and s.
    bool leaf(const TextRecord& r, usize leafArg, fmt::OcBtNodeKind kind, fmt::OcBtNode& n) {
        if (r.args.size() != leafArg + 1) return fail(r.line, r.keyword + " takes a leaf name and p0..p3 / s=");
        if (!isLeafName(r.args[leafArg])) return fail(r.line, "bad leaf name '" + r.args[leafArg] + "'");
        n = fmt::OcBtNode{};
        n.kind = kind;
        n.name = r.args[leafArg];
        for (const auto& p : r.kv) {
            if (p.first == "s") {
                n.stringParam = p.second;
            } else if (p.first.size() >= 2 && p.first[0] == 'p' && p.first.find_first_not_of("0123456789", 1) == std::string::npos) {
                if (p.first.size() != 2 || p.first[1] > '3') return fail(r.line, "p index outside 0..3");
                f32 v = 0;
                if (!parseF32(p.second, v)) return fail(r.line, "'" + p.first + "' is not a finite number");
                n.params[p.first[1] - '0'] = v;
            } else {
                return fail(r.line, "unknown key '" + p.first + "'");
            }
        }
        return true;
    }

    bool cond(const TextRecord& r, StCond& c) {
        std::string rs = r.rest;
        usize pos = 0;
        const auto word = [&]() {
            while (pos < rs.size() && (rs[pos] == ' ' || rs[pos] == '\t')) ++pos;
            const usize b = pos;
            while (pos < rs.size() && rs[pos] != ' ' && rs[pos] != '\t') ++pos;
            return rs.substr(b, pos - b);
        };
        std::string w = word();
        c = StCond{};
        if (lowerStr(w) == "not") { c.negate = true; w = word(); }
        const std::string kind = lowerStr(w);
        if (kind == "bb") {
            const std::string expr = trimmed(rs.substr(pos));
            BbExpr e;
            std::string why;
            if (!bbParseExpr(expr, &a.schema, e, &why)) return fail(r.line, "bad expression '" + expr + "': " + why);
            c.isBlackboard = true;
            c.bb.key = e.key;
            c.bb.op = e.op;
            c.bb.value = e.value;
            return true;
        }
        if (kind == "cond") {
            usize at = 0;
            while (at < r.args.size() && lowerStr(r.args[at]) != "cond") ++at;
            c.isBlackboard = false;
            return leaf(r, at + 1, fmt::OcBtNodeKind::Condition, c.leaf);
        }
        return fail(r.line, "IF needs 'bb <expression>' or 'cond <Leaf>'");
    }
};

} // namespace

bool stStateNameValid(std::string_view s) {
    return isKeyName(s) && s != "NEXT" && s != "SUCCEEDED" && s != "FAILED";
}

std::string StateTreeAsset::statePath(i32 index) const {
    if (index < 0 || static_cast<usize>(index) >= states.size()) return {};
    std::string p;
    for (i32 i = index; i >= 0; i = states[static_cast<usize>(i)].parent) {
        p = p.empty() ? states[static_cast<usize>(i)].name : states[static_cast<usize>(i)].name + "." + p;
        if (p.size() > 4096) break;
    }
    return p;
}

i32 StateTreeAsset::findPath(std::string_view dotted) const {
    if (dotted.empty()) return -1;
    const std::vector<i32>* level = &roots;
    i32 found = -1;
    for (const std::string& seg : splitPath(dotted)) {
        found = -1;
        for (const i32 i : *level) {
            if (states[static_cast<usize>(i)].name == seg) { found = i; break; }
        }
        if (found < 0) return -1;
        level = &states[static_cast<usize>(found)].children;
    }
    return found;
}

StComplete stEffectiveComplete(const StState& s) {
    if (s.completeExplicit) return s.complete;
    return s.tasks.empty() ? StComplete::None : StComplete::All;
}

bool parseStateTree(std::string_view text, const std::string& name, StateTreeAsset& out, std::string* why) {
    out = StateTreeAsset{};
    std::vector<TextRecord> recs;
    std::string rwhy;
    if (!readTextRecords(text, "OCST", 1, recs, &rwhy)) {
        if (why) *why = name + ":" + rwhy.substr(5);
        return false;
    }
    Parser p;
    p.name = name;
    p.a.name = name;
    StateTreeAsset& a = p.a;
    const auto bad = [&]() {
        if (why) *why = p.err;
        out = StateTreeAsset{};
        return false;
    };

    std::vector<i32> stack;
    std::vector<u32> stackLines;
    std::vector<std::vector<u32>> transLines;
    bool seenState = false, sawDesc = false, sawFinish = false;
    enum class IfTo { None, Enter, Trans } ifTo = IfTo::None;
    i32 ifState = -1;
    usize ifTrans = 0;

    for (const TextRecord& r : recs) {
        const std::string& k = r.keyword;
        if (k == "STATE") {
            if (r.args.size() != 1 || !r.kv.empty()) { p.fail(r.line, "STATE needs a name"); return bad(); }
            const std::string& n = r.args[0];
            if (!stStateNameValid(n)) { p.fail(r.line, "bad state name '" + n + "'"); return bad(); }
            if (a.states.size() >= kStMaxStates) { p.fail(r.line, "more than 256 states"); return bad(); }
            if (stack.size() >= kStMaxDepth) { p.fail(r.line, "states nested deeper than 16"); return bad(); }
            const std::vector<i32>& siblings = stack.empty() ? a.roots : a.states[static_cast<usize>(stack.back())].children;
            for (const i32 s : siblings)
                if (a.states[static_cast<usize>(s)].name == n) { p.fail(r.line, "duplicate state '" + n + "'"); return bad(); }
            const i32 idx = static_cast<i32>(a.states.size());
            StState st;
            st.name = n;
            st.parent = stack.empty() ? -1 : stack.back();
            a.states.push_back(std::move(st));
            transLines.emplace_back();
            if (stack.empty()) a.roots.push_back(idx);
            else a.states[static_cast<usize>(stack.back())].children.push_back(idx);
            stack.push_back(idx);
            stackLines.push_back(r.line);
            seenState = true;
            ifTo = IfTo::None;
        } else if (k == "END") {
            if (stack.empty()) { p.fail(r.line, "END without STATE"); return bad(); }
            if (!r.args.empty() || !r.kv.empty()) { p.fail(r.line, "END takes no arguments"); return bad(); }
            stack.pop_back();
            stackLines.pop_back();
            ifTo = IfTo::None;
        } else if (stack.empty()) {
            if (seenState && (k == "DESC" || k == "ONFINISH" || k == "KEY" || k == "EVAL")) { p.fail(r.line, k + " must come before the first STATE"); return bad(); }
            if (k == "DESC") {
                if (sawDesc) { p.fail(r.line, "duplicate DESC"); return bad(); }
                sawDesc = true;
                for (usize i = 0; i < r.args.size(); ++i) a.desc += (i ? " " : "") + r.args[i];
            } else if (k == "ONFINISH") {
                if (sawFinish) { p.fail(r.line, "duplicate ONFINISH"); return bad(); }
                sawFinish = true;
                const std::string m = r.args.size() == 1 ? lowerStr(r.args[0]) : std::string();
                if (m == "restart") a.restartOnFinish = true;
                else if (m == "stop") a.restartOnFinish = false;
                else { p.fail(r.line, "ONFINISH must be restart or stop"); return bad(); }
            } else if (k == "KEY") {
                if (r.args.size() < 3 || r.args.size() > 4 || !r.kv.empty()) { p.fail(r.line, "KEY needs <name> <type> <scope> [<default>]"); return bad(); }
                BbKeyDef d;
                d.name = r.args[0];
                if (!isKeyName(d.name)) { p.fail(r.line, "bad key name '" + d.name + "'"); return bad(); }
                if (!bbTypeFromName(r.args[1], d.type)) { p.fail(r.line, "unknown key type '" + r.args[1] + "'"); return bad(); }
                const std::string sc = lowerStr(r.args[2]);
                if (sc == "agent") d.scope = BbScope::Agent;
                else if (sc == "shared") d.scope = BbScope::Shared;
                else { p.fail(r.line, "key scope must be agent or shared"); return bad(); }
                d.defaultValue = bbDefault(d.type);
                if (r.args.size() == 4 && !bbParse(d.type, r.args[3], d.defaultValue)) { p.fail(r.line, "bad default '" + r.args[3] + "' for a " + bbTypeName(d.type) + " key"); return bad(); }
                if (!a.schema.add(d)) { p.fail(r.line, "duplicate key '" + d.name + "'"); return bad(); }
            } else if (k == "EVAL") {
                fmt::OcBtNode n;
                if (!p.leaf(r, 0, fmt::OcBtNodeKind::Action, n)) return bad();
                if (a.evaluators.size() >= kStMaxTasks) { p.fail(r.line, "more than 16 evaluators"); return bad(); }
                a.evaluators.push_back(std::move(n));
            } else {
                p.fail(r.line, k == "TASK" || k == "ON" || k == "ENTER" || k == "IF" || k == "COMPLETE" || k == "GROUP" || k == "DISABLED" || k == "COMMENT"
                                   ? k + " outside a STATE"
                                   : "unknown record '" + k + "'");
                return bad();
            }
        } else {
            StState& st = a.states[static_cast<usize>(stack.back())];
            const i32 si = stack.back();
            if (k == "GROUP" || k == "DISABLED") {
                if (!r.args.empty() || !r.kv.empty()) { p.fail(r.line, k + " takes no arguments"); return bad(); }
                (k == "GROUP" ? st.group : st.disabled) = true;
            } else if (k == "COMPLETE") {
                const std::string m = r.args.size() == 1 && r.kv.empty() ? lowerStr(r.args[0]) : std::string();
                if (m == "all") st.complete = StComplete::All;
                else if (m == "any") st.complete = StComplete::Any;
                else if (m == "none") st.complete = StComplete::None;
                else { p.fail(r.line, "COMPLETE must be all, any or none"); return bad(); }
                st.completeExplicit = true;
            } else if (k == "COMMENT") {
                st.comment.clear();
                for (usize i = 0; i < r.args.size(); ++i) st.comment += (i ? " " : "") + r.args[i];
            } else if (k == "ENTER") {
                if (!r.args.empty() || !r.kv.empty()) { p.fail(r.line, "ENTER takes no arguments"); return bad(); }
                ifTo = IfTo::Enter;
                ifState = si;
            } else if (k == "IF") {
                if (ifTo == IfTo::None) { p.fail(r.line, "IF outside ENTER / ON"); return bad(); }
                StCond c;
                if (!p.cond(r, c)) return bad();
                StState& target = a.states[static_cast<usize>(ifState)];
                if (ifTo == IfTo::Enter) target.enter.push_back(std::move(c));
                else target.transitions[ifTrans].conds.push_back(std::move(c));
            } else if (k == "TASK") {
                fmt::OcBtNode n;
                if (!p.leaf(r, 0, fmt::OcBtNodeKind::Action, n)) return bad();
                if (st.tasks.size() >= kStMaxTasks) { p.fail(r.line, "more than 16 tasks in one state"); return bad(); }
                st.tasks.push_back(std::move(n));
            } else if (k == "ON") {
                if (r.args.size() != 3 || !r.kv.empty() || lowerStr(r.args[1]) != "to") { p.fail(r.line, "ON needs <trigger> TO <target>"); return bad(); }
                if (st.transitions.size() >= kStMaxTransitions) { p.fail(r.line, "more than 16 transitions in one state"); return bad(); }
                StTransition t;
                const std::string trig = r.args[0];
                const std::string tl = lowerStr(trig);
                if (tl == "tick") t.trigger = StTrigger::Tick;
                else if (tl == "completed") t.trigger = StTrigger::Completed;
                else if (tl == "failed") t.trigger = StTrigger::Failed;
                else if (tl.rfind("event:", 0) == 0 && trig.size() > 6) { t.trigger = StTrigger::Event; t.event = trig.substr(6); }
                else { p.fail(r.line, "unknown trigger '" + trig + "'"); return bad(); }
                const std::string& tgt = r.args[2];
                if (tgt == "NEXT") t.target = StTargetKind::Next;
                else if (tgt == "SUCCEEDED") t.target = StTargetKind::Succeeded;
                else if (tgt == "FAILED") t.target = StTargetKind::Failed;
                else {
                    for (const std::string& seg : splitPath(tgt))
                        if (!stStateNameValid(seg)) { p.fail(r.line, "bad transition target '" + tgt + "'"); return bad(); }
                    t.target = StTargetKind::Goto;
                    t.targetPath = tgt;
                }
                st.transitions.push_back(std::move(t));
                transLines[static_cast<usize>(si)].push_back(r.line);
                ifTo = IfTo::Trans;
                ifState = si;
                ifTrans = st.transitions.size() - 1;
            } else {
                p.fail(r.line, "unknown record '" + k + "'");
                return bad();
            }
        }
    }
    if (!stack.empty()) {
        p.fail(stackLines.back(), "STATE '" + a.states[static_cast<usize>(stack.back())].name + "' is not closed with END");
        return bad();
    }
    for (usize s = 0; s < a.states.size(); ++s) {
        for (usize t = 0; t < a.states[s].transitions.size(); ++t) {
            StTransition& tr = a.states[s].transitions[t];
            if (tr.target != StTargetKind::Goto) continue;
            tr.targetState = a.findPath(tr.targetPath);
            if (tr.targetState < 0) { p.fail(transLines[s][t], "transition target '" + tr.targetPath + "' does not name a state"); return bad(); }
        }
    }
    out = std::move(a);
    return true;
}

// ---- writer -------------------------------------------------------------------------------------------

namespace {

std::string leafText(const fmt::OcBtNode& n) {
    std::string s = n.name;
    for (int i = 0; i < 4; ++i)
        if (n.params[i] != 0) s += std::string(" p") + static_cast<char>('0' + i) + "=" + fmtF32(n.params[i]);
    if (!n.stringParam.empty()) s += " s=" + quoteToken(n.stringParam);
    return s;
}

std::string condText(const StCond& c) {
    std::string s = "IF ";
    if (c.negate) s += "not ";
    if (c.isBlackboard) {
        BbExpr e;
        e.key = c.bb.key;
        e.op = c.bb.op;
        e.value = c.bb.value;
        return s + "bb " + bbFormatExpr(e);
    }
    return s + "cond " + leafText(c.leaf);
}

std::string defaultText(const BbKeyDef& d) {
    switch (d.defaultValue.type) {
        case BbType::Float: return fmtF32(d.defaultValue.f);
        case BbType::Vec3: return fmtF32(d.defaultValue.v.x) + "," + fmtF32(d.defaultValue.v.y) + "," + fmtF32(d.defaultValue.v.z);
        default: return bbToString(d.defaultValue);
    }
}

void writeState(const StateTreeAsset& a, i32 idx, int depth, std::string& s) {
    const StState& st = a.states[static_cast<usize>(idx)];
    const std::string in(static_cast<usize>(depth) * 2, ' ');
    const std::string in2 = in + "  ";
    s += in + "STATE " + st.name + "\n";
    if (st.group) s += in2 + "GROUP\n";
    if (st.disabled) s += in2 + "DISABLED\n";
    if (st.completeExplicit) s += in2 + "COMPLETE " + (st.complete == StComplete::All ? "all" : st.complete == StComplete::Any ? "any" : "none") + "\n";
    if (!st.comment.empty()) s += in2 + "COMMENT " + quoteToken(st.comment) + "\n";
    if (!st.enter.empty()) {
        s += in2 + "ENTER\n";
        for (const StCond& c : st.enter) s += in2 + "  " + condText(c) + "\n";
    }
    for (const fmt::OcBtNode& t : st.tasks) s += in2 + "TASK " + leafText(t) + "\n";
    for (const StTransition& t : st.transitions) {
        std::string trig = t.trigger == StTrigger::Tick ? "tick" : t.trigger == StTrigger::Completed ? "completed" : t.trigger == StTrigger::Failed ? "failed" : "event:" + t.event;
        std::string target;
        switch (t.target) {
            case StTargetKind::Next: target = "NEXT"; break;
            case StTargetKind::Succeeded: target = "SUCCEEDED"; break;
            case StTargetKind::Failed: target = "FAILED"; break;
            case StTargetKind::Goto: target = t.targetState >= 0 ? a.statePath(t.targetState) : t.targetPath; break;
        }
        s += in2 + "ON " + trig + " TO " + target + "\n";
        for (const StCond& c : t.conds) s += in2 + "  " + condText(c) + "\n";
    }
    for (const i32 c : st.children) writeState(a, c, depth + 1, s);
    s += in + "END\n";
}

} // namespace

std::string writeStateTree(const StateTreeAsset& a) {
    std::string s = "OCST 1\n";
    if (!a.desc.empty()) s += "DESC " + quoteToken(a.desc) + "\n";
    if (!a.restartOnFinish) s += "ONFINISH stop\n";
    for (const BbKeyDef& d : a.schema.keys) {
        s += "KEY " + d.name + " " + lowerStr(bbTypeName(d.type)) + " " + lowerStr(bbScopeName(d.scope));
        const BbValue def = bbDefault(d.type);
        if (!(d.defaultValue == def)) s += " " + quoteToken(defaultText(d));
        s += "\n";
    }
    for (const fmt::OcBtNode& e : a.evaluators) s += "EVAL " + leafText(e) + "\n";
    for (const i32 r : a.roots) writeState(a, r, 0, s);
    return s;
}

bool loadStateTree(const std::string& path, StateTreeAsset& out, std::string* why) {
    out = StateTreeAsset{};
    std::ifstream f(aver::nativePath(path), std::ios::binary);
    if (!f) {
        if (why) *why = path + ": cannot open";
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (!parseStateTree(ss.str(), path, out, why)) return false;
    out.path = path;
    return true;
}

bool saveStateTree(const std::string& path, const StateTreeAsset& in, std::string* why) {
    std::ofstream f(aver::nativePath(path), std::ios::binary | std::ios::trunc);
    if (!f) {
        if (why) *why = path + ": cannot write";
        return false;
    }
    const std::string text = writeStateTree(in);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) {
        if (why) *why = path + ": write failed";
        return false;
    }
    return true;
}

// ---- validation ---------------------------------------------------------------------------------------

std::vector<std::string> validateStateTree(const StateTreeAsset& a, const BtRegistry* reg) {
    std::vector<std::string> w;
    std::set<std::string> reported;
    const auto note = [&](const std::string& m) {
        if (reported.insert(m).second) w.push_back(m);
    };
    static const char* kReserved[] = {"Target", "CanSeeTarget", "TimeSinceSeen", "HasHeard", "HeardPosition", "HeardLevel", "HeardTag", "HeardSource",
                                      "HeardConfidence", "EventPayload", "EventPayloadF", "SmartObject", "SmartObjectSlot", "SmartObjectHandle",
                                      "SmartObjectPos", "SmartObjectEntry", "SmartObjectYaw"};
    const auto checkKey = [&](const std::string& key, const std::string& where) {
        if (a.schema.find(key)) return;
        for (const char* r : kReserved)
            if (key == r) return;
        note(where + ": blackboard key '" + key + "' is used but not declared");
    };
    const auto checkCond = [&](const StCond& c, const std::string& where) {
        if (c.isBlackboard) checkKey(c.bb.key, where);
        else if (reg && !reg->findCondition(c.leaf.name)) note(where + ": condition '" + c.leaf.name + "' is not registered");
    };
    const auto checkAction = [&](const fmt::OcBtNode& n, const std::string& where) {
        if (reg && !reg->findAction(n.name)) note(where + ": task '" + n.name + "' is not registered");
    };
    for (const fmt::OcBtNode& e : a.evaluators) checkAction(e, "evaluator");
    for (usize i = 0; i < a.states.size(); ++i) {
        const StState& s = a.states[i];
        const std::string where = "state " + a.statePath(static_cast<i32>(i));
        for (const StCond& c : s.enter) checkCond(c, where);
        for (const fmt::OcBtNode& t : s.tasks) checkAction(t, where);
        for (const StTransition& t : s.transitions) {
            for (const StCond& c : t.conds) checkCond(c, where);
            if (t.target == StTargetKind::Goto && t.targetState >= 0 && a.states[static_cast<usize>(t.targetState)].disabled)
                note(where + ": a transition targets the disabled state " + a.statePath(t.targetState));
        }
    }
    // Reachability: from the first root, downwards, sideways (NEXT) and along Goto transitions.
    std::vector<char> seen(a.states.size(), 0);
    std::vector<i32> work;
    if (!a.roots.empty()) work.push_back(a.roots[0]);
    while (!work.empty()) {
        const i32 i = work.back();
        work.pop_back();
        if (i < 0 || seen[static_cast<usize>(i)]) continue;
        seen[static_cast<usize>(i)] = 1;
        const StState& s = a.states[static_cast<usize>(i)];
        for (const i32 c : s.children) work.push_back(c);
        for (const StTransition& t : s.transitions)
            if (t.target == StTargetKind::Goto) work.push_back(t.targetState);
        const std::vector<i32>& sibs = s.parent < 0 ? a.roots : a.states[static_cast<usize>(s.parent)].children;
        for (usize k = 0; k < sibs.size(); ++k)
            if (sibs[k] == i && k + 1 < sibs.size()) work.push_back(sibs[k + 1]);
        if (s.parent >= 0) work.push_back(s.parent);
    }
    for (usize i = 0; i < a.states.size(); ++i)
        if (!seen[i]) note("state " + a.statePath(static_cast<i32>(i)) + " is unreachable");
    return w;
}

// ---- structural edits ---------------------------------------------------------------------------------

namespace {

// Rebuilds `states` in pre-order from `roots` / `children` (which may reference states appended at the
// end), dropping anything unreachable, and re-resolves every Goto. Returns old index -> new index (-1 gone).
std::vector<i32> rebuild(StateTreeAsset& a, i32* retargeted) {
    std::vector<StState> ns;
    std::vector<i32> map(a.states.size(), -1);
    std::vector<i32> newRoots;
    std::function<i32(i32, i32)> visit = [&](i32 old, i32 parent) -> i32 {
        const i32 idx = static_cast<i32>(ns.size());
        map[static_cast<usize>(old)] = idx;
        ns.push_back(a.states[static_cast<usize>(old)]);
        ns[static_cast<usize>(idx)].parent = parent;
        const std::vector<i32> kids = a.states[static_cast<usize>(old)].children;
        ns[static_cast<usize>(idx)].children.clear();
        for (const i32 c : kids) {
            const i32 ci = visit(c, idx);
            ns[static_cast<usize>(idx)].children.push_back(ci);
        }
        return idx;
    };
    for (const i32 r : a.roots) newRoots.push_back(visit(r, -1));
    a.states = std::move(ns);
    a.roots = std::move(newRoots);
    i32 count = 0;
    for (StState& s : a.states) {
        for (StTransition& t : s.transitions) {
            if (t.target != StTargetKind::Goto || t.targetState < 0) continue;
            const i32 m = static_cast<usize>(t.targetState) < map.size() ? map[static_cast<usize>(t.targetState)] : -1;
            if (m >= 0) {
                t.targetState = m;
                t.targetPath = a.statePath(m);
            } else {
                t.target = StTargetKind::Next;
                t.targetState = -1;
                t.targetPath.clear();
                ++count;
            }
        }
    }
    if (retargeted) *retargeted = count;
    return map;
}

std::vector<i32>& siblingsOf(StateTreeAsset& a, i32 parent) {
    return parent < 0 ? a.roots : a.states[static_cast<usize>(parent)].children;
}

bool nameTaken(StateTreeAsset& a, i32 parent, const std::string& name, i32 except) {
    for (const i32 s : siblingsOf(a, parent))
        if (s != except && a.states[static_cast<usize>(s)].name == name) return true;
    return false;
}

u32 depthOf(const StateTreeAsset& a, i32 i) {
    u32 d = 0;
    for (i32 p = a.states[static_cast<usize>(i)].parent; p >= 0; p = a.states[static_cast<usize>(p)].parent) ++d;
    return d;
}

u32 heightOf(const StateTreeAsset& a, i32 i) {
    u32 h = 0;
    for (const i32 c : a.states[static_cast<usize>(i)].children) h = std::max(h, 1 + heightOf(a, c));
    return h;
}

bool validIndex(const StateTreeAsset& a, i32 i) { return i >= 0 && static_cast<usize>(i) < a.states.size(); }

} // namespace

i32 stAddState(StateTreeAsset& a, i32 parent, std::string name) {
    if (parent >= 0 && !validIndex(a, parent)) return -1;
    if (a.states.size() >= kStMaxStates) return -1;
    if (parent >= 0 && depthOf(a, parent) + 1 >= kStMaxDepth) return -1;
    if (name.empty()) {
        name = "State";
        for (int n = 2; nameTaken(a, parent, name, -1); ++n) name = "State" + std::to_string(n);
    } else if (!stStateNameValid(name) || nameTaken(a, parent, name, -1)) {
        return -1;
    }
    const i32 idx = static_cast<i32>(a.states.size());
    StState s;
    s.name = std::move(name);
    s.parent = parent;
    a.states.push_back(std::move(s));
    siblingsOf(a, parent).push_back(idx);
    return rebuild(a, nullptr)[static_cast<usize>(idx)];
}

bool stDeleteState(StateTreeAsset& a, i32 index, i32* retargeted) {
    if (!validIndex(a, index)) return false;
    std::vector<i32>& sib = siblingsOf(a, a.states[static_cast<usize>(index)].parent);
    sib.erase(std::remove(sib.begin(), sib.end(), index), sib.end());
    rebuild(a, retargeted);
    return true;
}

bool stRenameState(StateTreeAsset& a, i32 index, std::string name) {
    if (!validIndex(a, index) || !stStateNameValid(name)) return false;
    if (nameTaken(a, a.states[static_cast<usize>(index)].parent, name, index)) return false;
    a.states[static_cast<usize>(index)].name = std::move(name);
    for (StState& s : a.states)
        for (StTransition& t : s.transitions)
            if (t.target == StTargetKind::Goto && t.targetState >= 0) t.targetPath = a.statePath(t.targetState);
    return true;
}

i32 stMoveState(StateTreeAsset& a, i32 index, i32 delta) {
    if (!validIndex(a, index) || delta == 0) return -1;
    std::vector<i32>& sib = siblingsOf(a, a.states[static_cast<usize>(index)].parent);
    const auto it = std::find(sib.begin(), sib.end(), index);
    if (it == sib.end()) return -1;
    const i32 pos = static_cast<i32>(it - sib.begin());
    const i32 to = std::clamp(pos + delta, 0, static_cast<i32>(sib.size()) - 1);
    if (to == pos) return -1;
    sib.erase(it);
    sib.insert(sib.begin() + to, index);
    return rebuild(a, nullptr)[static_cast<usize>(index)];
}

i32 stReparent(StateTreeAsset& a, i32 index, i32 newParent) {
    if (!validIndex(a, index) || (newParent >= 0 && !validIndex(a, newParent))) return -1;
    for (i32 p = newParent; p >= 0; p = a.states[static_cast<usize>(p)].parent)
        if (p == index) return -1;   // into itself or a descendant
    const i32 oldParent = a.states[static_cast<usize>(index)].parent;
    if (nameTaken(a, newParent, a.states[static_cast<usize>(index)].name, index)) return -1;
    const u32 baseDepth = newParent < 0 ? 0 : depthOf(a, newParent) + 1;
    if (baseDepth + heightOf(a, index) >= kStMaxDepth) return -1;
    std::vector<i32>& old = siblingsOf(a, oldParent);
    old.erase(std::remove(old.begin(), old.end(), index), old.end());
    siblingsOf(a, newParent).push_back(index);
    a.states[static_cast<usize>(index)].parent = newParent;
    return rebuild(a, nullptr)[static_cast<usize>(index)];
}

StateTreeAsset stStarter() {
    StateTreeAsset a;
    a.name = "NewStateTree";
    a.desc = "Idle for a moment, then active";
    const auto wait = [](f32 seconds) {
        fmt::OcBtNode n;
        n.kind = fmt::OcBtNodeKind::Action;
        n.name = "Wait";
        n.params[0] = seconds;
        return n;
    };
    const i32 idle = stAddState(a, -1, "Idle");
    const i32 active = stAddState(a, -1, "Active");
    for (const i32 i : {idle, active}) {
        a.states[static_cast<usize>(i)].tasks.push_back(wait(2.0f));
        StTransition t;
        t.trigger = StTrigger::Completed;
        t.target = StTargetKind::Goto;
        t.targetState = i == idle ? active : idle;
        t.targetPath = a.states[static_cast<usize>(t.targetState)].name;
        a.states[static_cast<usize>(i)].transitions.push_back(std::move(t));
    }
    return a;
}

} // namespace aver::synapse
