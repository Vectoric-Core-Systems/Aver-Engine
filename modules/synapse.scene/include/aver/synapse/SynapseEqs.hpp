#pragma once
// The scene join for environment queries: a query cache, a budgeted queue of EqsRuns, and the
// EqsEnv that answers contexts, actors, physics and slots from a live world. A member of SynapseAi
// (the Synapse DLL's single instance), reachable from C# through synapse_query_abi.h and from behaviour
// trees through the RunEqsQuery / EqsHasResult leaves SynapseAi::registerBehaviors installs.
#include "aver/scene/World.hpp"
#include "aver/synapse/Eqs.hpp"

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace aver::synapse {

class SynapseAi;

// Answers `class=` in the actors generator: writes up to `cap` entity handles whose class is `className`
// (or derives from it when includeDerived) and returns how many. Installed by the host.
using ActorProviderFn = i32 (*)(const char* className, i32 includeDerived, i32* out, i32 cap, void* user);

// What the debug overlay draws for one finished run (survivors only, plus how many were generated).
struct EqsDebugRun {
    std::string query;
    u32 querier = 0;
    Vec3 origin;
    std::vector<EqsItem> items;
    std::vector<std::string> testNames;
    std::vector<f32> bestValues;   // the per-test values of items[0]
    u32 generated = 0;
    f32 age = 0;                   // seconds since it finished
    bool pinned = false;           // the editor's preview: stays on screen until replaced
    bool failed = false;
    std::string reason;
};

class EqsSystem {
public:
    enum class Status : u8 { Unknown, Queued, Running, Finished, Failed, Cancelled };

    void bind(SynapseAi* ai) { ai_ = ai; }
    void setActorProvider(ActorProviderFn fn, void* user) { actors_ = fn; actorsUser_ = user; }

    // Query cache keyed by the path as written (content-relative). 0 on failure (logged once).
    u32 loadQuery(const std::string& path);
    u32 registerQuery(const std::string& name, const EqsQuery& q);
    bool reloadQuery(const std::string& path);   // runs already started keep their copy
    const EqsQuery* query(u32 id) const;
    const std::string& lastMessage() const { return lastMessage_; }

    // Run handle (nonzero), 0 when refused (unknown query, 64 runs already waiting).
    u32 start(u32 queryId, const EqsRunParams& params, i32 priority = 0);
    u32 runSync(u32 queryId, const EqsRunParams& params);   // unlimited budget, finished before return
    Status status(u32 handle) const;
    const EqsResult* result(u32 handle) const;   // finished runs only
    u32 queryIdOf(u32 handle) const;
    bool cancel(u32 handle);
    void release(u32 handle);

    // maxMs <= 0 disables the time limit, maxUnits 0 the unit limit. Defaults 1 ms and 0.
    void setBudget(f32 maxMs, u32 maxUnitsPerTick) { maxMs_ = maxMs; maxUnits_ = maxUnitsPerTick; }

    void tick(scene::World& world, const fmt::OcNavData* nav, f32 dt, SynapseAi& ai);
    void reset();   // drops runs and results, keeps the query cache
    // Floating-origin shift (LARGE_WORLD 2.2.5): finished results and the debug runs move with the scene (positions -= delta). A
    // queued or running run is cancelled instead: its remaining generation and tests would mix two frames.
    void rebase(const Vec3& delta);
    usize pending() const;
    const std::vector<EqsDebugRun>& debugRuns() const { return debug_; }
    // Keeps the newest finished run on screen (the editor's "Run on selection"); unpins every other.
    void pinNewestDebugRun() {
        for (usize i = 0; i < debug_.size(); ++i) debug_[i].pinned = i == 0;
    }
    double time() const { return time_; }

    // ---- behaviour-tree leaf support (SynapseAi.cpp) ----
    struct LastResult { double time = 0; u32 count = 0; };
    const LastResult* lastResult(i32 subject, u32 queryId) const;
    void noteResult(i32 subject, u32 queryId, u32 count) { last_[{subject, queryId}] = LastResult{time_, count}; }
    u32& leafRun(i32 subject, const void* node) { return leafRuns_[{subject, node}]; }
    void dropLeafRun(i32 subject, const void* node) { leafRuns_.erase({subject, node}); }

private:
    struct QueryRec {
        EqsQuery q;
        std::string path;
    };
    struct EnvCtx {
        EqsSystem* sys = nullptr;
        std::vector<std::pair<std::string, std::string>> contexts;
    };
    struct RunRec {
        u32 handle = 0, queryId = 0;
        i32 priority = 0;
        Status status = Status::Queued;
        std::string queryName;
        std::unique_ptr<EnvCtx> env;
        std::unique_ptr<EqsRun> run;
        double finishedAt = 0;
        bool announced = false;
    };

    EqsEnv makeEnv(EnvCtx* e) const;
    RunRec* find(u32 handle);
    const RunRec* find(u32 handle) const;
    u32 create(u32 queryId, const EqsRunParams& params, i32 priority);
    void finishRun(RunRec& r);

    static EqsContext envContext(void* user, std::string_view name, const EqsRunParams& p);
    static bool envForward(void* user, u32 entity, Vec3& out);
    static u32 envActors(void* user, const EqsGenerator& g, const Vec3& centre, u32 querier, u32* out, u32 cap);
    static u32 envSlots(void* user, const EqsGenerator& g, const Vec3& centre, const EqsRunParams& p, EqsItem* out, TagList* tags, u32 cap);
    static bool envEntityPos(void* user, u32 entity, Vec3& out);
    static u32 envTagBits(void* user, u32 entity);
    static bool envTrace(void* user, const Vec3& a, const Vec3& b, f32 radius, u32 ignoreA, u32 ignoreB);
    static bool envGround(void* user, const Vec3& p, f32 up, f32 down, f32& z);
    static bool envCustom(void* user, const EqsTest& t, const EqsRunParams& p, const EqsItem& item, const EqsContext& ctx, f32& value);

    SynapseAi* ai_ = nullptr;
    ActorProviderFn actors_ = nullptr;
    void* actorsUser_ = nullptr;
    const fmt::OcNavData* nav_ = nullptr;
    std::map<u32, QueryRec> queries_;
    std::unordered_map<std::string, u32> byPath_;
    std::map<std::string, bool> failedPaths_;
    std::string lastMessage_;
    u32 nextQuery_ = 1;
    std::map<u32, RunRec> runs_;
    u32 nextHandle_ = 1;
    f32 maxMs_ = 1.0f;
    u32 maxUnits_ = 0;
    double time_ = 0;
    std::vector<EqsDebugRun> debug_;
    std::map<std::pair<i32, u32>, LastResult> last_;
    std::map<std::pair<i32, const void*>, u32> leafRuns_;
};

} // namespace aver::synapse
