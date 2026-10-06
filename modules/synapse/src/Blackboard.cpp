#include "aver/synapse/Blackboard.hpp"

#include "aver/core/Hash.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace aver::synapse {
namespace {

std::atomic<u64> g_stamp{0};
u64 nextStamp() { return ++g_stamp; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

bool parseInt(std::string_view t, i64& out) {
    const std::string tmp(t);
    if (tmp.empty()) return false;
    char* end = nullptr;
    const long long v = std::strtoll(tmp.c_str(), &end, 10);
    if (end != tmp.c_str() + tmp.size()) return false;
    out = static_cast<i64>(v);
    return true;
}

bool parseFloat(std::string_view t, f32& out) {
    const std::string tmp(t);
    if (tmp.empty()) return false;
    char* end = nullptr;
    const double v = std::strtod(tmp.c_str(), &end);
    if (end != tmp.c_str() + tmp.size()) return false;
    out = static_cast<f32>(v);
    return true;
}

bool parseVec3(std::string_view t, Vec3& out) {
    t = trim(t);
    if (!t.empty() && (t.front() == '(' || t.front() == '[')) t.remove_prefix(1);
    if (!t.empty() && (t.back() == ')' || t.back() == ']')) t.remove_suffix(1);
    f32 c[3];
    usize start = 0;
    for (int k = 0; k < 3; ++k) {
        const usize comma = t.find(',', start);
        if ((k < 2) != (comma != std::string_view::npos)) return false;
        const std::string_view part = k < 2 ? t.substr(start, comma - start) : t.substr(start);
        if (!parseFloat(trim(part), c[k])) return false;
        start = comma + 1;
    }
    out = Vec3{c[0], c[1], c[2]};
    return true;
}

bool toNumber(const BbValue& v, double& out) {
    switch (v.type) {
        case BbType::Bool: case BbType::Int: case BbType::Entity: out = static_cast<double>(v.i); return true;
        case BbType::Float: out = static_cast<double>(v.f); return true;
        default: return false;
    }
}

BbType guessType(std::string_view t) {
    i64 i; f32 f; Vec3 v;
    if (iequals(t, "true") || iequals(t, "false")) return BbType::Bool;
    if (parseInt(t, i)) return BbType::Int;
    if (parseFloat(t, f)) return BbType::Float;
    if (t.find(',') != std::string_view::npos && parseVec3(t, v)) return BbType::Vec3;
    return BbType::String;
}

} // namespace

// ---- values -------------------------------------------------------------------------------------

bool operator==(const BbValue& a, const BbValue& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case BbType::Bool: case BbType::Int: case BbType::Entity: return a.i == b.i;
        case BbType::Float: return a.f == b.f || (std::isnan(a.f) && std::isnan(b.f));
        case BbType::Vec3:  return a.v.x == b.v.x && a.v.y == b.v.y && a.v.z == b.v.z;
        case BbType::String: return a.s == b.s;
    }
    return false;
}

const char* bbTypeName(BbType t) {
    switch (t) {
        case BbType::Bool: return "Bool";   case BbType::Int: return "Int";
        case BbType::Float: return "Float"; case BbType::Vec3: return "Vec3";
        case BbType::String: return "String"; case BbType::Entity: return "Entity";
    }
    return "?";
}

bool bbTypeFromName(std::string_view name, BbType& out) {
    for (u8 k = 0; k <= static_cast<u8>(BbType::Entity); ++k)
        if (iequals(name, bbTypeName(static_cast<BbType>(k)))) { out = static_cast<BbType>(k); return true; }
    return false;
}

const char* bbOpName(BbOp op) {
    switch (op) {
        case BbOp::Eq: return "==";  case BbOp::Ne: return "!=";
        case BbOp::Lt: return "<";   case BbOp::Le: return "<=";
        case BbOp::Gt: return ">";   case BbOp::Ge: return ">=";
        case BbOp::IsSet: return "is set"; case BbOp::NotSet: return "not set";
    }
    return "?";
}

bool bbOpFromName(std::string_view name, BbOp& out) {
    for (u8 k = 0; k <= static_cast<u8>(BbOp::NotSet); ++k)
        if (name == bbOpName(static_cast<BbOp>(k))) { out = static_cast<BbOp>(k); return true; }
    return false;
}

const char* bbScopeName(BbScope s) { return s == BbScope::Shared ? "Shared" : "Agent"; }

