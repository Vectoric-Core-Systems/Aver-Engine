#include "aver/synapse/BtAsset.hpp"

#include "aver/formats/Avr1.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>

namespace aver::synapse {
namespace {

constexpr u32 kChunkBBSC = fmt::avrFourCC("BBSC");
constexpr u32 kChunkBDEC = fmt::avrFourCC("BDEC");
constexpr u32 kChunkBNEX = fmt::avrFourCC("BNEX");
constexpr u16 kExtrasVersion = 2;

bool fail(std::string* why, std::string m) { if (why) *why = std::move(m); return false; }

struct W {
    std::vector<u8>& b;
    void u8v(u8 v) { b.push_back(v); }
    void u32v(u32 v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(v >> (i * 8))); }
    void i32v(i32 v) { u32v(static_cast<u32>(v)); }
    void i64v(i64 v) { const u64 x = static_cast<u64>(v); u32v(static_cast<u32>(x)); u32v(static_cast<u32>(x >> 32)); }
    void f32v(f32 v) { u32 x; std::memcpy(&x, &v, 4); u32v(x); }
    void str(const std::string& s) { u32v(static_cast<u32>(s.size())); b.insert(b.end(), s.begin(), s.end()); }
    void value(const BbValue& v) {
        u8v(static_cast<u8>(v.type));
        switch (v.type) {
            case BbType::Bool: case BbType::Int: case BbType::Entity: i64v(v.i); break;
            case BbType::Float:  f32v(v.f); break;
            case BbType::Vec3:   f32v(v.v.x); f32v(v.v.y); f32v(v.v.z); break;
            case BbType::String: str(v.s); break;
        }
    }
};

struct R {
    const u8* p; const u8* e; bool ok = true;
    bool need(usize n) { if (static_cast<usize>(e - p) < n) { ok = false; return false; } return true; }
    u8  u8v()  { if (!need(1)) return 0; return *p++; }
    u32 u32v() { if (!need(4)) return 0; u32 v; std::memcpy(&v, p, 4); p += 4; return v; }
    i32 i32v() { return static_cast<i32>(u32v()); }
    i64 i64v() { const u64 lo = u32v(); const u64 hi = u32v(); return static_cast<i64>(lo | (hi << 32)); }
    f32 f32v() { const u32 x = u32v(); f32 f; std::memcpy(&f, &x, 4); return f; }
    std::string str() {
        const u32 n = u32v();
        if (!ok || !need(n)) return {};
        std::string s(reinterpret_cast<const char*>(p), n);
        p += n;
        return s;
    }
    BbValue value() {
        const u8 t = u8v();
        if (t > static_cast<u8>(BbType::Entity)) { ok = false; return {}; }
        BbValue v = bbDefault(static_cast<BbType>(t));
        switch (v.type) {
            case BbType::Bool: case BbType::Int: case BbType::Entity: v.i = i64v(); break;
            case BbType::Float:  v.f = f32v(); break;
            case BbType::Vec3:   v.v.x = f32v(); v.v.y = f32v(); v.v.z = f32v(); break;
            case BbType::String: v.s = str(); break;
        }
        return v;
    }
};

bool isLeafKind(fmt::OcBtNodeKind k) {
    return k == fmt::OcBtNodeKind::Condition || k == fmt::OcBtNodeKind::Action;
}

bool readFile(const std::string& path, std::vector<u8>& bytes, std::string* why) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return fail(why, ".ocbt: cannot open " + path);
    const std::streamoff n = f.tellg();
    if (n <= 0) return fail(why, ".ocbt: empty file " + path);
    bytes.resize(static_cast<usize>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(bytes.data()), n);
    if (!f) return fail(why, ".ocbt: short read on " + path);
    return true;
}

// The key a BbCompare/BbSet/BbClear string starts with, and where it ends.
bool leadingKey(const std::string& text, usize& begin, usize& end) {
    begin = 0;
    while (begin < text.size() && (std::isspace(static_cast<unsigned char>(text[begin])) || text[begin] == '!')) ++begin;
    end = begin;
    while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_' || text[end] == '.')) ++end;
    return end > begin;
}

