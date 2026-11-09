#include "aver/world/PcgGraph.hpp"

#include "aver/core/Hash.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <set>

namespace aver::world {

// ---- ILandscapeQuery ----------------------------------------------------------------------------------------------------

bool ILandscapeQuery::normalAt(f64 x, f64 y, Vec3& outN) const {
    constexpr f64 d = 50.0;
    f32 hx0, hx1, hy0, hy1;
    if (!heightAt(x - d, y, hx0) || !heightAt(x + d, y, hx1) || !heightAt(x, y - d, hy0) || !heightAt(x, y + d, hy1))
        return false;
    // Surface z = h(x, y): the normal is (-dh/dx, -dh/dy, 1) normalised.
    const Vec3 n{-(hx1 - hx0) / static_cast<f32>(2.0 * d), -(hy1 - hy0) / static_cast<f32>(2.0 * d), 1.0f};
    outN = n.getSafeNormal();
    return true;
}

usize PcgResult::instanceCount() const {
    usize n = 0;
    for (const PcgInstanceGroup& g : groups) n += g.items.size();
    return n;
}

// ---- PcgParams ----------------------------------------------------------------------------------------------------------

PcgParams::PcgParams(const std::vector<std::string>& tokens) {
    for (const std::string& t : tokens) {
        const usize eq = t.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        kv_.emplace_back(t.substr(0, eq), t.substr(eq + 1));
    }
}
const std::string* PcgParams::find(std::string_view k) const {
    for (const auto& kv : kv_) if (kv.first == k) return &kv.second;
    return nullptr;
}
bool PcgParams::has(std::string_view k) const { return find(k) != nullptr; }
std::string PcgParams::str(std::string_view k, std::string_view def) const {
    const std::string* v = find(k);
    return v ? *v : std::string(def);
}
f64 PcgParams::num(std::string_view k, f64 def) const {
    const std::string* v = find(k);
    if (!v || v->empty()) return def;
    char* end = nullptr;
    const f64 r = std::strtod(v->c_str(), &end);
    return end == v->c_str() ? def : r;
}
i32 PcgParams::integer(std::string_view k, i32 def) const { return static_cast<i32>(num(k, static_cast<f64>(def))); }
bool PcgParams::flag(std::string_view k, bool def) const {
    const std::string* v = find(k);
    if (!v) return def;
    return *v == "1" || *v == "true" || *v == "yes" || *v == "on";
}
Vec3 PcgParams::vec3(std::string_view k, const Vec3& def) const {
    const std::string* v = find(k);
    if (!v) return def;
    f32 c[3] = {def.x, def.y, def.z};
    const char* s = v->c_str();
    for (int a = 0; a < 3 && *s; ++a) {
        char* end = nullptr;
        c[a] = static_cast<f32>(std::strtod(s, &end));
        if (end == s) break;
        s = end;
        if (*s == ',') ++s;
    }
    return Vec3{c[0], c[1], c[2]};
}
std::vector<Vec3> PcgParams::polyline(std::string_view k) const {
    std::vector<Vec3> out;
    const std::string* v = find(k);
    if (!v) return out;
    for (usize b = 0; b <= v->size();) {
        usize e = v->find('|', b);
        if (e == std::string::npos) e = v->size();
        if (e > b) {
            PcgParams one({"p=" + v->substr(b, e - b)});
            out.push_back(one.vec3("p", {}));
        }
        b = e + 1;
    }
    return out;
}

// ---- registry helpers ---------------------------------------------------------------------------------------------------

const PcgNodeDef* pcgFindNode(std::string_view type) {
    for (const PcgNodeDef& d : pcgNodeDefs()) if (type == d.type) return &d;
    return nullptr;
}

fmt::OcGraphNode pcgMakeNode(std::string_view id, std::string_view type, const std::vector<std::string>& params) {
    fmt::OcGraphNode n;
    n.id = std::string(id);
    n.type = std::string(type);
    n.hasPosition = false;
    n.extraTokens = params;
    if (const PcgNodeDef* d = pcgFindNode(type)) {
        for (const char* p : d->inputs) n.pins.push_back({p, "points", false, ""});
        for (const char* p : d->outputs) n.pins.push_back({p, "points", true, ""});
    }
    return n;
}

bool pcgIsGraph(const fmt::OcGraphData& g) {
    return g.domain.size() == 3 && (g.domain[0] | 32) == 'p' && (g.domain[1] | 32) == 'c' && (g.domain[2] | 32) == 'g';
}

namespace {

int indexOf(const std::vector<const char*>& pins, std::string_view name) {
    for (usize i = 0; i < pins.size(); ++i) if (name == pins[i]) return static_cast<int>(i);
    return -1;
}

u64 mix64(u64 h, std::string_view s) {
    for (char c : s) { h ^= static_cast<u8>(c); h *= kFnv1a64Prime; }
    h ^= 0xFFu; h *= kFnv1a64Prime;   // separator, so ("ab","c") != ("a","bc")
    return h;
}
u64 mixBytes(u64 h, const void* d, usize n) {
    const u8* b = static_cast<const u8*>(d);
    for (usize i = 0; i < n; ++i) { h ^= b[i]; h *= kFnv1a64Prime; }
    return h;
}

// ---- evaluation ---------------------------------------------------------------------------------------------------------

constexpr int kMaxSubgraphDepth = 8;

struct LinkRef { int node = -1; int pin = -1; };

class Evaluator {
public:
    Evaluator(const PcgContext& ctx, PcgResult& res)
        : ctx_(ctx), res_(res), start_(std::chrono::steady_clock::now()) {}