BbValue bbDefault(BbType t) {
    BbValue v;
    v.type = t;
    return v;
}

bool bbCoerce(BbType target, const BbValue& in, BbValue& out) {
    if (in.type == target) { out = in; return true; }
    double n = 0.0;
    if (!toNumber(in, n)) return false;   // Vec3 / String only convert to themselves
    BbValue r = bbDefault(target);
    switch (target) {
        case BbType::Bool:   r.i = (n != 0.0) ? 1 : 0; break;
        case BbType::Int:    r.i = static_cast<i64>(n); break;
        case BbType::Float:  r.f = static_cast<f32>(n); break;
        case BbType::Entity: r.i = n < 0.0 ? 0 : static_cast<i64>(n); break;
        default: return false;
    }
    out = r;
    return true;
}

std::string bbToString(const BbValue& v) {
    char buf[96];
    switch (v.type) {
        case BbType::Bool:   return v.i ? "true" : "false";
        case BbType::Int:    std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v.i)); return buf;
        case BbType::Entity: std::snprintf(buf, sizeof buf, "#%lld", static_cast<long long>(v.i)); return buf;
        case BbType::Float:  std::snprintf(buf, sizeof buf, "%g", static_cast<double>(v.f)); return buf;
        case BbType::Vec3:
            std::snprintf(buf, sizeof buf, "%g,%g,%g", static_cast<double>(v.v.x),
                          static_cast<double>(v.v.y), static_cast<double>(v.v.z));
            return buf;
        case BbType::String: return v.s;
    }
    return {};
}

bool bbParse(BbType type, std::string_view text, BbValue& out) {
    text = trim(text);
    BbValue r = bbDefault(type);
    switch (type) {
        case BbType::Bool:
            if (iequals(text, "true") || text == "1") r.i = 1;
            else if (iequals(text, "false") || text == "0") r.i = 0;
            else return false;
            break;
        case BbType::Int:
            if (!parseInt(text, r.i)) return false;
            break;
        case BbType::Entity:
            if (!text.empty() && text.front() == '#') text.remove_prefix(1);
            if (!parseInt(text, r.i) || r.i < 0) return false;
            break;
        case BbType::Float:
            if (!parseFloat(text, r.f)) return false;
            break;
        case BbType::Vec3:
            if (!parseVec3(text, r.v)) return false;
            break;
        case BbType::String:
            if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
                text = text.substr(1, text.size() - 2);
            r.s = std::string(text);
            break;
    }
    out = r;
    return true;
}

bool bbTruthy(const BbValue& v) {
    switch (v.type) {
        case BbType::Bool: case BbType::Int: case BbType::Entity: return v.i != 0;
        case BbType::Float:  return v.f != 0.0f;
        case BbType::Vec3:   return v.v.x != 0.0f || v.v.y != 0.0f || v.v.z != 0.0f;
        case BbType::String: return !v.s.empty();
    }
    return false;
}

bool bbCompare(const BbValue& actual, BbOp op, const BbValue& expected) {
    if (op == BbOp::IsSet)  return bbTruthy(actual);
    if (op == BbOp::NotSet) return !bbTruthy(actual);

    double a = 0.0, b = 0.0;
    if (toNumber(actual, a) && toNumber(expected, b)) {
        switch (op) {
            case BbOp::Eq: return a == b;  case BbOp::Ne: return a != b;
            case BbOp::Lt: return a < b;   case BbOp::Le: return a <= b;
            case BbOp::Gt: return a > b;   case BbOp::Ge: return a >= b;
            default: return false;
        }
    }
    if (actual.type == BbType::String && expected.type == BbType::String) {
        const int c = actual.s.compare(expected.s);
        switch (op) {
            case BbOp::Eq: return c == 0;  case BbOp::Ne: return c != 0;
            case BbOp::Lt: return c < 0;   case BbOp::Le: return c <= 0;
            case BbOp::Gt: return c > 0;   case BbOp::Ge: return c >= 0;
            default: return false;
        }
    }
    if (actual.type == BbType::Vec3 && expected.type == BbType::Vec3) {
        if (op == BbOp::Eq) return actual == expected;
        if (op == BbOp::Ne) return !(actual == expected);
    }
    return op == BbOp::Ne;   // mismatched kinds are never equal
}

// ---- schema -------------------------------------------------------------------------------------