bool isBbLeaf(const fmt::OcBtNode& n) {
    return isLeafKind(n.kind) && (n.name == "BbCompare" || n.name == "BbSet" || n.name == "BbClear");
}

} // namespace

// ---- conversion ---------------------------------------------------------------------------------

fmt::OcBtData BtAsset::toOcBt() const {
    fmt::OcBtData d;
    d.nodes.reserve(nodes.size());
    for (const BtAssetNode& n : nodes) d.nodes.push_back(n.node);
    return d;
}

BtDecoratorSet BtAsset::decoratorSet() const {
    BtDecoratorSet s;
    for (usize i = 0; i < nodes.size(); ++i)
        if (!nodes[i].decorators.empty()) s.byNode[static_cast<i32>(i)] = nodes[i].decorators;
    return s;
}

BtAsset BtAsset::fromOcBt(const fmt::OcBtData& data) {
    BtAsset a;
    a.nodes.reserve(data.nodes.size());
    for (const fmt::OcBtNode& n : data.nodes) {
        BtAssetNode an;
        an.node = n;
        a.nodes.push_back(std::move(an));
    }
    return a;
}

BtRuntimeTree compileBtAsset(const BtAsset& asset) {
    BtRuntimeTree t;
    t.tree = asset.toOcBt();
    t.decorators = asset.decoratorSet();
    t.schema = asset.schema;
    t.children = btBuildChildren(t.tree);
    return t;
}

bool btAssetEqual(const BtAsset& a, const BtAsset& b) {
    if (a.nodes.size() != b.nodes.size() || a.schema.keys.size() != b.schema.keys.size()) return false;
    for (usize i = 0; i < a.schema.keys.size(); ++i) {
        const BbKeyDef& x = a.schema.keys[i];
        const BbKeyDef& y = b.schema.keys[i];
        if (x.name != y.name || x.type != y.type || x.scope != y.scope || x.description != y.description ||
            x.defaultValue != y.defaultValue) return false;
    }
    for (usize i = 0; i < a.nodes.size(); ++i) {
        const BtAssetNode& x = a.nodes[i];
        const BtAssetNode& y = b.nodes[i];
        if (x.node.kind != y.node.kind || x.node.parent != y.node.parent || x.node.name != y.node.name ||
            x.node.stringParam != y.node.stringParam || x.comment != y.comment ||
            x.decorators.size() != y.decorators.size()) return false;
        for (int k = 0; k < 4; ++k)
            if (x.node.params[k] != y.node.params[k]) return false;
        for (usize d = 0; d < x.decorators.size(); ++d) {
            const BtDecorator& p = x.decorators[d];
            const BtDecorator& q = y.decorators[d];
            if (p.key != q.key || p.op != q.op || p.abort != q.abort || p.value != q.value) return false;
        }
    }
    return true;
}

std::vector<std::string> BtAsset::validate() const {
    std::vector<std::string> out;
    const auto label = [](usize i) { return "node " + std::to_string(i); };
    for (usize i = 0; i < nodes.size(); ++i) {
        const BtAssetNode& n = nodes[i];
        for (const BtDecorator& d : n.decorators) {
            const BbKeyDef* def = schema.find(d.key);
            if (!def) { out.push_back(label(i) + ": decorator uses unknown key '" + d.key + "'"); continue; }
            BbValue c;
            const bool needsValue = d.op != BbOp::IsSet && d.op != BbOp::NotSet;
            if (needsValue && !bbCoerce(def->type, d.value, c))
                out.push_back(label(i) + ": decorator value on '" + d.key + "' is not a " + bbTypeName(def->type));
        }
        if (isBbLeaf(n.node)) {
            if (n.node.name == "BbClear") {
                usize b, e;
                if (!leadingKey(n.node.stringParam, b, e) || !schema.find(n.node.stringParam.substr(b, e - b)))
                    out.push_back(label(i) + ": BbClear names an unknown key");
            } else {
                BbExpr ex;
                std::string why;
                if (!bbParseExpr(n.node.stringParam, &schema, ex, &why))
                    out.push_back(label(i) + ": " + n.node.name + " text '" + n.node.stringParam + "': " + why);
                else if (!schema.find(ex.key))
                    out.push_back(label(i) + ": " + n.node.name + " uses unknown key '" + ex.key + "'");
            }
        }
    }
    return out;
}

