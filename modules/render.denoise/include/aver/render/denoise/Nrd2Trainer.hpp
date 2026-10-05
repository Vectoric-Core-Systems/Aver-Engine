// Nrd2Trainer -- NRD2 phase 4 (docs/rendering/NRD2.md, "Phase 4"): the per-tile parameter network trained
// in-engine on the GPU from the phase 3 pose files.
//
// PARAMETER SPACE ONLY (NEURAA_NRD.md section 7, rule 4): the loss, the validation metric, the checkpoint
// choice, early stopping and the live gate all compare predicted tile parameters with the oracle's. The
// network's output is never run through the resolve here.
//
// Records: 56x56 half-resolution patches (14x14 tiles) of a pose frame, standardised with the training
// set's per-channel statistics; targets = the oracle parameters (standardised per parameter) on the
// central 8x8 tiles only, weighted per tile and per signal by the pose file's tile weights.
//
// The CPU helpers below are the spec the GPU gather (nrd2_net.hlsl, CSNrd2Gather) mirrors and what
// Nrd2TrainerTest checks; Nrd2Trainer is the GPU session driven once per frame by the host.
#pragma once

#include <aver/core/Types.hpp>
#include <aver/render/denoise/Nrd2Dataset.hpp>
#include <aver/render/denoise/Nrd2Network.hpp>   // kNrd2WeightsFileName
#include <aver/render/neural/ConvNet.hpp>
#include <aver/render/neural/WeightFile.hpp>
#include <aver/rhi/RHI.hpp>
#include <aver/rhi/RHIResources.hpp>

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace aver::render::denoise {

// ---- the network and its records

inline constexpr u32 kNrd2PatchTiles  = 14;                      // a record: 14 x 14 tiles
inline constexpr u32 kNrd2PatchTexels = 4 * kNrd2PatchTiles;     // = 56 half-resolution texels
inline constexpr u32 kNrd2CoreOffset  = 3;                       // loss on tiles 3..10 of the patch
inline constexpr u32 kNrd2CoreTiles   = 8;

// Fixed: 12 -> 3x3 s2 16 -> 3x3 16 -> 3x3 s2 32 -> 3x3 32 -> 1x1 12 (linear). Input 4 tilesX x 4 tilesY,
// output tilesX x tilesY x 12 = the tile-parameter planes (plane = signal * 6 + field).
neural::ConvNetDesc nrd2NetworkDesc();
// Nrd2Params{}'s defaults, diffuse then specular.
void nrd2DefaultParams(f32 out[12]);

// Per-channel input and per-parameter output statistics of the training poses.
struct Nrd2Standardisation {
    f32 inMean[12] = {}, inStd[12] = {}, outMean[12] = {}, outStd[12] = {};
};
// AVNN v2 io affine: inScale 1/std, inBias -mean/std (x' = x inScale + inBias); outScale std, outBias mean.
neural::ConvIoAffine nrd2IoAffine(const Nrd2Standardisation& s);
// Back from a file's affine; false when it is absent or the wrong size.
bool nrd2StandardisationFromAffine(const neural::ConvIoAffine& io, Nrd2Standardisation& s);
// Target standardisation t' = t * scale + bias (scale 1/outStd, bias -outMean/outStd), as the gather uses.
void nrd2TargetAffine(const Nrd2Standardisation& s, f32 scale[12], f32 bias[12]);

// fp64 sums: inputs over every texel of every frame, outputs weighted by the signal's tile weight.
// Std floors: 1e-3 (inputs), 1e-2 (outputs); an output with no weight keeps the default and std 1.
class Nrd2StatsAccumulator {
public:
    void addPose(const Nrd2Pose& p);
    [[nodiscard]] bool finish(Nrd2Standardisation& out) const;   // false when nothing was added
    [[nodiscard]] u32 poses() const { return poses_; }

private:
    f64 in1_[12] = {}, in2_[12] = {}, inN_[12] = {};
    f64 out1_[12] = {}, out2_[12] = {}, outW_[12] = {};
    u32 poses_ = 0;
};

// One record: a pose, its frame, and the patch's top-left tile (may be negative: past-edge texels read 0
// in standardised space, as the conv's zero padding at inference).
struct Nrd2PatchRef {
    u32 pose = 0, frame = 0;
    i32 tx0 = 0, ty0 = 0;
};

// CPU twin of CSNrd2Gather. input: 12 x 56 x 56; target, weight: 12 x 14 x 14 (any may be null).
// Inside the pose: input x * inScale + inBias (non-finite -> 0); target theta * scale + bias with weight
// = the signal's tile weight on core tiles (non-finite theta or weight, or weight <= 0 -> 0 and 0).
void nrd2ExtractPatch(const Nrd2Pose& pose, const neural::ConvIoAffine& io, const f32 tScale[12],
                      const f32 tBias[12], const Nrd2PatchRef& r, f32* input, f32* target, f32* weight);

// Header facts of a pose file (no full read): peekNrd2Pose checks the size against the shape.
struct Nrd2PoseInfo {
    std::string path, scene;
    u32 poseIndex = 0;
    bool heldOut = false;
    u32 stageBVersion = 0, halfW = 0, halfH = 0, tilesX = 0, tilesY = 0, frames = 0;
    u32 crc = 0;   // the stored CRC-32 (identifies the content)
};
bool peekNrd2Pose(const std::string& path, Nrd2PoseInfo& info, std::string* why = nullptr);
// pose_*.n2p in each dir and in its immediate subdirectories, sorted by (scene, pose index, path). Files
// that will not peek, or carry another Stage B version, are skipped with a line in `problems`.
std::vector<Nrd2PoseInfo> scanNrd2Dataset(const std::vector<std::string>& dirs, std::vector<std::string>* problems = nullptr);
// FNV-1a 64 over (scene, pose index, held-out, CRC) in scan order.
u64 nrd2DatasetId(const std::vector<Nrd2PoseInfo>& poses);
// Scenes without a held-out pose but with 2 or more poses hold out every 4th (index % 4 == 3); returns
// how many were promoted.
u32 nrd2EnsureHeldOut(std::vector<Nrd2PoseInfo>& poses);

// Deterministic stratified sampling: scenes take turns record by record (counter * n + i over the scenes
// with resident poses); pose, frame and core origin from a hash of (seed, counter, i). Core origins keep
// the 8x8 core inside the grid (tx0 = gx0 - 3, gx0 in [0, tilesX - 8]).
void nrd2SampleBatch(u64 seed, u64 counter, const std::vector<std::vector<u32>>& residentByScene,
                     const std::vector<Nrd2PoseInfo>& poses, std::span<Nrd2PatchRef> out);
// Validation records of one pose: cores tile the grid exactly once (tx0 = -3 + 8 i). cap > 0 keeps an
// evenly spaced subset of at most cap.
std::vector<Nrd2PatchRef> nrd2ValidationPatches(u32 pose, u32 tilesX, u32 tilesY, u32 frame, u32 cap = 0);
// sum w (d' - t')^2 over one record's 12 x 14 x 14 target and weight, d' the standardised defaults.
f64 nrd2DefaultLoss(const f32 defStd[12], const f32* target, const f32* weight);

f32 nrd2LearningRate(u64 lifetimeSteps);   // 5e-4 / (1 + steps / 1000), floored at 2e-5 (NeuraFI's)
inline constexpr f32 kNrd2GateOpen  = 0.8f;   // held-out val/default at or below: the network runs
inline constexpr f32 kNrd2GateClose = 0.9f;   // above: off (between: unchanged)
bool nrd2GateOpen(f32 ratio, bool wasOpen);

// "<path>.steps" beside a weight file: one text line "lifetimeSteps valRatio datasetIdHex bestRatio
// evalsSinceBest" (ratio < 0 = not measured). NeuraFI-style; older or shorter lines read what they have.
struct Nrd2Sidecar {
    u64 lifetimeSteps = 0;
    f32 valRatio = -1.0f;
    u64 datasetId = 0;
    f32 bestRatio = -1.0f;
    u32 evalsSinceBest = 0;
};
bool writeNrd2Sidecar(const std::string& path, const Nrd2Sidecar& s);
bool readNrd2Sidecar(const std::string& path, Nrd2Sidecar& s);
// ".../nrd2_v1.avnn" -> ".../nrd2_v1.last.avnn".
std::string nrd2LastPath(const std::string& weightsPath);

// ---- the GPU session

struct Nrd2TrainConfig {
    std::vector<std::string> dirs;   // dataset roots (scene subfolders or pose files)
    std::string weightsPath;         // best weights; "<name>.last.avnn" beside it for resuming
    u32 steps = 30000;               // steps this session (early stop may end it sooner)
    u32 batch = 32;
    u32 validateEvery = 250, saveEvery = 500, patience = 8;
    u32 swapEvery = 62;              // streaming: one resident pose replaced per this many steps
    u64 vramBudget = 1536ull << 20;  // pose cache (training + validation slots)
    f32 gpuBudgetMs = 8.0f;          // training + validation GPU time per frame
    u32 maxStepsPerFrame = 16;
    u32 valPatchCap = 0;             // validation records per held-out pose (0 = all)
    bool resume = true;
    u64 seed = 1;
};

struct Nrd2SceneRatio {
    std::string scene;
    f64 net = 0.0, def = 0.0;   // held-out weighted squared error, network vs defaults (standardised)
    f32 ratio = -1.0f;
    u32 poses = 0;
};

struct Nrd2TrainStatus {
    enum class Phase : u8 { Idle, Loading, Training, Validating, Stopping, Finished, Failed, Cancelled };
    Phase phase = Phase::Idle;
    std::string message;              // current activity or why it ended
    u64 lifetimeSteps = 0;
    u32 sessionSteps = 0, targetSteps = 0;
    f32 learningRate = 0.0f;
    f32 loss = -1.0f;                 // training batch weighted MSE (standardised), last measured
    f32 lastRatio = -1.0f, bestRatio = -1.0f;
    u32 evaluations = 0, sinceBest = 0;
    std::vector<Nrd2SceneRatio> scenes;
    u32 trainPoses = 0, heldOutPoses = 0, residentPoses = 0;
    f32 stepsPerFrame = 0.0f;
    f32 progress = 0.0f;              // 0..1 of this session
};

class Nrd2Trainer {
public:
    explicit Nrd2Trainer(rhi::IDevice& dev);
    ~Nrd2Trainer();
    Nrd2Trainer(const Nrd2Trainer&)            = delete;
    Nrd2Trainer& operator=(const Nrd2Trainer&) = delete;

    // False when a session is running or the config is unusable (no dirs, no weights path).
    bool start(const Nrd2TrainConfig& cfg);
    // Saves "<name>.last.avnn" and stops (a few frames).
    void cancel();
    // Once per frame, outside any render pass (an IRenderFeature::prePass). GPU span "NRD2 training".
    void step(rhi::IRenderContext& ctx);
    [[nodiscard]] bool active() const;
    [[nodiscard]] Nrd2TrainStatus status() const;
    // At shutdown: writes the last read-back weights as .last when they are newer than the saved one.
    void saveOnExit();

    static constexpr u32 kReadbackDelay = 4;   // frames before a readback is read (2 in flight)

private:
    struct Slot;
    struct Packed;
    struct Pending;
    enum class Mode : u8 { Train, Validate };

    void worker();
    bool prepare(std::string& why);          // worker: scan, split, resume, statistics, slot sizes
    std::unique_ptr<Packed> pack(u32 pose, bool validation, std::string& why);
    void requestLoad(u32 pose);
    std::unique_ptr<Packed> takeLoaded();

    bool setupGpu(std::string& why);
    bool upload(rhi::IRenderContext& ctx, Slot& slot, const Packed& p);
    void gather(rhi::IRenderContext& ctx, const Slot& slot, const Nrd2PatchRef& r, u32 record);
    void gatherBatch(rhi::IRenderContext& ctx, const std::vector<std::pair<const Slot*, Nrd2PatchRef>>& items);
    bool trainStep(rhi::IRenderContext& ctx);
    void startValidation(rhi::IRenderContext& ctx);
    void validateBatch(rhi::IRenderContext& ctx);
    void collect();
    void finishValidation(const std::vector<f32>& losses);
    bool saveWeights(const std::string& path, bool ema, const Nrd2Sidecar& sc);
    void measureGpu();
    void end(Nrd2TrainStatus::Phase phase, std::string why);
    void releaseGpu();
    void setMessage(std::string m);

    rhi::IDevice&          dev_;
    rhi::IResourceFactory* res_ = nullptr;
    Nrd2TrainConfig cfg_;

    // Shared with the worker (mutex_).
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
    bool stopWorker_ = false, prepared_ = false, prepareFailed_ = false, cancel_ = false;
    std::deque<u32> requests_;
    std::deque<std::unique_ptr<Packed>> loaded_;
    Nrd2TrainStatus status_;

    // Written by prepare() before prepared_, read-only afterwards.
    std::vector<Nrd2PoseInfo> poses_;
    std::vector<std::string> scenes_;
    std::vector<u32> sceneOf_;                // per pose
    std::vector<u32> train_, held_;           // pose ids; train_ in stratified rotation order
    u64 datasetId_ = 0;
    Nrd2Standardisation std_{};
    neural::ConvIoAffine io_;
    f32 tScale_[12] = {}, tBias_[12] = {}, defStd_[12] = {};
    std::vector<f32> resumeWeights_;
    Nrd2Sidecar resume_{};
    u64 trainSlotBytes_ = 0, valSlotBytes_ = 0;
    u32 trainSlots_ = 0;

    // Frame thread only.
    bool gpuReady_ = false, stopping_ = false;
    neural::ConvNet net_;
    rhi::PipelineHandle gatherPso_ = 0;
    std::vector<Slot> slots_, valSlots_;
    std::vector<std::vector<Nrd2PatchRef>> valRefs_;   // per held-out slot
    std::vector<f64> valDefault_, valWeight_;          // per held-out slot: default loss, weight sum
    rhi::BufferHandle batchIn_ = 0, batchTgt_ = 0, batchW_ = 0, lossBuf_ = 0, valRb_ = 0;
    rhi::BufferHandle staging_[3] = {};
    u64 stagingBytes_ = 0;
    u64 frame_ = 0;
    u32 rotation_ = 0, fifo_ = 0, valUploaded_ = 0, trainFilled_ = 0;
    u32 sessionSteps_ = 0, lastValidated_ = ~0u, lastSwapped_ = ~0u, lastSavedStep_ = 0;
    u64 sampleCounter_ = 0;
    std::vector<Pending> pending_;
    Mode mode_ = Mode::Train;
    u32 valNext_ = 0, valTotal_ = 0;
    std::vector<std::pair<u32, u32>> valWork_;   // (held-out slot, record) in order
    u64 valDue_ = 0, weightsDue_ = 0, weightsStep_ = 0;
    std::vector<f32> lastWeights_;               // master weights of the last readback
    u64 lastWeightsStep_ = 0;
    f64 msPerUnit_ = 0.0, lastTimingSum_ = 0.0, unitsRecorded_ = 0.0, lastUnits_ = 0.0;
    u32 lastTimingFrames_ = 0;
    bool timingOk_ = false;
};

}  // namespace aver::render::denoise
