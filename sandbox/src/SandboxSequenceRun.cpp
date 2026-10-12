// Camera paths in the editor: the Animate mode's path overlay, the --sequence-play capture run, and the
// Movie Render Queue (--render-queue <job.ocrender>, Window > Render Queue) built on the same run mode.
// Part of SandboxApp; the class is declared in SandboxApp.hpp, the sequence itself lives in SequenceEditor,
// the frame plan and file output in RenderQueueOutput. docs/EDITOR.md Phase 15, docs/RENDER_QUEUE.md.

#include "SandboxApp.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace aver {

#if AVER_MODULE_SCENE

namespace {

// The frame is the backbuffer; the capture is its 3D viewport rect, clamped to the image.
bool cropViewport(const std::vector<u8>& img, u32 iw, u32 ih, f32 vx, f32 vy, f32 vw, f32 vh,
                  std::vector<u8>& out, u32& ow, u32& oh) {
    const auto clampTo = [](f32 v, u32 hi) -> u32 { return v <= 0.0f ? 0u : (v >= static_cast<f32>(hi) ? hi : static_cast<u32>(v)); };
    const u32 cx = clampTo(vx, iw), cy = clampTo(vy, ih);
    const u32 cw = clampTo(vw, iw - cx), ch = clampTo(vh, ih - cy);
    if (cw == 0 || ch == 0) { out = img; ow = iw; oh = ih; return true; }
    out.resize(static_cast<usize>(cw) * ch * 4);
    for (u32 row = 0; row < ch; ++row)
        std::memcpy(out.data() + static_cast<usize>(row) * cw * 4,
                    img.data() + (static_cast<usize>(cy + row) * iw + cx) * 4, static_cast<usize>(cw) * 4);
    ow = cw;
    oh = ch;
    return true;
}

// Waiting ends after this many frames without a level and a camera track; the run fails then.
constexpr u32 kGiveUpFrames = 6000;

} // namespace

void SandboxApp::setSequenceRun(const SequenceRunArgs& a) {
    using Phase = SequenceRunState::Phase;
    seqRun_.args = a;
    seqRun_.args.every = std::max(1, a.every);
    seqRun_.args.warmup = std::max(0, a.warmup);
    seqRun_.phase = a.play ? Phase::Waiting : Phase::Off;
    if (a.play) setNoEditorChrome(true);   // scene only: no grid, gizmo or path lines in the frames
    if (a.play && !a.job.empty()) {
        // A render queue job; its own warm-up, frame range and output replace the plain capture's.
        SequenceRunState& r = seqRun_;
        r.isJob = true;
        r.exitWhenDone = true;
        std::error_code ec;
        const std::filesystem::path abs = std::filesystem::absolute(a.job, ec);
        r.jobDir = abs.parent_path().string();
        std::string why;
        if (!fmt::loadRenderJob(abs.string(), r.job, &why)) r.loadError = why;
    }
}

int SandboxApp::sequenceRunExit() const { return seqRun_.failed ? 1 : 0; }