// ---- file ---------------------------------------------------------------------------------------

namespace {

bool hasExtras(const BtAsset& in) {
    if (!in.schema.keys.empty()) return true;
    for (const BtAssetNode& n : in.nodes)
        if (!n.decorators.empty() || !n.comment.empty()) return true;
    return false;
}

bool buildContainer(const BtAsset& in, fmt::Avr1File& file, std::string* why) {
    std::vector<u8> base;
    if (!fmt::writeOcBt(in.toOcBt(), base, why)) return false;
    if (!fmt::parseAvr1(base.data(), base.size(), file, why)) return false;

    if (!in.schema.keys.empty()) {
        std::vector<u8> b;
        W w{b};
        w.u32v(static_cast<u32>(in.schema.keys.size()));
        for (const BbKeyDef& k : in.schema.keys) {
            w.str(k.name);
            w.u8v(static_cast<u8>(k.type));
            w.u8v(static_cast<u8>(k.scope));
            w.value(k.defaultValue);
            w.str(k.description);
        }
        file.add(kChunkBBSC, std::move(b));
    }

    u32 decoCount = 0;
    for (const BtAssetNode& n : in.nodes) decoCount += static_cast<u32>(n.decorators.size());
    if (decoCount > 0) {
        std::vector<u8> b;
        W w{b};
        w.u32v(decoCount);
        for (usize i = 0; i < in.nodes.size(); ++i)
            for (const BtDecorator& d : in.nodes[i].decorators) {
                w.i32v(static_cast<i32>(i));
                w.str(d.key);
                w.u8v(static_cast<u8>(d.op));
                w.u8v(static_cast<u8>(d.abort));
                w.value(d.value);
            }
        file.add(kChunkBDEC, std::move(b));
    }

    bool anyComment = false;
    for (const BtAssetNode& n : in.nodes) if (!n.comment.empty()) anyComment = true;
    if (anyComment) {
        std::vector<u8> b;
        W w{b};
        w.u32v(static_cast<u32>(in.nodes.size()));
        for (const BtAssetNode& n : in.nodes) w.str(n.comment);
        file.add(kChunkBNEX, std::move(b));
    }
    file.contentVersion = kExtrasVersion;
    return true;
}

} // namespace

bool writeBtAsset(const BtAsset& in, std::vector<u8>& out, std::string* why) {
    if (!hasExtras(in)) return fmt::writeOcBt(in.toOcBt(), out, why);   // exactly a plain .ocbt
    fmt::Avr1File file;
    if (!buildContainer(in, file, why)) return false;
    return fmt::writeAvr1(file, out, why);
}

