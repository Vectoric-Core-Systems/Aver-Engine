#pragma once
// World Settings > Level Streaming > Build HLOD (WAVE2_UE W2 step 5, editor half): the pure pieces. Whether a level can be baked and why not,
// the one-line summary of a bake, and the background job that runs it, so the editor never stalls on a big level. The bake itself is
// fmt::bakeHlod (Formats); the job only supplies a thread, a cancel flag and progress counters. No ImGui, no SandboxApp.
#include "aver/core/Types.hpp"
#include "aver/formats/OcHlodBake.hpp"
#include "aver/formats/OcWorld.hpp"

#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace aver::editor {

// Empty when `s` can be baked; otherwise what to do first.
inline std::string hlodBlockedReason(const fmt::OcStreamSettings& s, bool levelOpen) {
    if (!levelOpen) return "Open a level first.";
    if (!s.enabled) return "Turn on \"Stream this level\" first: HLOD proxies are per streaming cell.";
    if (s.cellCm <= 0.0f) return "The cell size is not set.";
    return {};
}

// "12 cells: 9 built, 3 up to date; 4210 -> 980 triangles" plus anything that went wrong.
inline std::string hlodSummary(const fmt::OcHlodReport& r) {
    std::string t = std::to_string(r.cells) + (r.cells == 1 ? " cell: " : " cells: ") + std::to_string(r.built) + " built, " + std::to_string(r.fresh) + " up to date";
    if (r.removed) t += ", " + std::to_string(r.removed) + " removed";
    if (r.failed) t += ", " + std::to_string(r.failed) + " failed";
    t += "; " + std::to_string(r.trianglesIn) + " -> " + std::to_string(r.trianglesOut) + " triangles";
    if (r.excludedByLayer) t += "; " + std::to_string(r.excludedByLayer) + " left out by data layers";
    if (r.cancelled) t += " (cancelled; the finished cells are kept)";
    return t;
}

// The default HLOD distance when the level has none: three load distances, as the bake itself picks.
inline f32 defaultHlodCm(const fmt::OcStreamSettings& s) { return s.hlodCm > 0.0f ? s.hlodCm : s.loadCm * 3.0f; }

struct HlodJobResult {
    bool ok = false;
    std::string why;
    fmt::OcHlodReport report;
    f32 hlodCm = 0.0f;   // the distance the bake used, for the level's STREAM record
};

// One bake at a time on its own thread. `work` runs off the main thread and must poll `cancel` (the bake's own cancel option) and may report
// progress. poll() hands back the result once, on the caller's thread. Destroying a running job cancels it and waits.
class HlodBuildJob {
public:
    using Work = std::function<HlodJobResult(const std::atomic<bool>& cancel, std::atomic<u32>& done, std::atomic<u32>& total)>;

    ~HlodBuildJob() { cancelAndJoin(); }

    bool running() const { return running_.load(); }
    // False while a bake is already running.
    bool start(Work work) {
        if (running_.load()) return false;
        if (thread_.joinable()) thread_.join();
        cancel_ = false; done_ = 0; total_ = 0; finished_ = false;
        result_ = {};
        running_ = true;
        thread_ = std::thread([this, w = std::move(work)]() {
            HlodJobResult r;
            try { r = w(cancel_, done_, total_); }
            catch (const std::exception& e) { r.ok = false; r.why = e.what(); }
            catch (...) { r.ok = false; r.why = "the bake threw"; }
            { std::lock_guard<std::mutex> lock(mutex_); result_ = std::move(r); }
            finished_ = true;
            running_ = false;
        });
        return true;
    }
    void cancel() { cancel_ = true; }
    u32 done() const { return done_.load(); }
    u32 total() const { return total_.load(); }
    f32 fraction() const { const u32 t = total_.load(); return t ? static_cast<f32>(done_.load()) / static_cast<f32>(t) : 0.0f; }
    // True exactly once per bake, with its result.
    bool poll(HlodJobResult& out) {
        if (!finished_.load()) return false;
        if (thread_.joinable()) thread_.join();
        finished_ = false;
        std::lock_guard<std::mutex> lock(mutex_);
        out = std::move(result_);
        return true;
    }
    void cancelAndJoin() {
        cancel_ = true;
        if (thread_.joinable()) thread_.join();
    }

private:
    std::thread thread_;
    std::atomic<bool> cancel_{false}, running_{false}, finished_{false};
    std::atomic<u32> done_{0}, total_{0};
    std::mutex mutex_;
    HlodJobResult result_;
};

}  // namespace aver::editor