// Once per frame, before the sequence ticks: waits for the level, then poses frame n for the whole pass.
void SandboxApp::sequenceRunStep(Engine& e) {
    using Phase = SequenceRunState::Phase;
    SequenceRunState& r = seqRun_;
    if (r.phase == Phase::Off || r.phase == Phase::Done) return;
    if (r.isJob) { renderJobStep(e); return; }

    const auto fail = [&](const char* why) {
        AVER_ERROR("[SequenceRun] {}", why);
        r.failed = true;
        r.phase = Phase::Done;
        seqEditor_.endRun();
        e.requestExit();
    };

    if (r.phase == Phase::Waiting) {
        ++r.waited;
        bool ready = !levelPath_.empty() && !projectLoading_;
#if AVER_MODULE_VOXI
        ready = ready && !voxiRenderer_.accelBuildsPending();
#endif
        if (ready && !seqEditor_.anyCamera()) {
            fail("the level has no camera path: key the camera in Animate mode (K) and save the level first");
            return;
        }
        if (!ready) {
            r.readyFrames = 0;
            if (r.waited > kGiveUpFrames) fail("the level was not ready after 6000 frames");
            return;
        }
        {   // [wave5/sequencer] the first sequence of the level that has a camera
            std::string selWhy;
            if (!seqEditor_.running() && !seqEditor_.selectRunSequence(std::string(), &selWhy)) { fail(selWhy.c_str()); return; }
        }
        // Frame 0's pose is held through the warmup, so the temporal histories settle on the first view
        // instead of meeting a camera jump at frame 0. A level reload during the wait drops the run
        // state (running() false), which starts it over.
        if (!seqEditor_.running()) {
            setEditorMode(EditorMode::Animate);
            if (mode_ != EditorMode::Animate) { fail("could not enter Animate mode"); return; }
            seqEditor_.beginRun();
            seqEditor_.runFrame(0);
        }
        if (++r.readyFrames < static_cast<u32>(r.args.warmup)) return;
        r.frames = seqEditor_.runFrameCount();
        r.frame = 0;
        r.frameSet = false;
        r.phase = Phase::Running;
        if (!r.args.dir.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(r.args.dir, ec);
        }
        AVER_INFO("[SequenceRun] start: {} frame(s) at {:.0f} fps{}{}", r.frames, seqEditor_.runFps(),
                  r.args.dir.empty() ? "" : ", capturing to ", r.args.dir.empty() ? "" : r.args.dir.c_str());
    }

    if (r.phase == Phase::Running) {
        if (r.frameSet) ++r.frame;   // the previous frame has been rendered
        r.frameSet = true;
        if (!seqEditor_.runFrame(r.frame)) {
            r.phase = Phase::Draining;
        } else if (r.frame % 60 == 0) {
            AVER_INFO("[SequenceRun] frame {}/{}", r.frame, r.frames);
        }
    }

    if (r.phase == Phase::Draining && r.pendingFrame < 0) {
        AVER_INFO("[SequenceRun] done: {} image(s) written{}", r.written,
                  r.missed ? " (some captures never arrived, see the warnings above)" : "");
        r.phase = Phase::Done;
        seqEditor_.endRun();
        e.requestExit();
    }
}

// In onRender, after the frame is submitted: reads the image requested last frame, then requests this
// frame's. The camera of frame n was set in onUpdate, so image n shows exactly that pose.
void SandboxApp::serviceSequenceCapture(Engine& e) {
    using Phase = SequenceRunState::Phase;
    SequenceRunState& r = seqRun_;
    if (r.isJob) { serviceRenderCapture(e); return; }
    if ((r.phase != Phase::Running && r.phase != Phase::Draining) || r.args.dir.empty()) return;

    if (r.pendingFrame >= 0 && e.time().frame > r.pendingEngineFrame) {
        std::vector<u8> img;
        u32 iw = 0, ih = 0;
        f32 probe[4];   // true once THIS request was serviced; getFrameImage alone would hand back the last image
        if (e.device()->getCapture(probe) && e.device()->getFrameImage(img, iw, ih) && iw && ih) {
            std::vector<u8> crop;
            u32 ow = 0, oh = 0;
            cropViewport(img, iw, ih, vpX_, vpY_, vpW_, vpH_, crop, ow, oh);
            char name[32];
            std::snprintf(name, sizeof name, "seq_%05lld.png", static_cast<long long>(r.pendingFrame));
            const std::string path = (std::filesystem::path(r.args.dir) / name).string();
            if (stbi_write_png(path.c_str(), static_cast<int>(ow), static_cast<int>(oh), 4, crop.data(),
                               static_cast<int>(ow) * 4)) {
                ++r.written;
            } else {
                AVER_WARN("[SequenceRun] could not write {}", path);
                ++r.missed;
            }
            r.pendingFrame = -1;
        } else if (++r.tries >= 30) {
            AVER_WARN("[SequenceRun] no image for frame {} after {} frames, skipped", r.pendingFrame, r.tries);
            ++r.missed;
            r.pendingFrame = -1;
        }
    }

    if (r.phase == Phase::Running && r.pendingFrame < 0 && r.frame % r.args.every == 0) {
        e.device()->requestCapture(static_cast<u32>(vpX_ + vpW_ * 0.5f), static_cast<u32>(vpY_ + vpH_ * 0.5f));
        r.pendingFrame = r.frame;
        r.pendingEngineFrame = e.time().frame;
        r.tries = 0;
    }
}

// ---- Movie Render Queue ---------------------------------------------------------------------------
//
// A job reuses run mode: the camera track is piloted at a time of the job's choosing, one engine frame
// shows one TEMPORAL SAMPLE, and its capture is read the next frame (the pipeline of the plain capture).
// The samples of one output frame average in linear light (RenderRun::addSample); their times spread
// over the shutter, so motion blur needs no engine support. While a job runs it owns the screen: the
// window is sized to the output, buildUI draws nothing, and the scene fills it.