bool parseBtAsset(const u8* bytes, usize size, BtAsset& out, std::string* why) {
    fmt::OcBtData data;
    if (!fmt::parseOcBt(bytes, size, data, why)) return false;
    fmt::Avr1File file;
    if (!fmt::parseAvr1(bytes, size, file, why)) return false;

    BtAsset a = BtAsset::fromOcBt(data);

    if (const fmt::AvrChunk* c = file.find(kChunkBBSC)) {
        R r{c->data.data(), c->data.data() + c->data.size()};
        const u32 n = r.u32v();
        if (n > c->data.size()) return fail(why, ".ocbt: BBSC count is larger than its chunk");
        for (u32 i = 0; i < n && r.ok; ++i) {
            BbKeyDef k;
            k.name = r.str();
            const u8 type = r.u8v();
            const u8 scope = r.u8v();
            if (type > static_cast<u8>(BbType::Entity) || scope > static_cast<u8>(BbScope::Shared))
                return fail(why, ".ocbt: BBSC key '" + k.name + "' has an unknown type or scope");
            k.type = static_cast<BbType>(type);
            k.scope = static_cast<BbScope>(scope);
            k.defaultValue = r.value();
            k.description = r.str();
            if (!r.ok) break;
            if (!a.schema.add(std::move(k))) return fail(why, ".ocbt: BBSC has an empty or duplicate key name");
        }
        if (!r.ok) return fail(why, ".ocbt: truncated BBSC chunk");
    }

    if (const fmt::AvrChunk* c = file.find(kChunkBDEC)) {
        R r{c->data.data(), c->data.data() + c->data.size()};
        const u32 n = r.u32v();
        if (n > c->data.size()) return fail(why, ".ocbt: BDEC count is larger than its chunk");
        for (u32 i = 0; i < n && r.ok; ++i) {
            const i32 node = r.i32v();
            BtDecorator d;
            d.key = r.str();
            const u8 op = r.u8v();
            const u8 abort = r.u8v();
            d.value = r.value();
            if (!r.ok) break;
            if (node < 0 || static_cast<usize>(node) >= a.nodes.size() || op > static_cast<u8>(BbOp::NotSet) ||
                abort > static_cast<u8>(BtAbortMode::Both))
                return fail(why, ".ocbt: BDEC record " + std::to_string(i) + " is out of range");
            d.op = static_cast<BbOp>(op);
            d.abort = static_cast<BtAbortMode>(abort);
            a.nodes[static_cast<usize>(node)].decorators.push_back(std::move(d));
        }
        if (!r.ok) return fail(why, ".ocbt: truncated BDEC chunk");
    }

    if (const fmt::AvrChunk* c = file.find(kChunkBNEX)) {
        R r{c->data.data(), c->data.data() + c->data.size()};
        const u32 n = r.u32v();
        if (n != a.nodes.size()) return fail(why, ".ocbt: BNEX does not match the node count");
        for (u32 i = 0; i < n && r.ok; ++i) a.nodes[i].comment = r.str();
        if (!r.ok) return fail(why, ".ocbt: truncated BNEX chunk");
    }

    out = std::move(a);
    return true;
}

bool saveBtAsset(const std::string& path, const BtAsset& in, std::string* why) {
    if (!hasExtras(in)) return fmt::saveOcBt(path, in.toOcBt(), why);
    fmt::Avr1File file;
    if (!buildContainer(in, file, why)) return false;
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    return fmt::saveAvr1(path, file, why);
}

bool loadBtAsset(const std::string& path, BtAsset& out, std::string* why) {
    std::vector<u8> bytes;
    if (!readFile(path, bytes, why)) return false;
    return parseBtAsset(bytes.data(), bytes.size(), out, why);
}

// ---- structural edits ---------------------------------------------------------------------------

namespace {

using ChildOrder = std::unordered_map<i32, std::vector<i32>>;

ChildOrder captureOrder(const std::vector<BtAssetNode>& nodes) {
    ChildOrder order;
    for (i32 i = 0; i < static_cast<i32>(nodes.size()); ++i)
        if (nodes[static_cast<usize>(i)].node.parent != fmt::kOcBtNoParent)
            order[nodes[static_cast<usize>(i)].node.parent].push_back(i);
    return order;
}

void emitDfs(const std::vector<BtAssetNode>& oldNodes, const ChildOrder& order, i32 oldIndex,
             i32 newParent, std::vector<BtAssetNode>& out, std::unordered_map<i32, i32>& remap) {
    const i32 newIndex = static_cast<i32>(out.size());
    remap[oldIndex] = newIndex;
    out.push_back(oldNodes[static_cast<usize>(oldIndex)]);
    out.back().node.parent = newParent;
    const auto it = order.find(oldIndex);
    if (it == order.end()) return;
    for (const i32 child : it->second) emitDfs(oldNodes, order, child, newIndex, out, remap);
}

// Pre-order from node 0; anything unreachable is dropped (which is how a delete works).
std::unordered_map<i32, i32> rebuild(std::vector<BtAssetNode>& nodes, const ChildOrder& order) {
    std::vector<BtAssetNode> out;
    out.reserve(nodes.size());
    std::unordered_map<i32, i32> remap;
    if (!nodes.empty()) emitDfs(nodes, order, 0, fmt::kOcBtNoParent, out, remap);
    nodes = std::move(out);
    return remap;
}

bool inRange(const std::vector<BtAssetNode>& nodes, i32 i) { return i >= 0 && static_cast<usize>(i) < nodes.size(); }

bool isAncestorOf(const std::vector<BtAssetNode>& nodes, i32 maybeAncestor, i32 index) {
    for (i32 walk = index; walk != fmt::kOcBtNoParent && inRange(nodes, walk);
         walk = nodes[static_cast<usize>(walk)].node.parent)
        if (walk == maybeAncestor) return true;
    return false;
}

void detach(ChildOrder& order, const std::vector<BtAssetNode>& nodes, i32 child) {
    const i32 parent = nodes[static_cast<usize>(child)].node.parent;
    const auto it = order.find(parent);
    if (it == order.end()) return;
    auto& list = it->second;
    list.erase(std::remove(list.begin(), list.end(), child), list.end());
}

const char* defaultLeafName(fmt::OcBtNodeKind k) { return k == fmt::OcBtNodeKind::Condition ? "HasTarget" : "Wait"; }

} // namespace