    // Runs `g`. `subInput` feeds Input nodes; `collect` receives Output nodes' points (null at the top level).
    void run(const fmt::OcGraphData& g, const PcgPoints* subInput, PcgPoints* collect, i32 seedBase, int depth) {
        Frame f;
        f.g = &g; f.subInput = subInput; f.collect = collect; f.seedBase = seedBase; f.depth = depth;
        const usize N = g.nodes.size();
        f.defs.assign(N, nullptr);
        f.state.assign(N, 0);
        f.outs.assign(N, nullptr);
        f.in.assign(N, {});
        std::map<std::string, int> idx;
        for (usize i = 0; i < N; ++i) {
            idx[g.nodes[i].id] = static_cast<int>(i);
            f.defs[i] = pcgFindNode(g.nodes[i].type);
            if (!f.defs[i]) diag(g.nodes[i].id, "unknown node type '" + g.nodes[i].type + "'", true);
        }
        for (usize i = 0; i < N; ++i) if (f.defs[i]) f.in[i].assign(f.defs[i]->inputs.size(), LinkRef{});
        for (const fmt::OcGraphLink& l : g.links) {
            const auto s = idx.find(l.sourceNode), d = idx.find(l.destNode);
            if (s == idx.end() || d == idx.end() || !f.defs[s->second] || !f.defs[d->second]) continue;
            const int sp = indexOf(f.defs[s->second]->outputs, l.sourcePin);
            const int dp = indexOf(f.defs[d->second]->inputs, l.destPin);
            if (sp < 0 || dp < 0) { diag(l.destNode, "link to unknown pin " + l.sourceNode + "." + l.sourcePin + " -> " + l.destNode + "." + l.destPin, true); continue; }
            f.in[d->second][dp] = {s->second, sp};
        }
        bool anyRoot = false;
        for (usize i = 0; i < N && !stop_; ++i) {
            if (!f.defs[i]) continue;
            const bool root = f.defs[i]->sink || (collect && std::string_view(f.defs[i]->type) == "Output");
            if (!root) continue;
            anyRoot = true;
            node(f, static_cast<int>(i));
        }
        if (!anyRoot && depth == 0) diag("", "the graph has no spawner node, so it produces nothing", false);
    }

    u32 run_ = 0, cached_ = 0;

private:
    struct Frame {
        const fmt::OcGraphData* g = nullptr;
        const PcgPoints* subInput = nullptr;
        PcgPoints* collect = nullptr;
        i32 seedBase = 0;
        int depth = 0;
        std::vector<const PcgNodeDef*> defs;
        std::vector<int> state;
        std::vector<std::shared_ptr<const PcgNodeOut>> outs;
        std::vector<std::vector<LinkRef>> in;
    };

    void diag(const std::string& id, std::string m, bool error) {
        res_.diags.push_back({id, std::move(m), error});
        if (error) res_.ok = false;
    }

    bool shouldStop() {
        if (stop_) return true;
        if (ctx_.cancel && ctx_.cancel->load(std::memory_order_relaxed)) {
            res_.cancelled = true; res_.ok = false; stop_ = true;
            diag("", "cancelled", true);
        } else if (ctx_.budgetMs > 0.0) {
            const f64 ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start_).count();
            if (ms > ctx_.budgetMs) {
                res_.timedOut = true; res_.ok = false; stop_ = true;
                diag("", "time budget exceeded", true);
            }
        }
        return stop_;
    }

