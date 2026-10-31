// SubViewport registry (R5, https://github.com/Vectoric-Core-Systems/Fabric-Connector/blob/main/docs/plans/G4_RENDER.md): the render-to-texture views a level asks for, their update-mode
// scheduling, the per-frame render budget and the texture-handle bookkeeping. Header-only and pure -- no device, no scene -- so the
// framework ABI, the hosts and the tests share one set of rules. Nothing is drawn here.
//
// A view exists only because a script or a converted level created it (aver_fw_subview_create). With no view the hosts do nothing.
#pragma once
#include "aver/core/Types.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace aver {

// The update-mode / clear-mode numbers, so an imported project stores them verbatim.
enum class ViewUpdate : i32 { Disabled = 0, Once = 1, WhenVisible = 2, WhenParentVisible = 3, Always = 4 };
enum class ViewClear : i32 { Always = 0, Never = 1, Once = 2 };

inline ViewUpdate viewUpdateFromInt(i32 v) { return v < 0 || v > 4 ? ViewUpdate::WhenVisible : static_cast<ViewUpdate>(v); }
inline ViewClear viewClearFromInt(i32 v) { return v < 0 || v > 2 ? ViewClear::Always : static_cast<ViewClear>(v); }

inline constexpr u32 kSubViewAbsoluteMax = 16;     // the table size; the budget below is what normally limits
inline constexpr u32 kSubViewMinSide = 16;
inline constexpr u32 kSubViewMaxSide = 4096;
inline constexpr u32 kSubViewKeyMax = 120;

// The cost guard. `maxViews` 0 turns the feature off (every create is refused).
struct SubViewBudget {
    u32 maxViews = 4;            // live views at once (RENDER.SUBVIEWPORTS)
    u32 rendersPerFrame = 2;     // views drawn per frame; the rest wait their turn (RENDER.SUBVIEWRENDERS)
    u64 maxTotalPixels = 8u * 1024u * 1024u;   // summed over live views; larger sets are scaled down together (RENDER.SUBVIEWPIXELS, megapixels)
};

struct SubViewDesc {
    u32 width = 512, height = 512;          // the node's Size, pixels
    ViewUpdate update = ViewUpdate::WhenVisible;
    ViewClear clear = ViewClear::Always;
    bool transparent = false;               // transparent_bg: the clear colour has alpha 0
    bool ownWorld3d = false;                // own_world_3d: carried; the engine has one world, so it is reported, not honoured
    i32 cameraEntity = 0;                   // the entity whose CCamera and world matrix drive the view; 0 = none yet
    f32 scale3d = 1.0f;                     // scaling_3d_scale: multiplies the rendered size
    i32 msaa = 0;                           // msaa_3d: carried, not used (the view is drawn at one sample)
    bool parentVisible = true;              // WhenParentVisible reads this
    f32 clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};   // display values; alpha is forced to 0 when `transparent`
};

class SubViewRegistry {
public:
    struct Entry {
        u32 id = 0;
        std::string key;
        SubViewDesc desc;
        u32 effW = 0, effH = 0;          // the size actually rendered: scaled and fitted to the pixel budget
        u32 materialConsumers = 0;       // materials naming the view's texture
        u32 uiConsumers = 0;             // UI that shows it
        bool pendingRender = false;      // a script asked for one render now
        u64 lastRenderFrame = 0;
        u64 renderCount = 0;
        u64 texture = 0;                 // the host's texture handle (an rhi::TextureHandle), 0 until it has one
        u32 texW = 0, texH = 0;
        u32 generation = 0;              // bumps when `texture` changes, so a consumer rebinds
        bool onScreen = false;           // drawn over the window (SubViewportContainer)
        f32 screen[4] = {0, 0, 0, 0};    // x, y, w, h in window pixels
    };

    struct Job {
        u32 id = 0;
        bool clear = true;
    };

    explicit SubViewRegistry(const SubViewBudget& b = {}) : budget_(b) {}

    const SubViewBudget& budget() const { return budget_; }
    void setBudget(const SubViewBudget& b) {
        budget_ = b;
        budget_.maxViews = std::min(budget_.maxViews, kSubViewAbsoluteMax);
        budget_.rendersPerFrame = std::max<u32>(1, std::min(budget_.rendersPerFrame, kSubViewAbsoluteMax));
        budget_.maxTotalPixels = std::max<u64>(budget_.maxTotalPixels, 64ull * 64ull);
        fit();
    }