const char* btNodeKindName(fmt::OcBtNodeKind kind) {
    switch (kind) {
        case fmt::OcBtNodeKind::Selector:  return "Selector";
        case fmt::OcBtNodeKind::Sequence:  return "Sequence";
        case fmt::OcBtNodeKind::Parallel:  return "Parallel";
        case fmt::OcBtNodeKind::Inverter:  return "Inverter";
        case fmt::OcBtNodeKind::Succeeder: return "Succeeder";
        case fmt::OcBtNodeKind::Cooldown:  return "Cooldown";
        case fmt::OcBtNodeKind::Condition: return "Condition";
        case fmt::OcBtNodeKind::Action:    return "Action";
    }
    return "?";
}

std::vector<i32> btAssetChildren(const BtAsset& a, i32 parent) {
    std::vector<i32> out;
    for (i32 i = 0; i < static_cast<i32>(a.nodes.size()); ++i)
        if (a.nodes[static_cast<usize>(i)].node.parent == parent) out.push_back(i);
    return out;
}

i32 btAssetAddChild(BtAsset& a, i32 parent, fmt::OcBtNodeKind kind, const std::string& name) {
    if (!inRange(a.nodes, parent)) return -1;
    BtAssetNode n;
    n.node.kind = kind;
    n.node.parent = parent;
    if (isLeafKind(kind)) n.node.name = name.empty() ? defaultLeafName(kind) : name;
    if (kind == fmt::OcBtNodeKind::Cooldown || n.node.name == "Wait") n.node.params[0] = 1.0f;

    ChildOrder order = captureOrder(a.nodes);
    const i32 added = static_cast<i32>(a.nodes.size());
    a.nodes.push_back(std::move(n));
    order[parent].push_back(added);
    return rebuild(a.nodes, order)[added];
}

i32 btAssetDeleteSubtree(BtAsset& a, i32 index) {
    if (!inRange(a.nodes, index) || index == 0) return -1;
    const i32 parent = a.nodes[static_cast<usize>(index)].node.parent;
    ChildOrder order = captureOrder(a.nodes);
    detach(order, a.nodes, index);
    order.erase(index);
    return rebuild(a.nodes, order)[parent];
}

i32 btAssetReparent(BtAsset& a, i32 index, i32 newParent) {
    if (!inRange(a.nodes, index) || !inRange(a.nodes, newParent) || index == 0) return -1;
    if (isAncestorOf(a.nodes, index, newParent)) return -1;
    ChildOrder order = captureOrder(a.nodes);
    detach(order, a.nodes, index);
    a.nodes[static_cast<usize>(index)].node.parent = newParent;
    order[newParent].push_back(index);
    return rebuild(a.nodes, order)[index];
}

