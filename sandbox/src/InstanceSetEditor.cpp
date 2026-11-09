// The `.ocinst` instance-set editor. See InstanceSetEditor.hpp for the three layers.
#include "InstanceSetEditor.hpp"
#include "EditorChrome.hpp"

#include "EditorTransform.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_set>
#include <vector>

#if AVER_WITH_IMGUI
#  include "ActorEditor.hpp"       // actorEditorContentRoot
#  include "EditorKeybinds.hpp"
#  include "EditorWidgets.hpp"     // SplitPane
#  include "PreviewChrome.hpp"     // toolbar, stats, axes, formatCount
#  include "PreviewGizmo.hpp"
#  include "aver/render/preview/ActorPreview.hpp"
#  include "aver/render/preview/PreviewMeshCache.hpp"
#  include "aver/rhi/RHI.hpp"
#  include "aver/runtime/Engine.hpp"
#  include "imgui.h"
#  include "imgui_internal.h"      // AddContextHook: the per-frame tick for deferred GPU release
#endif

namespace aver::editor {

// ============================================================================ maths (ImGui-free)

namespace instset {

void decompose(const f32 m[12], Trs& out) {
    f32 u[3][3];
    for (int r = 0; r < 3; ++r) {
        const f32 x = m[r*3+0], y = m[r*3+1], z = m[r*3+2];
        const f32 len = std::sqrt(x*x + y*y + z*z);
        out.scale[r] = len;
        for (int c = 0; c < 3; ++c) u[r][c] = (len > 1e-12f) ? m[r*3+c] / len : (r == c ? 1.0f : 0.0f);
    }
    for (int c = 0; c < 3; ++c) out.pos[c] = m[9 + c];

    // composeEditorTransform's rows: row0 = (cy*cp, sy*cp, -sp); row1 z = cp*sr; row2 z = cp*cr.
    const f32 sp = std::clamp(-u[0][2], -1.0f, 1.0f);
    const f32 pitch = std::asin(sp);
    f32 yaw, roll;
    if (std::fabs(std::cos(pitch)) > 1e-4f) {
        yaw  = std::atan2(u[0][1], u[0][0]);
        roll = std::atan2(u[1][2], u[2][2]);
    } else {   // straight up/down: only yaw - roll is observable; park it in yaw
        roll = 0.0f;
        yaw  = std::atan2(-u[1][0], u[1][1]);
    }
    const f32 deg = 180.0f / kPi;
    out.rotDeg[0] = yaw * deg;
    out.rotDeg[1] = pitch * deg;
    out.rotDeg[2] = roll * deg;
}

void compose(const Trs& t, f32 out12[12]) {
    f32 m16[16];
    composeEditorTransform(t.pos, t.rotDeg, t.scale, m16);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out12[r*3+c] = m16[r*4+c];
    for (int c = 0; c < 3; ++c) out12[9 + c] = m16[12 + c];
}

bool invert4(const f32 m[16], f32 out[16]) {
    f32 inv[16];
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];
    const f32 det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (!(std::fabs(det) > 1e-30f)) return false;
    const f32 s = 1.0f / det;
    for (int i = 0; i < 16; ++i) out[i] = inv[i] * s;
    return true;
}

bool viewRay(const f32 vp[16], f32 ndcX, f32 ndcY, const f32 eye[3], f32 dir[3]) {
    f32 inv[16];
    if (!invert4(vp, inv)) return false;
    const f32 v[4] = {ndcX, ndcY, 1.0f, 1.0f};   // far plane; any depth lies on the same ray
    f32 p[4];
    for (int c = 0; c < 4; ++c) p[c] = v[0]*inv[0*4+c] + v[1]*inv[1*4+c] + v[2]*inv[2*4+c] + v[3]*inv[3*4+c];
    if (!(std::fabs(p[3]) > 1e-20f)) return false;
    f32 d[3];
    for (int c = 0; c < 3; ++c) d[c] = p[c] / p[3] - eye[c];
    const f32 len = std::sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    if (!(len > 1e-12f)) return false;
    for (int c = 0; c < 3; ++c) dir[c] = d[c] / len;
    return true;
}

bool rayPlaneZ(const f32 o[3], const f32 d[3], f32 planeZ, f32 out[3]) {
    if (std::fabs(d[2]) < 1e-6f) return false;
    const f32 t = (planeZ - o[2]) / d[2];
    if (t <= 0.0f) return false;
    for (int c = 0; c < 3; ++c) out[c] = o[c] + d[c] * t;
    return true;
}

} // namespace instset

// ============================================================================ the document

InstanceSetDoc::Stamp InstanceSetDoc::stampOf(const std::string& path) {
    Stamp s;
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) return s;
    const auto t = std::filesystem::last_write_time(path, ec);
    if (ec) return s;
    s.size = size;
    s.mtime = static_cast<long long>(t.time_since_epoch().count());
    s.valid = true;
    return s;
}

bool InstanceSetDoc::load(const std::string& path, std::string* why) {
    fmt::OcInstanceData data;
    if (!fmt::loadOcInstances(path, data, why)) return false;   // a failed (re)load leaves the old state
    if (!model_.load(data, why)) { loaded_ = false; return false; }
    // [wave3/level-instances] a baked level pack (Binaries/LevelPacks) is read-only: Bake Pack regenerates it
    {
        std::string norm = path;
        for (char& c : norm) c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        model_.setReadOnly(norm.find("/levelpacks/") != std::string::npos);
    }
    path_ = path;
    loaded_ = true;
    stamp_ = stampOf(path_);
    haveBounds_ = false;
    recomputeBounds();
    return true;
}

bool InstanceSetDoc::save(std::string* why) {
    if (!loaded_) { if (why) *why = "nothing is loaded"; return false; }
    fmt::OcInstanceData out;
    model_.exportTo(out);
    if (!fmt::saveOcInstances(path_, out, why)) return false;
    model_.markSaved();
    stamp_ = stampOf(path_);
    return true;
}

bool InstanceSetDoc::diskMatchesLastIo() const {
    const Stamp now = stampOf(path_);
    return now.valid && stamp_.valid && now.size == stamp_.size && now.mtime == stamp_.mtime;
}