i32 BbSchema::indexOf(std::string_view name) const {
    for (usize i = 0; i < keys.size(); ++i)
        if (keys[i].name == name) return static_cast<i32>(i);
    return -1;
}

const BbKeyDef* BbSchema::find(std::string_view name) const {
    const i32 i = indexOf(name);
    return i < 0 ? nullptr : &keys[static_cast<usize>(i)];
}

bool BbSchema::add(BbKeyDef def) {
    if (def.name.empty() || indexOf(def.name) >= 0) return false;
    BbValue c;
    def.defaultValue = bbCoerce(def.type, def.defaultValue, c) ? c : bbDefault(def.type);
    keys.push_back(std::move(def));
    return true;
}

bool BbSchema::remove(std::string_view name) {
    const i32 i = indexOf(name);
    if (i < 0) return false;
    keys.erase(keys.begin() + i);
    return true;
}

// ---- expressions --------------------------------------------------------------------------------

namespace {
bool parseExprImpl(std::string_view text, const std::function<bool(std::string_view, BbType&)>& typeOf,
                   BbExpr& out, std::string* why) {
    const auto fail = [&](const char* m) { if (why) *why = m; return false; };
    text = trim(text);
    if (text.empty()) return fail("empty expression");

    BbExpr e;
    if (text.front() == '!') {
        e.key = std::string(trim(text.substr(1)));
        e.op = BbOp::NotSet;
        if (e.key.empty()) return fail("missing key");
        out = std::move(e);
        return true;
    }

    usize k = 0;
    while (k < text.size() && (std::isalnum(static_cast<unsigned char>(text[k])) || text[k] == '_' || text[k] == '.'))
        ++k;
    if (k == 0) return fail("missing key");
    e.key = std::string(text.substr(0, k));
    std::string_view rest = trim(text.substr(k));
    if (rest.empty()) { e.op = BbOp::IsSet; out = std::move(e); return true; }

    usize opLen = 0;
    if (rest.rfind("==", 0) == 0)      { e.op = BbOp::Eq; opLen = 2; }
    else if (rest.rfind("!=", 0) == 0) { e.op = BbOp::Ne; opLen = 2; }
    else if (rest.rfind("<=", 0) == 0) { e.op = BbOp::Le; opLen = 2; }
    else if (rest.rfind(">=", 0) == 0) { e.op = BbOp::Ge; opLen = 2; }
    else if (rest.rfind("<", 0) == 0)  { e.op = BbOp::Lt; opLen = 1; }
    else if (rest.rfind(">", 0) == 0)  { e.op = BbOp::Gt; opLen = 1; }
    else if (rest.rfind("=", 0) == 0)  { e.op = BbOp::Eq; opLen = 1; }
    else return fail("unknown operator");

    const std::string_view valueText = trim(rest.substr(opLen));
    if (valueText.empty()) return fail("missing value");

    BbType type = guessType(valueText);
    typeOf(e.key, type);
    if (!bbParse(type, valueText, e.value)) return fail("value does not parse as the key's type");
    out = std::move(e);
    return true;
}
} // namespace

bool bbParseExpr(std::string_view text, const BbSchema* schema, BbExpr& out, std::string* why) {
    return parseExprImpl(text, [schema](std::string_view key, BbType& t) {
        const BbKeyDef* d = schema ? schema->find(key) : nullptr;
        if (d) t = d->type;
        return d != nullptr;
    }, out, why);
}

bool bbParseExprFor(std::string_view text, const Blackboard& board, BbExpr& out, std::string* why) {
    return parseExprImpl(text, [&board](std::string_view key, BbType& t) {
        const i32 i = board.indexOf(key);
        if (i >= 0) t = board.keyDef(i).type;
        return i >= 0;
    }, out, why);
}

std::string bbFormatExpr(const BbExpr& e) {
    if (e.op == BbOp::IsSet) return e.key;
    if (e.op == BbOp::NotSet) return "!" + e.key;
    return e.key + " " + bbOpName(e.op) + " " + bbToString(e.value);
}

// ---- Blackboard ---------------------------------------------------------------------------------

Blackboard::~Blackboard() {
    for (const Handle& h : handles_)
        if (h.target && h.target != this) h.target->removeLocal(h.targetId);
}

Blackboard::Slot* Blackboard::slot(std::string_view name) {
    const auto it = index_.find(std::string(name));
    return it == index_.end() ? nullptr : &slots_[static_cast<usize>(it->second)];
}