    u32 count() const { return static_cast<u32>(entries_.size()); }
    u64 refused() const { return refused_; }

    // Creates a view, or returns the one already made under `key` (a level reload makes the same node again). 0 = refused: the feature is off,
    // the budget is full, or the key is empty or too long. The first creation fixes the key's id for as long as the view lives.
    u32 create(const std::string& key, const SubViewDesc& d) {
        if (key.empty() || key.size() > kSubViewKeyMax) { ++refused_; return 0; }
        if (Entry* e = find(key)) { e->desc = sanitise(d); fit(); return e->id; }
        if (entries_.size() >= budget_.maxViews || entries_.size() >= kSubViewAbsoluteMax) { ++refused_; return 0; }
        Entry e;
        e.id = nextId_++;
        e.key = key;
        e.desc = sanitise(d);
        entries_.push_back(std::move(e));
        fit();
        return entries_.back().id;
    }

    bool destroy(u32 id) {
        for (usize i = 0; i < entries_.size(); ++i)
            if (entries_[i].id == id) { entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i)); fit(); return true; }
        return false;
    }

    void clear() { entries_.clear(); }   // a session boundary; ids keep counting so a stale id never names a new view

    Entry* get(u32 id) {
        for (Entry& e : entries_) if (e.id == id) return &e;
        return nullptr;
    }
    const Entry* get(u32 id) const { return const_cast<SubViewRegistry*>(this)->get(id); }
    Entry* find(const std::string& key) {
        for (Entry& e : entries_) if (e.key == key) return &e;
        return nullptr;
    }
    const Entry* at(u32 index) const { return index < entries_.size() ? &entries_[index] : nullptr; }

    // Replaces the description (size, modes, camera...). The scheduling state (render counts, consumers, texture) is kept.
    bool update(u32 id, const SubViewDesc& d) {
        Entry* e = get(id);
        if (!e) return false;
        e->desc = sanitise(d);
        fit();
        return true;
    }

    // ---- sizes ----

    static u32 clampSide(f64 v) {
        if (!(v >= static_cast<f64>(kSubViewMinSide))) return kSubViewMinSide;
        if (v > static_cast<f64>(kSubViewMaxSide)) return kSubViewMaxSide;
        return static_cast<u32>(std::lround(v));
    }

    // Size times the 3D scale, clamped to the side limits. The pixel budget is applied on top, in fit().
    static void scaledSize(const SubViewDesc& d, u32& w, u32& h) {
        const f64 s = d.scale3d > 0.0f ? static_cast<f64>(d.scale3d) : 1.0;
        w = clampSide(static_cast<f64>(d.width) * s);
        h = clampSide(static_cast<f64>(d.height) * s);
    }

    // Recomputes every effective size: each view's scaled size, and when their sum is over the pixel cap all of them shrink by the same linear
    // factor (aspect kept, floor of the minimum side) until the sum fits or every view is at the floor.
    void fit() {
        u64 total = 0;
        for (Entry& e : entries_) {
            scaledSize(e.desc, e.effW, e.effH);
            total += static_cast<u64>(e.effW) * e.effH;
        }
        for (int pass = 0; pass < 6 && total > budget_.maxTotalPixels; ++pass) {
            const f64 k = std::sqrt(static_cast<f64>(budget_.maxTotalPixels) / static_cast<f64>(total)) * 0.999;
            total = 0;
            for (Entry& e : entries_) {
                e.effW = clampSide(static_cast<f64>(e.effW) * k);
                e.effH = clampSide(static_cast<f64>(e.effH) * k);
                total += static_cast<u64>(e.effW) * e.effH;
            }
        }
    }

    u64 totalPixels() const {
        u64 t = 0;
        for (const Entry& e : entries_) t += static_cast<u64>(e.effW) * e.effH;
        return t;
    }

    // ---- consumers ----

    // kind 0 = a material names the texture, 1 = the UI shows it. delta +1 / -1; never goes below zero.
    bool consumer(u32 id, i32 kind, i32 delta) {
        Entry* e = get(id);
        if (!e || (kind != 0 && kind != 1)) return false;
        u32& c = kind == 0 ? e->materialConsumers : e->uiConsumers;
        if (delta >= 0) c += static_cast<u32>(delta);
        else c = c > static_cast<u32>(-delta) ? c - static_cast<u32>(-delta) : 0u;
        return true;
    }
    static bool visible(const Entry& e) { return e.materialConsumers + e.uiConsumers > 0 || e.onScreen; }

    // Which colour encoding the target holds. A view only materials read stores LINEAR values (a material samples its albedo as linear and the
    // frame's own tonemap and gamma follow); one the UI shows, or both, stores display values (the UI draws the pixels as they are).
    static bool linearOutput(const Entry& e) { return e.materialConsumers > 0 && e.uiConsumers == 0 && !e.onScreen; }

    // ---- scheduling ----

    // Whether the update mode asks for a render this frame (before the budget). A view with no camera is never due: a Once view must not use its
    // one render on nothing.
    static bool due(const Entry& e) {
        if (e.desc.cameraEntity == 0) return false;
        if (e.pendingRender) return true;
        switch (e.desc.update) {
            case ViewUpdate::Disabled: return false;
            case ViewUpdate::Once: return e.renderCount == 0;
            case ViewUpdate::WhenVisible: return visible(e);
            case ViewUpdate::WhenParentVisible: return e.desc.parentVisible;
            case ViewUpdate::Always: return true;
        }
        return false;
    }

    // The views to draw this frame, at most `rendersPerFrame` and at most `max`. Order: views that have never been drawn, a requested render and
    // a Once view first (they are waiting for their first picture), then the least recently drawn; ties by id. Consumes the Once state (the view
    // becomes Disabled) and the pending request, and stamps the frame. `frame` must not go backwards.
    u32 plan(u64 frame, Job* out, u32 max) {
        std::vector<Entry*> due_;
        due_.reserve(entries_.size());
        for (Entry& e : entries_) if (due(e)) due_.push_back(&e);
        std::stable_sort(due_.begin(), due_.end(), [](const Entry* a, const Entry* b) {
            const bool fa = a->renderCount == 0 || a->pendingRender, fb = b->renderCount == 0 || b->pendingRender;
            if (fa != fb) return fa;
            if (a->lastRenderFrame != b->lastRenderFrame) return a->lastRenderFrame < b->lastRenderFrame;
            return a->id < b->id;
        });
        const u32 cap = std::min(budget_.rendersPerFrame, max);
        u32 n = 0;
        for (Entry* e : due_) {
            if (n >= cap) { ++deferred_; continue; }
            Job j;
            j.id = e->id;
            // The first picture always clears (there is nothing to keep); later ones follow the clear mode.
            j.clear = e->renderCount == 0 || e->desc.clear == ViewClear::Always;
            out[n++] = j;
            e->lastRenderFrame = frame;
            ++e->renderCount;
            e->pendingRender = false;
            if (e->desc.update == ViewUpdate::Once) e->desc.update = ViewUpdate::Disabled;
        }
        return n;
    }
    u64 deferred() const { return deferred_; }   // renders postponed by the per-frame budget, ever

    // ---- texture bookkeeping ----

    // The host's texture for a view. A new handle (or size) bumps `generation`; a consumer holding the old one must rebind.
    bool setTexture(u32 id, u64 handle, u32 w, u32 h) {
        Entry* e = get(id);
        if (!e) return false;
        if (e->texture != handle || e->texW != w || e->texH != h) ++e->generation;
        e->texture = handle;
        e->texW = w;
        e->texH = h;
        return true;
    }

    // The name a material's texture slot uses to reach a view: MaterialSystem resolves "@rt:<name>" before any host resolver.
    static std::string runtimeTextureName(const std::string& key) { return "viewport." + key; }
    static std::string runtimeTexturePath(const std::string& key) { return "@rt:viewport." + key; }

private:
    static SubViewDesc sanitise(SubViewDesc d) {
        d.width = std::max<u32>(1, std::min<u32>(d.width, 16384));
        d.height = std::max<u32>(1, std::min<u32>(d.height, 16384));
        if (!(d.scale3d > 0.0f) || !std::isfinite(d.scale3d)) d.scale3d = 1.0f;
        d.scale3d = std::min(d.scale3d, 8.0f);
        for (int i = 0; i < 4; ++i) if (!std::isfinite(d.clearColor[i])) d.clearColor[i] = i == 3 ? 1.0f : 0.0f;
        return d;
    }

    SubViewBudget budget_;
    std::vector<Entry> entries_;
    u32 nextId_ = 1;
    u64 refused_ = 0;
    u64 deferred_ = 0;
};

} // namespace aver