void InstanceSetDoc::recomputeBounds() {
    haveBounds_ = false;
    for (u32 g = 0; g < model_.groupCount(); ++g) {
        const u32 rows = model_.groupRows(g);
        for (u32 r = 0; r < rows; ++r) {
            f32 lo[3], hi[3];
            if (!model_.worldBounds(makeInstanceId(g, r), lo, hi)) continue;
            if (!haveBounds_) {
                for (int c = 0; c < 3; ++c) { lo_[c] = lo[c]; hi_[c] = hi[c]; }
                haveBounds_ = true;
            } else {
                for (int c = 0; c < 3; ++c) { lo_[c] = std::min(lo_[c], lo[c]); hi_[c] = std::max(hi_[c], hi[c]); }
            }
        }
    }
}

void InstanceSetDoc::growBounds() {
    f32 lo[3], hi[3];
    if (!model_.selectionBounds(lo, hi)) return;
    if (!haveBounds_) {
        for (int c = 0; c < 3; ++c) { lo_[c] = lo[c]; hi_[c] = hi[c]; }
        haveBounds_ = true;
        return;
    }
    for (int c = 0; c < 3; ++c) { lo_[c] = std::min(lo_[c], lo[c]); hi_[c] = std::max(hi_[c], hi[c]); }
}

// ============================================================================ the tab

namespace {

InstanceSetEditorHooks g_hooks;

constexpr u32 kDrawCap = 150000;            // instances submitted per frame
constexpr usize kMaxMeshes = 128;           // distinct meshes this tab will load
constexpr int kMeshLoadsPerFrame = 3;       // a mesh load is synchronous; spread them out
constexpr usize kMaxSingleDraws = 256;      // selected instances drawn one by one; the rest tint a batch
constexpr f32 kMinPixels = 1.5f;            // pixel cull
constexpr f32 kProxyHalf = 50.0f;           // box proxy half-extent, cm (the model's default bounds)

[[maybe_unused]] std::string lowered(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

#if AVER_WITH_IMGUI

constexpr f32 kDefaultViewFraction = 0.72f;
constexpr const char* kPrefViewSplit = "instanceSetEditor.viewSplit.v1";

// Everything GPU the tab owns. Moved wholesale to the graveyard when the tab closes.
struct Gpu {
    rhi::IDevice* dev = nullptr;
    render::preview::ActorPreview* preview = nullptr;
    render::preview::PreviewMeshCache meshes;   // its OWN cache, not the shared one
    rhi::MeshHandle proxyBox = 0;
};

void destroyGpu(Gpu& g) {
    if (g.dev && g.preview) g.dev->removeRenderFeature(g.preview);
    delete g.preview;   // waits for the GPU, so the meshes below are no longer in flight
    g.preview = nullptr;
    if (g.dev) g.meshes.clear(*g.dev);
}

class InstanceSetEditor;
std::vector<std::unique_ptr<Gpu>> g_graveyard;     // closed tabs' GPU state, freed one frame later
std::vector<InstanceSetEditor*> g_live;
ImGuiContext* g_hookCtx = nullptr;
bool g_shutDown = false;

void hidePreview(InstanceSetEditor& ed);

// NewFrame, before anything of the new frame is built: free what a tab closed last frame (its image
// was still on that frame's draw list, so it could not go sooner), and switch every preview off --
// a tab that is drawn this frame switches its own back on, a hidden one stays off.
void tickHook(ImGuiContext*, ImGuiContextHook*);

void ensureHook() {
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx || g_hookCtx == ctx) return;
    ImGuiContextHook h;
    h.Type = ImGuiContextHookType_NewFramePre;
    h.Callback = &tickHook;
    ImGui::AddContextHook(ctx, &h);
    g_hookCtx = ctx;
}

[[maybe_unused]] void camEye(const render::preview::PreviewCamera& c, f32 eye[3]) {
    const f32 d = kPi / 180.0f;
    const f32 cy = std::cos(c.yawDeg * d), sy = std::sin(c.yawDeg * d);
    const f32 cp = std::cos(c.pitchDeg * d), sp = std::sin(c.pitchDeg * d);
    eye[0] = c.pivot[0] - c.distance * cp * cy;
    eye[1] = c.pivot[1] - c.distance * cp * sy;
    eye[2] = c.pivot[2] + c.distance * sp;
}

#endif // AVER_WITH_IMGUI

class InstanceSetEditor final : public AssetEditor {
public:
    explicit InstanceSetEditor(const std::string& path) : path_(path) {
#if AVER_WITH_IMGUI
        g_live.push_back(this);
#endif
        doc_.load(path_, &loadError_);
    }

    ~InstanceSetEditor() override {
#if AVER_WITH_IMGUI
        g_live.erase(std::remove(g_live.begin(), g_live.end(), this), g_live.end());
        if (gpu_ && !g_shutDown) {
            if (gpu_->preview) gpu_->preview->setEnabled(false);
            g_graveyard.push_back(std::move(gpu_));   // not before the frame that drew it has been sampled
            ensureHook();
        }
#endif
    }

    const std::string& path() const override { return path_; }
    std::string title() const override {
        return std::filesystem::path(path_).filename().string() + "  [Instances]";
    }
    bool dirty() const override { return doc_.loaded() && doc_.model().dirty(); }

    bool save(std::string* why) override {
        if (!doc_.loaded()) { if (why) *why = "cannot save: the file failed to load"; return false; }
        if (!doc_.save(why)) return false;
        staleNotice_ = true;
        regenReport_.clear();
        externalChange_ = false;
        AVER_INFO("[InstanceSet] wrote {} instance(s) to {}", doc_.model().aliveCount(), path_);
        return true;
    }

    void onFileChanged() override {
        if (!doc_.loaded()) { reloadFromDisk(); return; }
        if (doc_.diskMatchesLastIo()) return;   // the echo of our own save
        if (doc_.model().dirty()) { externalChange_ = true; return; }   // never discard edits on our own
        reloadFromDisk();
    }

#if AVER_WITH_IMGUI
    void resetLayout() override { resetSplitPane(split_, kPrefViewSplit, kDefaultViewFraction); }

    void hideForFrame() { if (gpu_ && gpu_->preview) gpu_->preview->setEnabled(false); }
    void releaseGpuNow() {
        if (!gpu_) return;
        destroyGpu(*gpu_);
        gpu_.reset();
    }
#endif

    void draw(Engine& e) override {
#if AVER_WITH_IMGUI
        if (!doc_.loaded()) {
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "This file could not be read.");
            ImGui::Separator();
            ImGui::TextWrapped("%s", loadError_.c_str());
            return;
        }
        focused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        handleKeys();

