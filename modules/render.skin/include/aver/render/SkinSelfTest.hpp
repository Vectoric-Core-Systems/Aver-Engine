#pragma once
// A one-shot ON-DEVICE check that the compute pass agrees with anim::skinVertices.
//
// This exists because the factory's own self-test cannot reach it. That runs at init, where there
// is no open command list, so it can prove the DESCRIPTORS are well formed and nothing about the
// dispatch. A skinning shader that reads the wrong stride, transposes the matrix, or drops the
// translation row still produces a plausible-looking mesh -- so the only thing that catches it is
// the same rig skinned both ways and compared number by number.
//
// It is a render feature so that it gets an open command list from the frame it is registered in,
// and it draws nothing: no target is bound, no scene state is read, no pipeline the scene uses is
// touched. Registering it changes the frame's pixels not at all.
//
// THREE INSTANCES, NOT ONE, AND EACH WITH ITS OWN RIG. The first version dispatched a single mesh
// per frame, and that is exactly why it missed a real bug: the bone ring lived on the PASS and
// advanced per dispatch, so instances one and three shared a slot and the third overwrote the
// descriptors the first's already-recorded dispatch would read. One instance can never see it.
// Three is the smallest number that can, and each carries a DIFFERENT rig so a crossed instance
// produces a wrong answer rather than a coincidentally identical one.
#include "aver/render/SkinningPass.hpp"

namespace aver::render {

// Builds a synthetic rig, skins it both ways, and reports whether the two agree.
class SkinSelfTest final : public rhi::IRenderFeature {
public:
    ~SkinSelfTest() override;

    // Allocates the synthetic mesh and its readback buffer. False means the check cannot run --
    // which is not a failure, and is reported as "unavailable" rather than as a mismatch.
    bool init(rhi::IDevice& dev);
    void shutdown();

    const char* name() const override { return "Aver.Skin.SelfTest"; }

    // Frame 1 records the dispatch and the copy; frame 2 waits for the GPU and compares. Doing the
    // comparison a frame later is what makes the readback meaningful: readBuffer synchronises
    // nothing, and a copy recorded this frame has not run yet.
    void prePass(rhi::IRenderContext& ctx) override;

    bool finished() const { return stage_ >= 2; }
    bool passed() const { return passed_; }
    // Largest absolute disagreement in centimetres, once finished(). Meaningless before.
    f32  worstPosition() const { return worstPos_; }
    // Largest absolute disagreement between the two unit normals, once finished().
    f32  worstNormal() const { return worstNrm_; }

private:
    // One skinned instance under test: its GPU residency, its own rig, and the CPU answer it is
    // measured against.
    struct Case {
        SkinnedMeshGpu    gpu{};
        rhi::BufferHandle readback = 0;
        std::vector<Mat4> skin;                 // this instance's rig, as poseToSkinning hands it over
        std::vector<f32>  cpuPositions, cpuNormals;
        std::vector<f32>  uvs;                  // what the shader must carry through unchanged
        u32               vertexCount = 0;
    };

    bool buildCase(u32 index, u32 bones, Case& c);
    void compare();

    // Three, for the reason in this class's comment. Raising it is free; lowering it below three
    // removes the only thing that can see a cross-instance collision.
    static constexpr u32 kCases = 3;

    SkinningPass       pass_;
    Case               cases_[kCases];
    rhi::IDevice*      dev_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;

    u32  stage_ = 0;                  // 0 record, 1 compare, 2 done
    bool passed_ = false;
    f32  worstPos_ = 0.0f, worstNrm_ = 0.0f;
    u32  uvMismatches_ = 0;           // uv is carried through, so anything but zero is a stride bug
};

} // namespace aver::render