const Blackboard::Slot* Blackboard::slot(std::string_view name) const {
    const auto it = index_.find(std::string(name));
    return it == index_.end() ? nullptr : &slots_[static_cast<usize>(it->second)];
}

i32 Blackboard::indexOf(std::string_view name) const {
    const auto it = index_.find(std::string(name));
    return it == index_.end() ? -1 : it->second;
}

i32 Blackboard::defineKey(const BbKeyDef& def) {
    if (def.name.empty()) return -1;
    BbValue dv;
    if (!bbCoerce(def.type, def.defaultValue, dv)) dv = bbDefault(def.type);

    if (const i32 existing = indexOf(def.name); existing >= 0) {
        Slot& s = slots_[static_cast<usize>(existing)];
        if (s.def.type != def.type) return -1;
        if (s.def.description.empty()) s.def.description = def.description;
        return existing;
    }

    Slot s;
    s.def = def;
    s.def.defaultValue = dv;
    s.value = dv;
    s.stamp = nextStamp();
    if (def.scope == BbScope::Shared && shared_) {
        BbKeyDef sd = s.def;
        if (shared_->defineKey(sd) < 0) return -1;   // the team already uses this name with another type
        s.forwarded = true;
    }
    slots_.push_back(std::move(s));
    const i32 index = static_cast<i32>(slots_.size()) - 1;
    index_[slots_.back().def.name] = index;
    return index;
}

void Blackboard::applySchema(const BbSchema& schema) {
    for (const BbKeyDef& d : schema.keys) defineKey(d);
}

bool Blackboard::removeKey(std::string_view name) {
    const i32 i = indexOf(name);
    if (i < 0) return false;
    slots_.erase(slots_.begin() + i);
    index_.clear();
    for (usize k = 0; k < slots_.size(); ++k) index_[slots_[k].def.name] = static_cast<i32>(k);
    return true;
}

const BbValue& Blackboard::valueAt(i32 index) const {
    const Slot& s = slots_[static_cast<usize>(index)];
    if (s.forwarded && shared_)
        if (const BbValue* v = shared_->get(s.def.name)) return *v;
    return s.value;
}

u64 Blackboard::stampAt(i32 index) const {
    const Slot& s = slots_[static_cast<usize>(index)];
    return (s.forwarded && shared_) ? shared_->stamp(s.def.name) : s.stamp;
}

const BbValue* Blackboard::get(std::string_view name) const {
    const Slot* s = slot(name);
    if (!s) return nullptr;
    if (s->forwarded && shared_) return shared_->get(name);
    return &s->value;
}

u64 Blackboard::stamp(std::string_view name) const {
    const Slot* s = slot(name);
    if (!s) return 0;
    return (s->forwarded && shared_) ? shared_->stamp(name) : s->stamp;
}

bool Blackboard::setLocal(Slot& s, const BbValue& coerced) {
    if (s.value == coerced) return true;
    const std::string key = s.def.name;
    const BbValue oldV = s.value;
    s.value = coerced;
    s.stamp = nextStamp();
    notify(key, oldV, coerced);   // `s` may be invalidated by an observer defining keys
    return true;
}

bool Blackboard::set(std::string_view name, const BbValue& value) {
    Slot* s = slot(name);
    if (!s) return false;
    BbValue c;
    if (!bbCoerce(s->def.type, value, c)) return false;
    if (s->forwarded && shared_) return shared_->set(name, c);
    return setLocal(*s, c);
}

bool Blackboard::reset(std::string_view name) {
    Slot* s = slot(name);
    if (!s) return false;
    if (s->forwarded && shared_) return shared_->reset(name);
    return setLocal(*s, s->def.defaultValue);
}

bool Blackboard::getBool(std::string_view name, bool fallback) const {
    const BbValue* v = get(name);
    return v ? bbTruthy(*v) : fallback;
}

i64 Blackboard::getInt(std::string_view name, i64 fallback) const {
    const BbValue* v = get(name);
    BbValue c;
    return (v && bbCoerce(BbType::Int, *v, c)) ? c.i : fallback;
}

f32 Blackboard::getFloat(std::string_view name, f32 fallback) const {
    const BbValue* v = get(name);
    BbValue c;
    return (v && bbCoerce(BbType::Float, *v, c)) ? c.f : fallback;
}