namespace {

// Esc cancels, but only when this window has the keyboard (another app's Esc must not).
bool escapeDown(Engine& e) {
    return e.window() && e.window()->isForeground() && keyDownNow(0x1B /* VK_ESCAPE */);
}

} // namespace

bool SandboxApp::startRenderJob(const fmt::RenderJob& job, const std::string& baseDir, std::string& why) {
    SequenceRunState& r = seqRun_;
    if (sequenceRunActive()) { why = "a render is already running"; return false; }
    if (!window_) { why = "the render queue needs a window"; return false; }
#if AVER_MODULE_FRAMEWORK
    if (anyPlayActive()) { why = "stop Play before rendering"; return false; }
#endif
    if (job.level.empty() && levelPath_.empty()) { why = "open a level first"; return false; }
    if (job.level.empty() && !seqEditor_.anyCamera()) {
        why = "the level has no camera path: key the camera in Animate mode (K) and save the level first";
        return false;
    }
    const SequenceRunArgs keep = r.args;
    r = SequenceRunState{};
    r.args = keep;
    r.isJob = true;
    r.exitWhenDone = false;
    r.job = job;
    r.job.normalise();
    r.jobDir = baseDir;
    r.prevMode = mode_;
    r.prevChrome = noEditorChrome_;
    r.phase = SequenceRunState::Phase::Waiting;
    setNoEditorChrome(true);
    return true;
}

void SandboxApp::cancelRenderJob() {
    if (seqRun_.isJob && sequenceRunActive()) seqRun_.cancelRequested = true;
}

editor::RenderQueueHost SandboxApp::renderQueueHost() {
    editor::RenderQueueHost h;
    h.hasSequence = seqEditor_.anyCamera();
    h.sequenceName = seqEditor_.sequenceName();
    h.sequenceLength = seqEditor_.runLength();
    h.sequenceFps = seqEditor_.runFps();
    h.levelPath = levelPath_;
    h.baseDir = project_.valid() ? project_.dir : executableDir();
    h.busy = seqRun_.isJob && sequenceRunActive();
    h.progress = seqRun_.samplesTotal > 0 ? static_cast<f32>(seqRun_.samplesDone) / static_cast<f32>(seqRun_.samplesTotal) : 0.0f;
    h.status = seqRun_.status;
    h.statusIsError = seqRun_.statusIsError;
    h.start = [this, base = h.baseDir](const fmt::RenderJob& j, std::string& why) { return startRenderJob(j, base, why); };
    h.cancel = [this] { cancelRenderJob(); };
    return h;
}

// The scene rect while a job owns the screen: the output size when the window can hold it, else the
// largest rect of the output's aspect that fits (the frame is scaled up when it is written).
void SandboxApp::applyRenderJobViewport() {
    const f32 w = static_cast<f32>(seqRun_.job.width), h = static_cast<f32>(seqRun_.job.height);
    const f32 bw = window_ ? static_cast<f32>(window_->width()) : 0.0f;
    const f32 bh = window_ ? static_cast<f32>(window_->height()) : 0.0f;
    f32 vw = w, vh = h;
    if (bw >= 1.0f && bh >= 1.0f && (bw < w || bh < h)) {
        const f32 s = std::min(bw / w, bh / h);
        vw = std::max(1.0f, std::floor(w * s));
        vh = std::max(1.0f, std::floor(h * s));
    }
    vpX_ = 0.0f;
    vpY_ = 0.0f;
    vpW_ = vw;
    vpH_ = vh;
}