        const f32 dpi = fontScale();
        const f32 avail = ImGui::GetContentRegionAvail().x;
        const f32 minView = 260.0f * dpi, minSide = 260.0f * dpi;
        const f32 viewW = splitPaneWidth(split_, kPrefViewSplit, kDefaultViewFraction, avail, minView, minSide);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        const bool viewOpen = ImGui::BeginChild("##instView", ImVec2(viewW, 0.0f), ImGuiChildFlags_None,
                                                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        if (viewOpen) drawViewport(e, dpi);
        ImGui::EndChild();
        drawSplitHandle(split_, "##instsplit", kPrefViewSplit, avail, minView, minSide, 6.0f * dpi);

        if (ImGui::BeginChild("##instSide", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders)) drawSidePanel(dpi);
        ImGui::EndChild();
#else
        (void)e;
#endif
    }

private:
    void reloadFromDisk() {
        std::string why;
        if (!doc_.load(path_, &why)) {
            AVER_WARN("[InstanceSet] could not reload '{}': {}", path_, why);
            if (!doc_.loaded()) loadError_ = why;
            return;
        }
        loadError_.clear();
        externalChange_ = false;
#if AVER_WITH_IMGUI
        if (leftState_ == Left::Gizmo) { gizmo::endDrag(drag_); }
        leftState_ = Left::None;
        fieldEditOpen_ = false;
        groupGpu_.clear();
        loadedAssets_.clear();
        currentGroup_ = -1;
        boundsDirty_ = true;
        touch();
#endif
    }

#if AVER_WITH_IMGUI
    enum class Left { None, Gizmo, Marquee };
    struct GroupGpu { rhi::MeshHandle mesh = 0; bool tried = false; };
    struct SelDraw { u32 group; f32 m[12]; };

    void touch() { ++epoch_; }
    InstanceSetModel& model() { return doc_.model(); }

    // ---- GPU ---------------------------------------------------------------------------------------
    bool ensureGpu(Engine& e, f32 w, f32 h) {
        if (gpu_) return true;
        if (gpuFailed_) return false;
        rhi::IDevice* dev = e.device();
        if (!dev) return false;
        auto g = std::make_unique<Gpu>();
        g->dev = dev;
        g->preview = render::preview::ActorPreview::create(*dev, static_cast<u32>(w), static_cast<u32>(h));
        if (!g->preview) {
            AVER_WARN("[InstanceSet] no preview on this backend; the tab shows numbers only");
            gpuFailed_ = true;
            return false;
        }
        dev->addRenderFeature(g->preview);
        g->meshes.setContentRoot(*dev, contentRoot());
        g->proxyBox = g->meshes.generated(*dev, "$instproxy",
            [](std::vector<rhi::MeshVertex>& v, std::vector<u32>& idx) { render::preview::appendPreviewUnitBox(v, idx); });
        gpu_ = std::move(g);
        ensureHook();
        wantFrame_ = true;
        return true;
    }

    // The Content directory mesh paths are relative to: the project's, else the nearest ancestor of this
    // file called Content.
    std::string contentRoot() const {
        std::string root = actorEditorContentRoot();
        if (!root.empty()) return root;
        for (std::filesystem::path p = std::filesystem::path(path_).parent_path(); !p.empty() && p != p.root_path();
             p = p.parent_path())
            if (p.filename() == "Content") return p.string();
        return std::filesystem::path(path_).parent_path().string();
    }

    // Loads a few meshes per frame for groups that have none yet, and tells the model their bounds.
    void loadMeshes(Gpu& g) {
        InstanceSetModel& m = model();
        if (groupGpu_.size() < m.groupCount()) groupGpu_.resize(m.groupCount());
        int budget = kMeshLoadsPerFrame;
        bool any = false;
        for (u32 gi = 0; gi < m.groupCount() && budget > 0; ++gi) {
            GroupGpu& gg = groupGpu_[gi];
            if (gg.tried || m.groupAlive(gi) == 0) continue;
            gg.tried = true;
            --budget;
            const std::string asset = m.groupAsset(gi);
            const bool known = loadedAssets_.count(asset) != 0;
            if (!known && loadedAssets_.size() >= kMaxMeshes) continue;   // stays a box proxy
            f32 radius = 0.0f;
            const rhi::MeshHandle h = g.meshes.resolve(*g.dev, asset, &radius);
            if (!h) { AVER_WARN("[InstanceSet] mesh '{}' did not load; drawing a box", asset); continue; }
            gg.mesh = h;
            any = true;
            if (known) continue;
            loadedAssets_.insert(asset);
            InstanceBounds b;
            f32 lo[3], hi[3], c[3], r = 0.0f;
            if (g.dev->meshBoundsAabb(h, lo, hi)) {
                for (int i = 0; i < 3; ++i) { b.lo[i] = lo[i]; b.hi[i] = hi[i]; }
            } else if (g.dev->meshBounds(h, c, &r) && r > 0.0f) {
                for (int i = 0; i < 3; ++i) { b.lo[i] = c[i] - r; b.hi[i] = c[i] + r; }
            } else if (radius > 0.0f) {
                for (int i = 0; i < 3; ++i) { b.lo[i] = -radius; b.hi[i] = radius; }
            }
            m.setAssetBounds(asset, b);
            boundsDirty_ = true;
        }
        if (any) { touch(); meshesLoadedThisFrame_ = true; }
    }

    // Matches the preview's target to the viewport, debounced: resize() costs a waitIdle.
    void requestPreviewSize(render::preview::ActorPreview& pv, f32 w, f32 h) {
        const u32 want[2] = {static_cast<u32>(w), static_cast<u32>(h)};
        const u32 have[2] = {pv.width(), pv.height()};
        const auto far_ = [](u32 a, u32 b) { return (a > b ? a - b : b - a) > 24u; };
        if (!far_(want[0], have[0]) && !far_(want[1], have[1])) { previewResizeAt_ = -1.0; return; }
        const double now = ImGui::GetTime();
        if (previewResizeAt_ < 0.0 || far_(want[0], pendingW_) || far_(want[1], pendingH_)) {
            previewResizeAt_ = now + 0.25;
            pendingW_ = want[0];
            pendingH_ = want[1];
            return;
        }
        if (now < previewResizeAt_) return;
        previewResizeAt_ = -1.0;
        pv.resize(pendingW_, pendingH_);
    }