    static const std::shared_ptr<const PcgNodeOut>& emptyOut() {
        static const std::shared_ptr<const PcgNodeOut> e = std::make_shared<PcgNodeOut>();
        return e;
    }

    void sink(const PcgNodeOut& o) {
        for (const PcgInstanceGroup& g : o.groups) {
            auto it = std::find_if(res_.groups.begin(), res_.groups.end(),
                                   [&](const PcgInstanceGroup& x) { return x.mesh == g.mesh && x.material == g.material; });
            if (it == res_.groups.end()) { res_.groups.push_back({g.mesh, g.material, {}}); it = res_.groups.end() - 1; }
            it->items.insert(it->items.end(), g.items.begin(), g.items.end());
        }
        res_.actors.insert(res_.actors.end(), o.actors.begin(), o.actors.end());
    }

    std::shared_ptr<const PcgNodeOut> node(Frame& f, int i) {
        if (f.state[i] == 2) return f.outs[i];
        const fmt::OcGraphNode& gn = f.g->nodes[i];
        if (f.state[i] == 1) { diag(gn.id, "cycle through this node", true); return emptyOut(); }
        const PcgNodeDef* def = f.defs[i];
        if (!def) { f.state[i] = 2; f.outs[i] = emptyOut(); return emptyOut(); }
        f.state[i] = 1;

        std::vector<std::shared_ptr<const PcgNodeOut>> ins(def->inputs.size());
        std::vector<const PcgPoints*> inPts(def->inputs.size(), nullptr);
        for (usize k = 0; k < def->inputs.size(); ++k) {
            const LinkRef r = f.in[i][k];
            if (r.node < 0) continue;
            ins[k] = node(f, r.node);
            if (ins[k] && static_cast<usize>(r.pin) < ins[k]->pins.size()) inPts[k] = &ins[k]->pins[r.pin];
        }
        if (shouldStop()) { f.state[i] = 2; f.outs[i] = emptyOut(); return emptyOut(); }

        const PcgParams params(gn.extraTokens);
        const i32 nodeSeed = static_cast<i32>(pcgHash2(f.seedBase, static_cast<i32>(pcgHashString(gn.id))));
        std::shared_ptr<const PcgNodeOut> result;
        const std::string_view type = def->type;

        if (def->fn) {
            u64 key = mix64(kFnv1a64OffsetBasis, def->type);
            for (const std::string& t : gn.extraTokens) key = mix64(key, t);
            key = mixBytes(key, &nodeSeed, sizeof nodeSeed);
            key = mixBytes(key, &ctx_.bounds, sizeof ctx_.bounds);
            key = mixBytes(key, &ctx_.environmentStamp, sizeof ctx_.environmentStamp);
            key = mixBytes(key, &ctx_.maxPoints, sizeof ctx_.maxPoints);
            key = mixBytes(key, ctx_.landscape ? "L" : "-", 1);
            for (const PcgPoints* p : inPts) { const u64 h = p ? p->contentHash() : 0; key = mixBytes(key, &h, sizeof h); }
            if (ctx_.cache) {
                const auto it = ctx_.cache->entries.find(key);
                if (it != ctx_.cache->entries.end()) {
                    result = it->second;
                    ++ctx_.cache->hits;
                    ++cached_;
                }
            }
            if (!result) {
                auto out = std::make_shared<PcgNodeOut>();
                PcgNodeCtx nc{params, nodeSeed, ctx_, gn.id, out->diags};
                def->fn(nc, inPts, *out);
                out->pins.resize(def->outputs.size());
                result = out;
                ++run_;
                if (ctx_.cache) { ++ctx_.cache->misses; ctx_.cache->entries[key] = result; }
            }
            for (const PcgDiag& d : result->diags) diag(d.node, d.message, d.error);
            if (def->sink) sink(*result);
        } else if (type == "Input") {
            auto out = std::make_shared<PcgNodeOut>();
            out->pins.resize(1);
            if (f.subInput) out->pins[0] = *f.subInput;
            result = out;
        } else if (type == "Output") {
            if (f.collect && inPts[0]) f.collect->appendAll(*inPts[0]);
            result = emptyOut();
        } else {   // Subgraph, Loop
            result = subgraphNode(f, gn, params, nodeSeed, type == "Loop", inPts[0]);
        }

        if (f.depth == 0 && !result->pins.empty()) res_.nodePointCount[gn.id] = static_cast<u32>(result->pins[0].size());
        f.state[i] = 2;
        f.outs[i] = result;
        return result;
    }