// Sizes the window to the output and takes the screen. False (nothing changed) when there is no window handle.
bool SandboxApp::renderJobEnter(Engine& e) {
    SequenceRunState& r = seqRun_;
#if defined(_WIN32)
    HWND hwnd = e.window() ? static_cast<HWND>(e.window()->nativeHandle()) : nullptr;
    if (!hwnd) return false;
    WINDOWPLACEMENT wp{};
    wp.length = sizeof wp;
    GetWindowPlacement(hwnd, &wp);
    r.savedWindow.resize(sizeof wp);
    std::memcpy(r.savedWindow.data(), &wp, sizeof wp);
    if (IsIconic(hwnd) || IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    RECT wr{}, cr{};
    GetWindowRect(hwnd, &wr);
    GetClientRect(hwnd, &cr);
    const int extraW = (wr.right - wr.left) - (cr.right - cr.left);
    const int extraH = (wr.bottom - wr.top) - (cr.bottom - cr.top);
    SetWindowPos(hwnd, nullptr, 0, 0, static_cast<int>(r.job.width) + extraW, static_cast<int>(r.job.height) + extraH,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    AVER_INFO("[RenderQueue] window sized for {}x{}", r.job.width, r.job.height);
#else
    (void)e;
#endif
    r.owns = true;
    return true;
}

// Ends the job: closes the video, hands the screen back, and either exits (command line) or returns to the editor.
void SandboxApp::renderJobFinish(Engine& e, bool ok) {
    SequenceRunState& r = seqRun_;
    std::string why;
    if (!renderRun_.end(why)) {
        AVER_WARN("[RenderQueue] the video file could not be finished: {}", why);
        if (ok) { ok = false; r.failed = true; r.status = "The video file could not be finished: " + why; r.statusIsError = true; }
    }
    if (ok) {
        r.status = "Rendered " + std::to_string(renderRun_.written()) + " frame(s) to " + renderRun_.dir();
        r.statusIsError = false;
        AVER_INFO("[RenderQueue] done: {}{}", r.status, r.missed ? " (some captures never arrived, see the warnings above)" : "");
    }
    r.phase = SequenceRunState::Phase::Done;
    seqEditor_.endRun();
    r.owns = false;
    if (r.exitWhenDone) { e.requestExit(); return; }
#if defined(_WIN32)
    if (r.savedWindow.size() == sizeof(WINDOWPLACEMENT) && e.window()) {
        WINDOWPLACEMENT wp{};
        std::memcpy(&wp, r.savedWindow.data(), sizeof wp);
        SetWindowPlacement(static_cast<HWND>(e.window()->nativeHandle()), &wp);
    }
#endif
    setNoEditorChrome(r.prevChrome);
    setEditorMode(r.prevMode);
}

// A capture was lost (the window was still resizing, or the readback never came): show that sample again.
void SandboxApp::renderJobRewind(i64 frame, i32 sub) {
    using Phase = SequenceRunState::Phase;
    SequenceRunState& r = seqRun_;
    r.frame = frame;
    r.sub = sub;
    r.rewind = true;
    r.sampleRequested = false;
    r.pendingFrame = -1;
    if (r.phase == Phase::Draining) r.phase = Phase::Running;
}

// Once per frame, before the sequence ticks: waits for the level, then poses each temporal sample in turn.
void SandboxApp::renderJobStep(Engine& e) {
    using Phase = SequenceRunState::Phase;
    SequenceRunState& r = seqRun_;
    const auto fail = [&](const std::string& why) {
        AVER_ERROR("[RenderQueue] {}", why);
        r.failed = true;
        r.status = why;
        r.statusIsError = true;
        renderJobFinish(e, false);
    };
    if (!r.loadError.empty()) { fail(r.loadError); return; }
    if (!e.window()) { fail("the render queue needs a window"); return; }
    if (r.cancelRequested || (r.owns && escapeDown(e))) { fail("Cancelled"); return; }

    const auto pose = [&] { seqEditor_.runAt(renderRun_.sampleTime(r.frame, r.sub)); r.hold = 0; };

    if (r.phase == Phase::Waiting) {
        ++r.waited;
        namespace fs = std::filesystem;
        bool onLevel = true;
        if (!r.job.level.empty()) {
            fs::path want = r.job.level;
            std::error_code ec;
            if (want.is_relative() && !fs::exists(want, ec)) {
                const fs::path bases[2] = {fs::path(r.jobDir), fs::path(project_.valid() ? project_.dir : std::string())};
                for (const fs::path& base : bases) {
                    if (!base.empty() && fs::exists(base / want, ec)) { want = base / want; break; }
                }
            }
            onLevel = !levelPath_.empty() && fs::equivalent(want, levelPath_, ec) && !ec;
            if (!onLevel && !r.levelRequested && !projectLoading_ && r.waited > 2) {
                if (!requestOpenLevel(want.string(), "render queue")) {
                    fail(openLevelError_.empty() ? "could not open the level " + want.string() : openLevelError_);
                    return;
                }
                r.levelRequested = true;
            }
        }
        bool ready = onLevel && !levelPath_.empty() && !projectLoading_ && pendingOpenPath_.empty();
#if AVER_MODULE_VOXI
        ready = ready && !voxiRenderer_.accelBuildsPending();
#endif
        if (ready && !seqEditor_.anyCamera()) {
            fail("the level has no camera path: key the camera in Animate mode (K) and save the level first");
            return;
        }
        if (!ready) {
            r.readyFrames = 0;
            if (r.waited > kGiveUpFrames) fail("the level was not ready after 6000 frames");
            return;
        }
        if (!r.planned) {
            // [wave5/sequencer] The job's `sequence` selects a level sequence by name (empty = the first with a camera).
            std::string why;
            if (!seqEditor_.running() && !seqEditor_.selectRunSequence(r.job.sequence, &why)) { fail(why); return; }
            why.clear();
            if (!renderRun_.begin(r.job, r.jobDir, seqEditor_.runLength(), seqEditor_.runFps(), why)) { fail(why); return; }
            r.planned = true;
            r.samplesTotal = renderRun_.frameCount() * renderRun_.samples();
            r.job = renderRun_.job();
        }
        if (!r.owns && !renderJobEnter(e)) { fail("could not size the window for the render"); return; }
        // The first pose is held through the warm-up so GI, TAA and the denoiser settle before frame 0. A
        // level reload during the wait drops the run state (running() false), which starts it over.
        if (!seqEditor_.running()) {
            setEditorMode(EditorMode::Animate);
            if (mode_ != EditorMode::Animate) { fail("could not enter Animate mode"); return; }
            seqEditor_.beginRun();
        }
        seqEditor_.runAt(renderRun_.sampleTime(renderRun_.firstFrame(), 0));
        // At least a few frames: the window resize and the new targets take a couple to arrive.
        if (++r.readyFrames < static_cast<u32>(std::max(r.job.warmupFrames, 4))) return;
        r.frame = renderRun_.firstFrame();
        r.sub = 0;
        r.pendingFrame = -1;
        r.sampleRequested = false;
        r.rewind = false;
        r.retries = 0;
        r.phase = Phase::Running;
        pose();
        AVER_INFO("[RenderQueue] start: {} frame(s) x {} sample(s) at {:.3f} fps, {}x{} {}", renderRun_.frameCount(),
                  renderRun_.samples(), renderRun_.fps(), r.job.width, r.job.height, fmt::renderOutputName(r.job.output));
        return;
    }

    if (r.phase == Phase::Running) {
        if (r.rewind) {
            r.rewind = false;
            pose();
        } else if (r.sampleRequested) {
            if (++r.sub >= renderRun_.samples()) { r.sub = 0; ++r.frame; }
            if (r.frame > renderRun_.lastFrame()) r.phase = Phase::Draining;
            else { r.sampleRequested = false; pose(); }
        }
    }

    if (r.phase == Phase::Draining && r.pendingFrame < 0) renderJobFinish(e, true);
}

// In onRender, after the frame is submitted: reads the capture requested last frame, then requests this
// sample's once its pose has been on screen long enough.
void SandboxApp::serviceRenderCapture(Engine& e) {
    using Phase = SequenceRunState::Phase;
    SequenceRunState& r = seqRun_;
    if (r.phase != Phase::Running && r.phase != Phase::Draining) return;
    const auto fail = [&](const std::string& why) {
        AVER_ERROR("[RenderQueue] {}", why);
        r.failed = true;
        r.status = why;
        r.statusIsError = true;
        renderJobFinish(e, false);
    };

    if (r.pendingFrame >= 0 && e.time().frame > r.pendingEngineFrame) {
        std::vector<u8> img;
        u32 iw = 0, ih = 0;
        f32 probe[4];   // true once THIS request was serviced; getFrameImage alone would hand back the last image
        bool lost = false;
        if (e.device()->getCapture(probe) && e.device()->getFrameImage(img, iw, ih) && iw && ih) {
            std::vector<u8> crop;
            u32 ow = 0, oh = 0;
            cropViewport(img, iw, ih, r.capX, r.capY, static_cast<f32>(r.capW), static_cast<f32>(r.capH), crop, ow, oh);
            if (ow == r.capW && oh == r.capH) {
                renderRun_.addSample(crop.data(), ow, oh);
                ++r.samplesDone;
                r.retries = 0;
                if (r.pendingSub + 1 >= renderRun_.samples()) {
                    std::string why;
                    if (!renderRun_.finishFrame(r.pendingFrame, why)) { fail(why); return; }
                    if ((r.pendingFrame - renderRun_.firstFrame()) % 30 == 0)
                        AVER_INFO("[RenderQueue] frame {}/{}", r.pendingFrame - renderRun_.firstFrame() + 1, renderRun_.frameCount());
                }
                r.pendingFrame = -1;
            } else {
                lost = true;   // the backbuffer was still the old size
            }
        } else if (++r.tries >= 30) {
            lost = true;
        }
        if (lost) {
            ++r.missed;
            if (++r.retries > 300) { fail("no usable image came back from the renderer"); return; }
            AVER_WARN("[RenderQueue] frame {} sample {}: no image, rendering it again", r.pendingFrame, r.pendingSub);
            renderJobRewind(r.pendingFrame, r.pendingSub);
        }
    }

    if (r.phase != Phase::Running || r.pendingFrame >= 0 || r.rewind || r.sampleRequested) return;
    ++r.hold;
    if (r.hold < static_cast<u32>(r.job.settleFrames) + 1) return;
    if (ptSceneView_ && !r.warnedPt) {
        // Path tracing restarts its accumulator at every pose; wait for the job's sample count. A view
        // that never accumulates (it yielded to another view) is waited on only for a bounded time.
        const u32 want = static_cast<u32>(std::min(r.job.ptSamples, 1600));
        const u32 limit = want / 8 + 120;
        const bool ready = r.hold >= 2 && ptSceneView_->samplesAccumulated() >= want;
        if (!ready) {
            if (r.hold > limit) {
                AVER_WARN("[RenderQueue] the path tracer reached {} of {} samples; rendering without waiting from now on",
                          ptSceneView_->samplesAccumulated(), want);
                r.warnedPt = true;
            }
            if (!r.warnedPt) return;
        }
    }
    r.capW = static_cast<u32>(vpW_);
    r.capH = static_cast<u32>(vpH_);
    r.capX = vpX_;
    r.capY = vpY_;
    e.device()->requestCapture(static_cast<u32>(vpX_ + vpW_ * 0.5f), static_cast<u32>(vpY_ + vpH_ * 0.5f));
    r.pendingFrame = r.frame;
    r.pendingSub = r.sub;
    r.pendingEngineFrame = e.time().frame;
    r.tries = 0;
    r.sampleRequested = true;
}

// Animate mode's camera path: the curve, a marker per key, the playhead. Lines are always on top, so a
// path that runs through a wall is still readable. The mesh is rebuilt only when its stamp changes.
void SandboxApp::drawSequencePath(Engine& e) {
    bool show = mode_ == EditorMode::Animate && !noEditorChrome_ && seqEditor_.active();
#if AVER_MODULE_FRAMEWORK
    show = show && !anyPlayActive();
#endif
    if (!show) {
        if (seqPathMesh_) { e.device()->destroyLineMesh(seqPathMesh_); seqPathMesh_ = 0; seqPathStamp_ = 0; }
        return;
    }
    const u64 stamp = seqEditor_.pathStamp();
    if (stamp != seqPathStamp_) {
        if (seqPathMesh_) { e.device()->destroyLineMesh(seqPathMesh_); seqPathMesh_ = 0; }
        std::vector<rhi::LineVertex> verts;
        if (seqEditor_.pathLines(verts)) seqPathMesh_ = e.device()->createLineMesh(verts.data(), static_cast<u32>(verts.size()));
        seqPathStamp_ = stamp;
    }
    if (!seqPathMesh_) return;
    const Mat4 world = Mat4::identity();   // vertices are in world space
    e.device()->setLineDepth(false);
    e.device()->setLineWidth(1.5f * dpi_);
    e.device()->drawLines(seqPathMesh_, &world.m[0][0]);
    e.device()->setLineWidth(1.0f);
    e.device()->setLineDepth(true);
}

#else  // !AVER_MODULE_SCENE

void SandboxApp::setSequenceRun(const SequenceRunArgs& a) {
    if (a.play) AVER_WARN("[SequenceRun] --sequence-play ignored: this build has no scene module");
}

bool SandboxApp::startRenderJob(const fmt::RenderJob&, const std::string&, std::string& why) {
    why = "this build has no scene module";
    return false;
}

void SandboxApp::cancelRenderJob() {}

#endif

} // namespace aver