Vec3 Blackboard::getVec3(std::string_view name, const Vec3& fallback) const {
    const BbValue* v = get(name);
    return (v && v->type == BbType::Vec3) ? v->v : fallback;
}

std::string Blackboard::getString(std::string_view name, const std::string& fallback) const {
    const BbValue* v = get(name);
    return (v && v->type == BbType::String) ? v->s : fallback;
}

u32 Blackboard::getEntity(std::string_view name, u32 fallback) const {
    const BbValue* v = get(name);
    BbValue c;
    return (v && bbCoerce(BbType::Entity, *v, c)) ? static_cast<u32>(c.i) : fallback;
}

// ---- observers ----------------------------------------------------------------------------------

Blackboard::ObserverId Blackboard::addLocal(std::string_view key, Observer fn) {
    ObserverRec r;
    r.id = nextObserver_++;
    r.key = std::string(key);
    r.fn = std::move(fn);
    observers_.push_back(std::move(r));
    return observers_.back().id;
}

void Blackboard::removeLocal(ObserverId id) {
    observers_.erase(std::remove_if(observers_.begin(), observers_.end(),
                                    [id](const ObserverRec& r) { return r.id == id; }),
                     observers_.end());
}

Blackboard::ObserverId Blackboard::observe(std::string_view key, Observer fn) {
    if (!fn) return 0;
    Handle h;
    h.id = nextObserver_++;
    h.key = std::string(key);
    h.fn = fn;
    const Slot* s = key.empty() ? nullptr : slot(key);
    if (s && s->forwarded && shared_) {
        h.target = shared_;
        h.targetId = shared_->addLocal(key, std::move(fn));
    } else {
        h.target = this;
        h.targetId = addLocal(key, std::move(fn));
    }
    handles_.push_back(std::move(h));
    return handles_.back().id;
}

void Blackboard::unobserve(ObserverId id) {
    for (usize i = 0; i < handles_.size(); ++i) {
        if (handles_[i].id != id) continue;
        if (handles_[i].target) handles_[i].target->removeLocal(handles_[i].targetId);
        handles_.erase(handles_.begin() + static_cast<std::ptrdiff_t>(i));
        return;
    }
}

void Blackboard::notify(std::string_view key, const BbValue& oldV, const BbValue& newV) {
    std::vector<Observer> run;
    for (const ObserverRec& r : observers_)
        if (r.key.empty() || r.key == key) run.push_back(r.fn);
    for (const Observer& fn : run) fn(key, oldV, newV);
}

void Blackboard::rebindObservers() {
    for (Handle& h : handles_) {
        if (h.target) h.target->removeLocal(h.targetId);
        const Slot* s = h.key.empty() ? nullptr : slot(h.key);
        if (s && s->forwarded && shared_) {
            h.target = shared_;
            h.targetId = shared_->addLocal(h.key, h.fn);
        } else {
            h.target = this;
            h.targetId = addLocal(h.key, h.fn);
        }
    }
}

void Blackboard::setShared(Blackboard* s) {
    if (s == shared_ || s == this) return;
    // Detach: forwarded keys keep their current team value as a local copy.
    if (shared_) {
        for (Slot& sl : slots_) {
            if (!sl.forwarded) continue;
            if (const BbValue* v = shared_->get(sl.def.name)) sl.value = *v;
            sl.forwarded = false;
        }
    }
    shared_ = s;
    if (shared_) {
        for (Slot& sl : slots_) {
            if (sl.def.scope != BbScope::Shared) continue;
            BbKeyDef sd = sl.def;
            sd.defaultValue = sl.value;   // seeds the team board only when it does not have the key yet
            if (shared_->defineKey(sd) >= 0) sl.forwarded = true;
        }
    }
    rebindObservers();
}

// ---- SharedBlackboards --------------------------------------------------------------------------

u64 SharedBlackboards::idOf(std::string_view team) { return team.empty() ? 0 : fnv1a64(team); }

Blackboard& SharedBlackboards::get(u64 id) {
    auto it = boards_.find(id);
    if (it == boards_.end()) it = boards_.emplace(id, std::make_unique<Blackboard>()).first;
    return *it->second;
}

Blackboard* SharedBlackboards::find(u64 id) {
    const auto it = boards_.find(id);
    return it == boards_.end() ? nullptr : it->second.get();
}

} // namespace aver::synapse