    std::shared_ptr<const PcgNodeOut> subgraphNode(Frame& f, const fmt::OcGraphNode& gn, const PcgParams& params, i32 nodeSeed,
                                                   bool loop, const PcgPoints* input) {
        auto out = std::make_shared<PcgNodeOut>();
        out->pins.resize(1);
        const std::string name = params.str("graph");
        const fmt::OcGraphData* sub = (ctx_.graphLoader && !name.empty()) ? ctx_.graphLoader(name) : nullptr;
        if (!sub) { diag(gn.id, "unknown graph '" + name + "'", true); return out; }
        if (f.depth + 1 >= kMaxSubgraphDepth) { diag(gn.id, "subgraph nesting is too deep (recursive graph?)", true); return out; }
        static const PcgPoints kNone;
        const PcgPoints& in = input ? *input : kNone;
        if (!loop) {
            run(*sub, &in, &out->pins[0], nodeSeed, f.depth + 1);
            return out;
        }
        const usize cap = static_cast<usize>(std::max(1, params.integer("max", 10000)));
        usize n = in.size();
        if (n > cap) { diag(gn.id, "loop truncated to " + std::to_string(cap) + " iterations", false); n = cap; }
        for (usize k = 0; k < n && !stop_; ++k) {
            const PcgPoints one = in.select({static_cast<u32>(k)});
            run(*sub, &one, &out->pins[0], static_cast<i32>(pcgHash2(nodeSeed, in.seed[k])), f.depth + 1);
        }
        return out;
    }

    const PcgContext& ctx_;
    PcgResult& res_;
    std::chrono::steady_clock::time_point start_;
    bool stop_ = false;
};

} // namespace

// ---- public -------------------------------------------------------------------------------------------------------------

PcgResult pcgEvaluate(const fmt::OcGraphData& g, const PcgContext& ctx) {
    PcgResult res;
    if (!pcgIsGraph(g)) {
        res.ok = false;
        res.diags.push_back({"", "not a pcg graph (DOMAIN pcg expected)", true});
        return res;
    }
    Evaluator ev(ctx, res);
    ev.run(g, nullptr, nullptr, ctx.seed, 0);
    res.nodesRun = ev.run_;
    res.nodesCached = ev.cached_;
    return res;
}

std::vector<PcgDiag> pcgValidate(const fmt::OcGraphData& g) {
    std::vector<PcgDiag> out;
    const auto err = [&](const std::string& id, std::string m) { out.push_back({id, std::move(m), true}); };
    if (!pcgIsGraph(g)) err("", "DOMAIN pcg is missing");
    std::map<std::string, const PcgNodeDef*> byId;
    bool anySink = false;
    for (const fmt::OcGraphNode& n : g.nodes) {
        const PcgNodeDef* d = pcgFindNode(n.type);
        if (!d) { err(n.id, "unknown node type '" + n.type + "'"); continue; }
        byId[n.id] = d;
        anySink = anySink || d->sink;
    }
    if (!anySink) out.push_back({"", "the graph has no spawner node", false});
    std::set<std::pair<std::string, std::string>> fed;
    std::map<std::string, std::vector<std::string>> succ;
    for (const fmt::OcGraphLink& l : g.links) {
        const auto s = byId.find(l.sourceNode), d = byId.find(l.destNode);
        if (s == byId.end() || d == byId.end()) { err(l.destNode, "link between missing or unknown nodes"); continue; }
        if (indexOf(s->second->outputs, l.sourcePin) < 0) err(l.sourceNode, "no output pin '" + l.sourcePin + "'");
        if (indexOf(d->second->inputs, l.destPin) < 0) err(l.destNode, "no input pin '" + l.destPin + "'");
        if (!fed.insert({l.destNode, l.destPin}).second) err(l.destNode, "input pin '" + l.destPin + "' has more than one link");
        succ[l.sourceNode].push_back(l.destNode);
    }
    // Cycle check: iterative colouring so a hostile graph cannot overflow the stack.
    std::map<std::string, int> colour;
    for (const auto& kv : byId) {
        if (colour[kv.first] != 0) continue;
        std::vector<std::pair<std::string, usize>> stack{{kv.first, 0}};
        colour[kv.first] = 1;
        while (!stack.empty()) {
            auto& [id, k] = stack.back();
            const auto& nx = succ[id];
            if (k < nx.size()) {
                const std::string next = nx[k++];
                if (colour[next] == 1) { err(next, "cycle through this node"); }
                else if (colour[next] == 0) { colour[next] = 1; stack.push_back({next, 0}); }
            } else {
                colour[id] = 2;
                stack.pop_back();
            }
        }
    }
    return out;
}