    // Culls with the model and hands the preview this frame's batches and highlighted draws.
    void rebuild(Gpu& g, const f32 vp[16], const f32 eye[3]) {
        InstanceSetModel& m = model();
        render::preview::ActorPreview& pv = *g.preview;

        CullParams cp;
        std::memcpy(cp.viewProj, vp, sizeof cp.viewProj);
        for (int i = 0; i < 3; ++i) cp.eye[i] = eye[i];
        const f32 fov = std::max(pv.camera().fovDeg, 1.0f) * kPi / 180.0f;
        cp.pixelScale = static_cast<f32>(pv.height()) / (2.0f * std::tan(fov * 0.5f));
        cp.minPixels = kMinPixels;
        cp.maxCount = kDrawCap;
        m.cull(cp, cull_);
        shown_ = cull_.total;
        capped_ = cull_.capped;

        const bool hasSel = !m.selection().empty();
        f32 proxyS[12];
        xf::identity(proxyS);
        proxyS[0] = proxyS[4] = proxyS[8] = kProxyHalf;   // unit box has half-extent 1

        struct Span { rhi::MeshHandle mesh; usize off; u32 count; bool proxy; };
        std::vector<Span> spans, selSpans;
        worlds_.clear();
        selWorlds_.clear();
        selDraws_.clear();
        worlds_.reserve(static_cast<usize>(cull_.total) * 12);
        for (u32 gi = 0; gi < cull_.rows.size(); ++gi) {
            const std::vector<u32>& rows = cull_.rows[gi];
            if (rows.empty()) continue;
            const rhi::MeshHandle gm = gi < groupGpu_.size() ? groupGpu_[gi].mesh : 0;
            const bool proxy = gm == 0;
            const rhi::MeshHandle mesh = proxy ? g.proxyBox : gm;
            if (!mesh) continue;
            const usize off0 = worlds_.size(), sel0 = selWorlds_.size();
            for (const u32 row : rows) {
                const InstanceId id = makeInstanceId(gi, row);
                const f32* mat = m.matrix(id);
                if (!mat) continue;
                f32 tmp[12];
                if (proxy) { xf::mul(proxyS, mat, tmp); mat = tmp; }
                if (hasSel && m.selected(id)) {
                    if (selDraws_.size() < kMaxSingleDraws) {
                        SelDraw sd;
                        sd.group = gi;
                        xf::copy(sd.m, mat);
                        selDraws_.push_back(sd);
                    } else {
                        selWorlds_.insert(selWorlds_.end(), mat, mat + 12);
                    }
                } else {
                    worlds_.insert(worlds_.end(), mat, mat + 12);
                }
            }
            if (worlds_.size() > off0) spans.push_back({mesh, off0, static_cast<u32>((worlds_.size() - off0) / 12), proxy});
            if (selWorlds_.size() > sel0) selSpans.push_back({mesh, sel0, static_cast<u32>((selWorlds_.size() - sel0) / 12), proxy});
        }

        batches_.clear();
        for (const Span& s : spans) {
            render::preview::PreviewInstanceBatch b;
            b.mesh = s.mesh;
            b.worlds = worlds_.data() + s.off;
            b.count = s.count;
            if (s.proxy) { b.baseColor[0] = 0.45f; b.baseColor[1] = 0.6f; b.baseColor[2] = 0.85f; }
            batches_.push_back(b);
        }
        for (const Span& s : selSpans) {
            render::preview::PreviewInstanceBatch b;
            b.mesh = s.mesh;
            b.worlds = selWorlds_.data() + s.off;
            b.count = s.count;
            b.baseColor[0] = 1.0f; b.baseColor[1] = 0.55f; b.baseColor[2] = 0.12f;
            batches_.push_back(b);
        }
        pv.setInstanceBatches(batches_.data(), static_cast<u32>(batches_.size()));

        std::vector<render::preview::PreviewDraw> draws;
        draws.reserve(selDraws_.size());
        for (const SelDraw& sd : selDraws_) {
            render::preview::PreviewDraw d;
            const rhi::MeshHandle gm = sd.group < groupGpu_.size() ? groupGpu_[sd.group].mesh : 0;
            d.mesh = gm ? gm : g.proxyBox;
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) d.world[r*4+c] = sd.m[r*3+c];
                d.world[r*4+3] = 0.0f;
            }
            d.world[12] = sd.m[9]; d.world[13] = sd.m[10]; d.world[14] = sd.m[11]; d.world[15] = 1.0f;
            d.selected = true;
            d.baseColor[0] = 1.0f; d.baseColor[1] = 0.6f; d.baseColor[2] = 0.2f;
            draws.push_back(d);
        }
        pv.setDrawList(std::move(draws));
        drawnCount_ = 0;
        for (const render::preview::PreviewInstanceBatch& b : batches_) drawnCount_ += b.count;
        drawnCount_ += static_cast<u32>(selDraws_.size());
    }

    // ---- camera helpers ----------------------------------------------------------------------------
    void frameSelection(render::preview::ActorPreview& pv) {
        f32 lo[3], hi[3];
        if (!model().selectionBounds(lo, hi)) { pv.frameAll(); return; }
        f32 span = 0.0f;
        for (int i = 0; i < 3; ++i) {
            pv.camera().pivot[i] = (lo[i] + hi[i]) * 0.5f;
            span = std::max(span, hi[i] - lo[i]);
        }
        pv.camera().distance = std::clamp(std::max(span, 1.0f) * 2.2f, 20.0f, 500000.0f);
    }

    // ---- editing actions ---------------------------------------------------------------------------
    void doUndo() { if (model().undo()) { touch(); doc_.growBounds(); } }
    void doRedo() { if (model().redo()) { touch(); doc_.growBounds(); } }
    void doDelete() {
        if (model().editing() || model().selection().empty()) return;
        model().deleteSelected();
        touch();
    }
    void doDuplicate() {
        if (model().editing() || model().selection().empty()) return;
        const f32 off[3] = {50.0f, 50.0f, 0.0f};
        model().duplicateSelected(off);
        doc_.growBounds();
        touch();
    }
    void doSave() {
        std::string why;
        if (!save(&why)) AVER_ERROR("[InstanceSet] save failed for '{}': {}", path_, why);
    }

    // Adds an instance of the typed asset (else the current group's) on the ground under the view centre.
    void addInstanceAtViewCentre() {
        std::string asset = addAsset_;
        if (asset.empty() && currentGroup_ >= 0 && static_cast<usize>(currentGroup_) < model().groupCount())
            asset = model().groupAsset(static_cast<u32>(currentGroup_));
        if (asset.empty()) { status_ = "Pick a group or type an asset path first."; return; }

        f32 pos[3] = {0, 0, 0};
        if (gpu_ && gpu_->preview) {
            const render::preview::PreviewCamera& c = gpu_->preview->camera();
            for (int i = 0; i < 3; ++i) pos[i] = c.pivot[i];
            f32 eye[3];
            camEye(c, eye);
            f32 dir[3] = {c.pivot[0] - eye[0], c.pivot[1] - eye[1], c.pivot[2] - eye[2]};
            const f32 len = std::sqrt(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
            if (len > 1e-6f) {
                for (f32& v : dir) v /= len;
                const f32 groundZ = doc_.haveBounds() ? doc_.boundsLo()[2] : c.pivot[2];
                f32 hit[3];
                if (instset::rayPlaneZ(eye, dir, groundZ, hit)) for (int i = 0; i < 3; ++i) pos[i] = hit[i];
            }
        }
        f32 m12[12];
        xf::identity(m12);
        m12[9] = pos[0]; m12[10] = pos[1]; m12[11] = pos[2];
        const InstanceId id = model().add(asset, m12);
        model().select({id}, SelectMode::Replace);
        doc_.growBounds();
        touch();
        status_.clear();
        if (groupGpu_.size() < model().groupCount()) groupGpu_.resize(model().groupCount());
    }

    // ---- keys ----------------------------------------------------------------------------------------
    void handleKeys() {
        const ImGuiIO& io = ImGui::GetIO();
        if (!focused_ || io.WantTextInput) return;
        KeybindRegistry& kb = keybinds();
        if (kb.pressed(CommandId::ToolSelect, io)) tool_ = 0;
        if (kb.pressed(CommandId::ToolMove, io))   tool_ = 1;
        if (kb.pressed(CommandId::ToolRotate, io)) tool_ = 2;
        if (kb.pressed(CommandId::ToolScale, io))  tool_ = 3;
        if (kb.pressed(CommandId::ViewFrameSelected, io) && gpu_ && gpu_->preview) frameSelection(*gpu_->preview);
        if (kb.pressed(CommandId::EditDelete, io))    doDelete();
        if (kb.pressed(CommandId::EditDuplicate, io)) doDuplicate();
        if (kb.pressed(CommandId::EditUndo, io) && model().canUndo()) doUndo();
        if (kb.pressed(CommandId::EditRedo, io) && model().canRedo()) doRedo();
        if (kb.pressed(CommandId::AssetSave, io)) doSave();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && leftState_ == Left::Gizmo) {
            model().cancelEdit();
            gizmo::endDrag(drag_);
            leftState_ = Left::None;
            touch();
        }
    }

    // ---- the viewport ------------------------------------------------------------------------------
    void drawViewport(Engine& e, f32 dpi) {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const f32 availW = std::max(64.0f, avail.x), availH = std::max(64.0f, avail.y);
        if (!ensureGpu(e, availW, availH)) {
            ImGui::TextDisabled("No preview on this backend.");
            return;
        }
        Gpu& g = *gpu_;
        render::preview::ActorPreview& pv = *g.preview;
        pv.setEnabled(true);
        pv.setViewMode(viewMode_);
        pv.setShowFlags(show_);
        requestPreviewSize(pv, availW, availH);

        meshesLoadedThisFrame_ = false;
        loadMeshes(g);
        if (boundsDirty_ && !meshesLoadedThisFrame_) {
            doc_.recomputeBounds();
            boundsDirty_ = false;
            touch();
        }
        if (doc_.haveBounds()) pv.setExternalBounds(doc_.boundsLo(), doc_.boundsHi());
        else pv.clearExternalBounds();
        if (wantFrame_) { pv.frameAll(); wantFrame_ = false; }

        f32 vp[16], eye[3];
        pv.viewProj(vp);
        camEye(pv.camera(), eye);
        if (!haveVp_ || std::memcmp(vp, lastVp_, sizeof vp) != 0 || epoch_ != builtEpoch_) {
            rebuild(g, vp, eye);
            std::memcpy(lastVp_, vp, sizeof vp);
            haveVp_ = true;
            builtEpoch_ = epoch_;
        }

        // The image: letterboxed (fit, never stretch) while the debounced resize catches up.
        const u64 texId = pv.uiTextureId();   // read fresh: a resize changes it
        if (!texId) { ImGui::TextDisabled("Preparing viewport..."); return; }
        const f32 texW = static_cast<f32>(pv.width()), texH = static_cast<f32>(pv.height());
        const f32 fit = std::min(availW / std::max(texW, 1.0f), availH / std::max(texH, 1.0f));
        const ImVec2 imgSize(std::max(texW * fit, 16.0f), std::max(texH * fit, 16.0f));
        const ImVec2 origin = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(origin.x + std::max(0.0f, (availW - imgSize.x) * 0.5f),
                                   origin.y + std::max(0.0f, (availH - imgSize.y) * 0.5f)));
        const ImVec2 imageMin = ImGui::GetCursorScreenPos();
        ImGui::Image(static_cast<ImTextureID>(texId), imgSize);
        const ImVec2 imageMax(imageMin.x + imgSize.x, imageMin.y + imgSize.y);

        drawPreviewToolbar("##instToolbar", imageMin, imageMax, dpi, viewMode_, show_);
        // After the toolbar, which leaves the image as the last item (and reads it as not hovered while
        // the pointer is over the toolbar).
        const bool hovered = ImGui::IsItemHovered();

        interact(pv, vp, eye, imageMin, imgSize, hovered);
        drawOverlays(pv, vp, imageMin, imageMax, imgSize, dpi);
    }

    // Selection frame for the gizmo; false when there is nothing to move.
    bool gizmoFrame(gizmo::Frame& f) {
        InstanceSetModel& m = model();
        const std::vector<InstanceId>& sel = m.selection();
        const gizmo::Mode mode = gizmoMode();
        if (sel.empty()) return false;
        if (sel.size() == 1) {
            const f32* mm = m.matrix(sel.front());
            if (!mm) return false;
            gizmo::frameFromMatrix(mode, space_, mm, f);
            return true;
        }
        f32 lo[3], hi[3];
        if (!m.selectionBounds(lo, hi)) return false;
        const f32 c[3] = {(lo[0]+hi[0])*0.5f, (lo[1]+hi[1])*0.5f, (lo[2]+hi[2])*0.5f};
        gizmo::frameAt(mode, space_, c, nullptr, f);
        return true;
    }
    gizmo::Mode gizmoMode() const {
        return tool_ == 2 ? gizmo::Mode::Rotate : tool_ == 3 ? gizmo::Mode::Scale : gizmo::Mode::Translate;
    }

    void interact(render::preview::ActorPreview& pv, const f32 vp[16], const f32 eye[3], ImVec2 imageMin,
                  ImVec2 imgSize, bool hovered) {
        const ImGuiIO& io = ImGui::GetIO();
        InstanceSetModel& m = model();
        const f32 cur[2] = {io.MousePos.x - imageMin.x, io.MousePos.y - imageMin.y};

        gizmo::View gv;
        std::memcpy(gv.viewProj, vp, sizeof gv.viewProj);
        gv.width = imgSize.x;
        gv.height = imgSize.y;

        // ---- navigation: RMB / Alt+LMB orbit, MMB pan, wheel zoom (the mesh viewer's gestures) ----
        if (navButton_ < 0 && hovered) {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) { navButton_ = ImGuiMouseButton_Right; navPan_ = false; }
            else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) { navButton_ = ImGuiMouseButton_Middle; navPan_ = true; }
            else if (io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { navButton_ = ImGuiMouseButton_Left; navPan_ = false; }
        }
        if (navButton_ >= 0) {
            if (ImGui::IsMouseDown(navButton_)) {
                if (navPan_) pv.camera().panPixels(io.MouseDelta.x, io.MouseDelta.y, static_cast<f32>(pv.height()));
                else pv.camera().addOrbit(io.MouseDelta.x * 0.4f, io.MouseDelta.y * 0.4f);
            } else {
                navButton_ = -1;
            }
        }
        if (hovered && io.MouseWheel != 0.0f) pv.camera().addZoom(io.MouseWheel > 0.0f ? 0.9f : 1.1f);

        // ---- left button: gizmo drag, marquee, click-pick ----
        gizmo::Frame frame;
        const bool haveFrame = tool_ > 0 && gizmoFrame(frame);

        hotAxis_ = gizmo::kAxisNone;
        if (leftState_ == Left::None && hovered && navButton_ < 0 && haveFrame)
            hotAxis_ = gizmo::pickAxis(gv, frame, gizmoMode(), cur);

        if (leftState_ == Left::None && hovered && navButton_ < 0 && !io.KeyAlt &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            bool started = false;
            if (haveFrame && hotAxis_ != gizmo::kAxisNone) {
                gizmo::Snap snap;
                snap.translateCm = snapT_; snap.rotateDeg = snapR_; snap.scaleStep = snapS_;
                if (gizmo::beginDrag(drag_, gv, frame, gizmoMode(), hotAxis_, cur, snap)) {
                    m.beginEdit();
                    leftState_ = Left::Gizmo;
                    started = true;
                }
            }
            if (!started) {
                leftState_ = Left::Marquee;
                marqueeStart_ = io.MousePos;
            }
        }

        if (leftState_ == Left::Gizmo) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                f32 delta[12];
                if (gizmo::updateDrag(drag_, cur, delta)) {
                    m.applyDeltaToSelected(delta);
                    touch();
                }
                hotAxis_ = drag_.axis;
            } else {
                m.endEdit();
                gizmo::endDrag(drag_);
                leftState_ = Left::None;
                doc_.growBounds();
                touch();
            }
        } else if (leftState_ == Left::Marquee && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const SelectMode mode = io.KeyShift ? SelectMode::Add : io.KeyCtrl ? SelectMode::Toggle : SelectMode::Replace;
            const f32 dx = io.MousePos.x - marqueeStart_.x, dy = io.MousePos.y - marqueeStart_.y;
            if (dx*dx + dy*dy < 16.0f) {
                f32 dir[3];
                const f32 ndcX = cur[0] / imgSize.x * 2.0f - 1.0f, ndcY = 1.0f - cur[1] / imgSize.y * 2.0f;
                PickHit hit;
                const f32 farDist = pv.camera().distance * 10.0f + 1000.0f;
                if (instset::viewRay(vp, ndcX, ndcY, eye, dir) && m.pick(eye, dir, farDist, hit)) {
                    m.select({hit.id}, mode);
                } else if (mode == SelectMode::Replace) {
                    m.selectNone();
                }
            } else {
                const f32 ax = marqueeStart_.x - imageMin.x, ay = marqueeStart_.y - imageMin.y;
                const f32 nx0 = std::min(ax, cur[0]) / imgSize.x * 2.0f - 1.0f;
                const f32 nx1 = std::max(ax, cur[0]) / imgSize.x * 2.0f - 1.0f;
                const f32 ny1 = 1.0f - std::min(ay, cur[1]) / imgSize.y * 2.0f;
                const f32 ny0 = 1.0f - std::max(ay, cur[1]) / imgSize.y * 2.0f;
                f32 sub[16];
                if (subFrustumViewProj(vp, std::clamp(nx0, -1.0f, 1.0f), std::clamp(ny0, -1.0f, 1.0f),
                                       std::clamp(nx1, -1.0f, 1.0f), std::clamp(ny1, -1.0f, 1.0f), sub))
                    m.selectInFrustum(sub, mode);
            }
            leftState_ = Left::None;
            touch();
        }
    }

    void drawOverlays(render::preview::ActorPreview& pv, const f32 vp[16], ImVec2 imageMin, ImVec2 imageMax,
                      ImVec2 imgSize, f32 dpi) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(imageMin, imageMax, true);

        gizmo::Frame frame;
        if (tool_ > 0 && gizmoFrame(frame)) {
            gizmo::View gv;
            std::memcpy(gv.viewProj, vp, sizeof gv.viewProj);
            gv.width = imgSize.x;
            gv.height = imgSize.y;
            gizmo::draw(dl, imageMin.x, imageMin.y, gv, frame, gizmoMode(), hotAxis_);
        }
        if (leftState_ == Left::Marquee) {
            const ImGuiIO& io = ImGui::GetIO();
            const f32 dx = io.MousePos.x - marqueeStart_.x, dy = io.MousePos.y - marqueeStart_.y;
            if (dx*dx + dy*dy >= 16.0f) {
                dl->AddRectFilled(marqueeStart_, io.MousePos, IM_COL32(255, 160, 50, 40));
                dl->AddRect(marqueeStart_, io.MousePos, IM_COL32(255, 160, 50, 220));
            }
        }
        dl->PopClipRect();

        std::vector<std::string> lines;
        lines.push_back("Instances: " + formatCount(model().aliveCount()));
        lines.push_back("In view: " + formatCount(shown_) + (capped_ ? "  (capped)" : ""));
        lines.push_back("Drawn: " + formatCount(drawnCount_) + "  of cap " + formatCount(kDrawCap));
        lines.push_back("Selected: " + formatCount(model().selection().size()));
        drawPreviewStats(imageMin, dpi, lines);
        drawPreviewAxes(imageMin, imageMax, dpi, pv.camera());

        const char* hint = "LMB select / drag marquee   RMB orbit   MMB pan   Wheel zoom   F frame";
        const ImVec2 ts = ImGui::CalcTextSize(hint);
        dl->AddText(ImVec2(imageMin.x + (imgSize.x - ts.x) * 0.5f, imageMax.y - ts.y - 6.0f * dpi),
                    IM_COL32(220, 220, 225, 150), hint);
    }

    // ---- the side panel ----------------------------------------------------------------------------
    void drawSidePanel(f32 dpi) {
        InstanceSetModel& m = model();
        const ImVec4 amber(1.0f, 0.75f, 0.3f, 1.0f);

        ImGui::TextUnformatted("Instance Set");
        ImGui::SameLine();
        ImGui::TextDisabled("  %s", std::filesystem::path(path_).filename().string().c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path_.c_str());
        if (m.readOnly()) ImGui::TextColored(amber, "Read-only: a baked level pack. Bake Pack regenerates it.");   // [wave3/level-instances]
        ImGui::Separator();

        if (externalChange_) {
            ImGui::TextColored(amber, "This file changed on disk.");
            if (ImGui::Button("Reload (discard my edits)")) reloadFromDisk();
            ImGui::SameLine();
            if (ImGui::Button("Keep my edits")) externalChange_ = false;
            ImGui::Separator();
        }

        if (ImGui::Button("Save")) doSave();
        ImGui::SameLine();
        ImGui::BeginDisabled(!m.canUndo());
        if (ImGui::Button("Undo")) doUndo();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!m.canRedo());
        if (ImGui::Button("Redo")) doRedo();
        ImGui::EndDisabled();

        if (staleNotice_) {
            ImGui::Spacing();
            ImGui::TextColored(amber, "Saved. The level's streaming data for this file is out of date.");
            const bool refs = g_hooks.levelReferences && g_hooks.levelReferences(path_);
            if (refs && g_hooks.regenerateStreaming) {
                if (ImGui::Button("Regenerate Streaming Data")) {
                    std::string report;
                    const bool ok = g_hooks.regenerateStreaming(report);
                    regenReport_ = ok ? report : "Failed: " + report;
                    if (ok) staleNotice_ = false;
                }
            } else {
                ImGui::TextDisabled("Open the level that uses it and run World Settings > Regenerate Streaming Data.");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Dismiss")) staleNotice_ = false;
        }
        if (!regenReport_.empty()) ImGui::TextDisabled("%s", regenReport_.c_str());
        ImGui::Separator();

        // Tools
        static const char* kToolNames[] = {"Select (1)", "Move (2)", "Rotate (3)", "Scale (4)"};
        for (int i = 0; i < 4; ++i) {
            if (i) ImGui::SameLine();
            const bool on = tool_ == i;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(kToolNames[i])) tool_ = i;
            if (on) ImGui::PopStyleColor();
        }
        bool local = space_ == gizmo::Space::Local;
        if (ImGui::Checkbox("Local space", &local)) space_ = local ? gizmo::Space::Local : gizmo::Space::World;
        ImGui::SetNextItemWidth(70.0f * dpi);
        ImGui::DragFloat("Snap cm", &snapT_, 0.5f, 0.0f, 10000.0f, "%.1f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60.0f * dpi);
        ImGui::DragFloat("deg", &snapR_, 0.5f, 0.0f, 180.0f, "%.1f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60.0f * dpi);
        ImGui::DragFloat("scale", &snapS_, 0.01f, 0.0f, 10.0f, "%.2f");
        ImGui::Separator();

        // Status
        ImGui::Text("%s instances in %zu group%s", formatCount(m.aliveCount()).c_str(), m.groupCount(),
                    m.groupCount() == 1 ? "" : "s");
        ImGui::TextDisabled("In view %s, drawn %s (cap %s)%s", formatCount(shown_).c_str(),
                            formatCount(drawnCount_).c_str(), formatCount(kDrawCap).c_str(),
                            capped_ ? " - nearest kept" : "");
        if (!status_.empty()) ImGui::TextColored(amber, "%s", status_.c_str());
        ImGui::Separator();

        drawGroups(dpi);
        ImGui::Separator();
        drawSelection(dpi);
    }

    void drawGroups(f32 dpi) {
        InstanceSetModel& m = model();
        ImGui::TextUnformatted("Groups");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##instFilter", "Filter by asset path", filter_, sizeof filter_);
        const std::string needle = lowered(filter_);

        const f32 listH = std::max(90.0f * dpi, ImGui::GetContentRegionAvail().y * 0.35f);
        if (ImGui::BeginChild("##instGroups", ImVec2(0.0f, listH), ImGuiChildFlags_Borders)) {
            for (u32 g = 0; g < m.groupCount(); ++g) {
                const std::string& asset = m.groupAsset(g);
                if (!needle.empty() && lowered(asset).find(needle) == std::string::npos) continue;
                const bool noMesh = g < groupGpu_.size() && groupGpu_[g].tried && groupGpu_[g].mesh == 0;
                char label[600];
                std::snprintf(label, sizeof label, "%s  (%u)%s###grp%u", asset.c_str(), m.groupAlive(g),
                              noMesh ? "  [no mesh]" : "", g);
                ImGui::BeginDisabled(m.groupAlive(g) == 0);
                if (ImGui::Selectable(label, currentGroup_ == static_cast<int>(g))) {
                    currentGroup_ = static_cast<int>(g);
                    m.selectGroup(g, ImGui::GetIO().KeyCtrl ? SelectMode::Add : SelectMode::Replace);
                    touch();
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nClick to select the whole group; Ctrl adds.", asset.c_str());
            }
        }
        ImGui::EndChild();

        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##instAddAsset", "Asset path to add (blank: current group)", addAsset_, sizeof addAsset_);
        if (ImGui::Button("Add instance at view centre")) addInstanceAtViewCentre();
    }

    void drawSelection(f32 dpi) {
        (void)dpi;
        InstanceSetModel& m = model();
        const std::vector<InstanceId> sel = m.selection();
        ImGui::Text("Selection: %s", formatCount(sel.size()).c_str());
        if (sel.empty()) {
            if (fieldEditOpen_) { m.endEdit(); fieldEditOpen_ = false; }
            ImGui::TextDisabled("Click an instance, drag a marquee, or pick a group.");
            return;
        }
        const f32* mat = m.matrix(sel.front());
        if (!mat) return;
        instset::Trs t;
        instset::decompose(mat, t);
        const bool single = sel.size() == 1;
        bool changed = false;

        f32 pos[3] = {t.pos[0], t.pos[1], t.pos[2]};
        if (ImGui::DragFloat3("Position", pos, 1.0f, 0.0f, 0.0f, "%.2f")) {
            openFieldEdit(t.pos);
            if (single) {
                t.pos[0] = pos[0]; t.pos[1] = pos[1]; t.pos[2] = pos[2];
                f32 nm[12];
                instset::compose(t, nm);
                m.setTransform(sel.front(), nm);
            } else {
                f32 d[12];
                xf::identity(d);
                for (int i = 0; i < 3; ++i) d[9 + i] = pos[i] - fieldPos0_[i];
                m.applyDeltaToSelected(d);
            }
            changed = true;
        }
        ImGui::BeginDisabled(!single);
        f32 rot[3] = {t.rotDeg[0], t.rotDeg[1], t.rotDeg[2]};
        if (ImGui::DragFloat3("Rotation (yaw, pitch, roll)", rot, 0.5f, 0.0f, 0.0f, "%.1f")) {
            openFieldEdit(t.pos);
            for (int i = 0; i < 3; ++i) t.rotDeg[i] = rot[i];
            f32 nm[12];
            instset::compose(t, nm);
            m.setTransform(sel.front(), nm);
            changed = true;
        }
        f32 us = (t.scale[0] + t.scale[1] + t.scale[2]) / 3.0f;
        if (ImGui::DragFloat("Scale", &us, 0.01f, 0.0001f, 10000.0f, "%.3f")) {
            openFieldEdit(t.pos);
            const f32 base = (t.scale[0] + t.scale[1] + t.scale[2]) / 3.0f;
            const f32 k = base > 1e-9f ? us / base : 1.0f;
            for (f32& s : t.scale) s *= k;
            f32 nm[12];
            instset::compose(t, nm);
            m.setTransform(sel.front(), nm);
            changed = true;
        }
        ImGui::EndDisabled();
        if (!single) ImGui::TextDisabled("Rotation and scale: use the gizmo for several at once.");
        if (changed) { doc_.growBounds(); touch(); }
        // A field edit is one undo step: closed when no widget is held any more.
        if (fieldEditOpen_ && !ImGui::IsAnyItemActive()) { m.endEdit(); fieldEditOpen_ = false; touch(); }
    }

    void openFieldEdit(const f32 pos0[3]) {
        if (fieldEditOpen_) return;
        model().beginEdit();
        fieldEditOpen_ = true;
        for (int i = 0; i < 3; ++i) fieldPos0_[i] = pos0[i];
    }

    // ---- state ---------------------------------------------------------------------------------------
    std::unique_ptr<Gpu> gpu_;
    bool gpuFailed_ = false;
    bool focused_ = false;
    bool wantFrame_ = true;
    bool boundsDirty_ = false;
    bool meshesLoadedThisFrame_ = false;

    SplitPane split_;
    render::preview::PreviewViewMode viewMode_ = render::preview::PreviewViewMode::Lit;
    render::preview::PreviewShowFlags show_{};
    u32 pendingW_ = 0, pendingH_ = 0;
    double previewResizeAt_ = -1.0;

    u32 epoch_ = 1, builtEpoch_ = 0;
    f32 lastVp_[16] = {};
    bool haveVp_ = false;
    std::vector<GroupGpu> groupGpu_;
    std::unordered_set<std::string> loadedAssets_;
    CullResult cull_;
    std::vector<f32> worlds_, selWorlds_;
    std::vector<SelDraw> selDraws_;
    std::vector<render::preview::PreviewInstanceBatch> batches_;
    u32 shown_ = 0, drawnCount_ = 0;
    bool capped_ = false;

    int tool_ = 0;   // 0 select, 1 move, 2 rotate, 3 scale
    gizmo::Space space_ = gizmo::Space::World;
    f32 snapT_ = 0.0f, snapR_ = 0.0f, snapS_ = 0.0f;
    gizmo::Drag drag_;
    int hotAxis_ = gizmo::kAxisNone;
    Left leftState_ = Left::None;
    ImVec2 marqueeStart_{0.0f, 0.0f};
    int navButton_ = -1;
    bool navPan_ = false;

    bool fieldEditOpen_ = false;
    f32 fieldPos0_[3] = {0, 0, 0};
    int currentGroup_ = -1;
    char filter_[128] = {};
    char addAsset_[256] = {};
    std::string status_;
#endif // AVER_WITH_IMGUI

    std::string path_;
    InstanceSetDoc doc_;
    std::string loadError_;
    bool staleNotice_ = false;
    bool externalChange_ = false;
    std::string regenReport_;
};

#if AVER_WITH_IMGUI
void hidePreview(InstanceSetEditor& ed) { ed.hideForFrame(); }

void tickHook(ImGuiContext*, ImGuiContextHook*) {
    if (!g_graveyard.empty()) {
        for (auto& g : g_graveyard) destroyGpu(*g);
        g_graveyard.clear();
    }
    for (InstanceSetEditor* ed : g_live) hidePreview(*ed);
}
#endif

} // namespace

void setInstanceSetEditorHooks(InstanceSetEditorHooks hooks) { g_hooks = std::move(hooks); }

void shutdownInstanceSetEditors() {
#if AVER_WITH_IMGUI
    for (auto& g : g_graveyard) destroyGpu(*g);
    g_graveyard.clear();
    for (InstanceSetEditor* ed : g_live) ed->releaseGpuNow();
    g_shutDown = true;
#endif
    g_hooks = InstanceSetEditorHooks{};
}

std::unique_ptr<AssetEditor> makeInstanceSetEditor(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext != ".ocinst") return nullptr;
#if AVER_WITH_IMGUI
    g_shutDown = false;
#endif
    return std::make_unique<InstanceSetEditor>(path);
}

} // namespace aver::editor
