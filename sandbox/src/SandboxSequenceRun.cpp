// Camera paths in the editor: the Animate mode's path overlay and the --sequence-play capture run.
// Part of SandboxApp; the class is declared in SandboxApp.hpp, the sequence itself lives in SequenceEditor.
// docs/EDITOR.md, Phase 15.

#include "SandboxApp.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

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
}

int SandboxApp::sequenceRunExit() const { return seqRun_.failed ? 1 : 0; }

// Once per frame, before the sequence ticks: waits for the level, then poses frame n for the whole pass.
void SandboxApp::sequenceRunStep(Engine& e) {
    using Phase = SequenceRunState::Phase;
    SequenceRunState& r = seqRun_;
    if (r.phase == Phase::Off || r.phase == Phase::Done) return;

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
        if (ready && !seqEditor_.hasCameraKeys()) {
            fail("the level has no camera path: key the camera in Animate mode (K) and save the level first");
            return;
        }
        if (!ready) {
            r.readyFrames = 0;
            if (r.waited > kGiveUpFrames) fail("the level was not ready after 6000 frames");
            return;
        }
        if (++r.readyFrames < static_cast<u32>(r.args.warmup)) return;
        setEditorMode(EditorMode::Animate);
        seqEditor_.beginRun();
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
    if ((r.phase != Phase::Running && r.phase != Phase::Draining) || r.args.dir.empty()) return;

    if (r.pendingFrame >= 0 && e.time().frame > r.pendingEngineFrame) {
        std::vector<u8> img;
        u32 iw = 0, ih = 0;
        if (e.device()->getFrameImage(img, iw, ih) && iw && ih) {
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
    using Phase = SequenceRunState::Phase;
    if (a.play) AVER_WARN("[SequenceRun] --sequence-play ignored: this build has no scene module");
}

#endif

} // namespace aver