f32 pcgGraphMargin(const fmt::OcGraphData& g, const PcgContext* ctx) {
    static thread_local int depth = 0;
    f32 total = 0.0f;
    for (const fmt::OcGraphNode& n : g.nodes) {
        const PcgNodeDef* d = pcgFindNode(n.type);
        if (!d) continue;
        const PcgParams p(n.extraTokens);
        total += d->margin ? d->margin(p) : 0.0f;
        const std::string_view t = d->type;
        if ((t == "Subgraph" || t == "Loop") && ctx && ctx->graphLoader && depth < kMaxSubgraphDepth) {
            if (const fmt::OcGraphData* sub = ctx->graphLoader(p.str("graph"))) {
                ++depth;
                total += pcgGraphMargin(*sub, ctx);
                --depth;
            }
        }
    }
    return total;
}

u64 pcgGraphHash(const fmt::OcGraphData& g) {
    std::vector<std::string> lines;
    for (const fmt::OcGraphNode& n : g.nodes) {
        std::string s = "N " + n.id + " " + n.type;
        for (const std::string& t : n.extraTokens) s += " " + t;
        lines.push_back(std::move(s));
    }
    for (const fmt::OcGraphLink& l : g.links)
        lines.push_back("L " + l.sourceNode + "." + l.sourcePin + ">" + l.destNode + "." + l.destPin);
    std::sort(lines.begin(), lines.end());
    u64 h = kFnv1a64OffsetBasis;
    for (const std::string& s : lines) h = mix64(h, s);
    return h;
}

// ---- Aver.Pcg.Pcg.scatter ---------------------------------------------------------------------------------------------

PcgPoints pcgScatterReference(const PcgScatterSpec& s) {
    PcgPoints out;
    if (s.count <= 0) return out;
    if (s.assetCount > 0) out.iAttr["asset"];
    const Vec3 size{s.region.max.x - s.region.min.x, s.region.max.y - s.region.min.y, s.region.max.z - s.region.min.z};
    const f32 minSq = s.minSpacing * s.minSpacing;
    PcgSpatialGrid grid(s.minSpacing > 0.0f ? s.minSpacing : 100.0f);
    for (i32 i = 0; i < s.count; ++i) {
        // Four independent streams per placement: distinct index spaces, not a running counter.
        const f32 px = pcgFloat01(pcgHash2(s.seed, i * 4));
        const f32 py = pcgFloat01(pcgHash2(s.seed, i * 4 + 1));
        const f32 pz = pcgFloat01(pcgHash2(s.seed, i * 4 + 2));
        const u32 extra = pcgHash2(s.seed, i * 4 + 3);
        const Vec3 pos{s.region.min.x + size.x * px, s.region.min.y + size.y * py, s.region.min.z + size.z * pz};

        bool far = true;
        if (s.minSpacing > 0.0f)
            grid.forEachNear(pos, s.minSpacing, [&](u32 j) {
                const Vec3 d = out.pos[j] - pos;
                if (d.x * d.x + d.y * d.y + d.z * d.z < minSq) far = false;
            });
        if (!far) continue;

        PcgPoint p;
        p.pos = pos;
        p.seed = static_cast<i32>(extra);
        if (s.randomYaw) {
            const f32 h = (pcgFloat01(extra) * 6.2831853f) * 0.5f;
            p.rot = Quat{0.0f, 0.0f, static_cast<f32>(std::sin(static_cast<f64>(h))), static_cast<f32>(std::cos(static_cast<f64>(h)))};
        }
        const f32 sc = s.scaleMax <= s.scaleMin ? s.scaleMin : s.scaleMin + (s.scaleMax - s.scaleMin) * pcgFloat01(pcgHash(extra));
        p.scale = Vec3{sc, sc, sc};
        out.push(p);
        if (s.assetCount > 0) out.iAttr["asset"].back() = static_cast<i32>(pcgHash(extra ^ 0x5bd1u) % static_cast<u32>(s.assetCount));
        if (s.minSpacing > 0.0f) grid.insert(pos, static_cast<u32>(out.size() - 1));
    }
    return out;
}

} // namespace aver::world