bool btAssetCanMoveSibling(const BtAsset& a, i32 index, i32 delta) {
    if (!inRange(a.nodes, index) || index == 0 || delta == 0) return false;
    const std::vector<i32> sibs = btAssetChildren(a, a.nodes[static_cast<usize>(index)].node.parent);
    for (usize i = 0; i < sibs.size(); ++i) {
        if (sibs[i] != index) continue;
        const i64 target = static_cast<i64>(i) + delta;
        return target >= 0 && target < static_cast<i64>(sibs.size());
    }
    return false;
}

i32 btAssetMoveSibling(BtAsset& a, i32 index, i32 delta) {
    if (!btAssetCanMoveSibling(a, index, delta)) return -1;
    ChildOrder order = captureOrder(a.nodes);
    auto& list = order[a.nodes[static_cast<usize>(index)].node.parent];
    const auto pos = std::find(list.begin(), list.end(), index);
    const std::ptrdiff_t from = pos - list.begin();
    std::swap(list[static_cast<usize>(from)], list[static_cast<usize>(from + (delta < 0 ? -1 : 1))]);
    return rebuild(a.nodes, order)[index];
}

void btAssetSetKind(BtAsset& a, i32 index, fmt::OcBtNodeKind kind) {
    if (!inRange(a.nodes, index)) return;
    fmt::OcBtNode& n = a.nodes[static_cast<usize>(index)].node;
    n.kind = kind;
    if (isLeafKind(kind)) { if (n.name.empty()) n.name = defaultLeafName(kind); }
    else { n.name.clear(); n.stringParam.clear(); }
}

bool btAssetRenameKey(BtAsset& a, const std::string& from, const std::string& to) {
    if (to.empty() || a.schema.indexOf(from) < 0 || a.schema.indexOf(to) >= 0) return false;
    a.schema.keys[static_cast<usize>(a.schema.indexOf(from))].name = to;
    for (BtAssetNode& n : a.nodes) {
        for (BtDecorator& d : n.decorators)
            if (d.key == from) d.key = to;
        if (!isBbLeaf(n.node)) continue;
        usize b, e;
        if (leadingKey(n.node.stringParam, b, e) && n.node.stringParam.substr(b, e - b) == from)
            n.node.stringParam.replace(b, e - b, to);
    }
    return true;
}

BtAsset btAssetStarter() {
    BtAsset a;
    {
        BbKeyDef k;
        k.name = "Target"; k.type = BbType::Entity; k.description = "Current target entity";
        a.schema.add(k);
        BbKeyDef v;
        v.name = "CanSeeTarget"; v.type = BbType::Bool; v.description = "Synced from perception";
        a.schema.add(v);
        BbKeyDef al;
        al.name = "Alert"; al.type = BbType::Bool; al.scope = BbScope::Shared;
        al.description = "Shared by the whole team";
        a.schema.add(al);
    }
    BtAssetNode root;
    root.node.kind = fmt::OcBtNodeKind::Selector;
    root.node.parent = fmt::kOcBtNoParent;
    a.nodes.push_back(root);

    BtAssetNode seq;
    seq.node.kind = fmt::OcBtNodeKind::Sequence;
    seq.node.parent = 0;
    BtDecorator d;
    d.key = "CanSeeTarget";
    d.op = BbOp::IsSet;
    d.abort = BtAbortMode::Both;
    seq.decorators.push_back(d);
    seq.comment = "Engage while the target is visible";
    a.nodes.push_back(seq);

    BtAssetNode look;
    look.node.kind = fmt::OcBtNodeKind::Action;
    look.node.parent = 1;
    look.node.name = "LookAt";
    a.nodes.push_back(look);

    BtAssetNode hold;
    hold.node.kind = fmt::OcBtNodeKind::Action;
    hold.node.parent = 1;
    hold.node.name = "Wait";
    hold.node.params[0] = 1.0f;
    a.nodes.push_back(hold);

    BtAssetNode idle;
    idle.node.kind = fmt::OcBtNodeKind::Action;
    idle.node.parent = 0;
    idle.node.name = "Wait";
    idle.node.params[0] = 2.0f;
    a.nodes.push_back(idle);
    return a;
}

} // namespace aver::synapse
