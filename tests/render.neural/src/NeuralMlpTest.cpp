// NeuralMlpTest -- Aver.Render.Neural's CPU reference, with no GPU and no device.
//
// The GPU network (Mlp, shaders/aver_neural_mlp.hlsl) cannot run in this suite, so the maths it is
// written to reproduce lives in MlpReference and the properties are checked there:
//   * the backward pass against finite differences, for every activation and both losses;
//   * a toy function (sin of a 2-D input) being learned, under both losses, with the EMA following;
//   * Adam's bias-corrected first step;
//   * deterministic init, the He bound, and the weight file's round trip and rejections;
//   * the fixed-point gradient quantisation, and that accumulation order does not change the result
//     -- the property that makes the GPU's grouped atomics deterministic.
// Output follows the suite's convention (see UiRenderTest): "  FAIL  ..." per failed assertion and
// "=== N FAILED ===" at the end; exit code = failure count != 0.
#include "aver/render/neural/MlpReference.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::render::neural;

static int g_failures = 0;

// Records one assertion. Counts a failure and logs it when the condition is false.
static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

namespace {

// A tiny deterministic generator for test data (xorshift32): the tests must not depend on the
// standard library's distributions, whose output differs between implementations.
struct Rng {
    u32 s;
    explicit Rng(u32 seed) : s(seed ? seed : 1u) {}
    u32 next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    f32 uniform() { return static_cast<f32>(next() >> 8) * (1.0f / 16777216.0f); }       // [0, 1)
    f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * uniform(); }
};

const char* actName(Activation a) {
    switch (a) {
        case Activation::None:    return "none";
        case Activation::ReLU:    return "relu";
        case Activation::Sigmoid: return "sigmoid";
        case Activation::Exp:     return "exp";
    }
    return "?";
}

// ---------------------------------------------------------------- validation and layout

void testValidationAndLayout() {
    AVER_INFO("-- validation and layout");
    MlpDesc d;
    d.inputs = 3; d.outputs = 2; d.hiddenWidth = 8; d.hiddenLayers = 2;
    check(validate(d), "a 3 -> 8x2 -> 2 network is valid");

    auto bad = [&](auto mutate, const char* what) {
        MlpDesc m = d;
        mutate(m);
        std::string why;
        check(!validate(m, &why) && !why.empty(), std::string("rejected: ") + what);
    };
    bad([](MlpDesc& m) { m.hiddenWidth = 6; },  "width not a multiple of 4");
    bad([](MlpDesc& m) { m.hiddenWidth = 0; },  "width 0");
    bad([](MlpDesc& m) { m.hiddenWidth = 68; }, "width over 64");
    bad([](MlpDesc& m) { m.hiddenLayers = 0; }, "no hidden layers");
    bad([](MlpDesc& m) { m.hiddenLayers = 7; }, "more than 6 hidden layers");
    bad([](MlpDesc& m) { m.inputs = 0; },       "no inputs");
    bad([](MlpDesc& m) { m.inputs = 65; },      "more than 64 inputs");
    bad([](MlpDesc& m) { m.outputs = 0; },      "no outputs");
    bad([](MlpDesc& m) { m.outputs = 17; },     "more than 16 outputs");
    MlpDesc edge; edge.inputs = 64; edge.outputs = 16; edge.hiddenWidth = 64; edge.hiddenLayers = 6;
    check(validate(edge), "the largest network the kernels support is valid");
    MlpDesc small; small.hiddenWidth = 4;
    check(validate(small), "width 4 is valid");

    OptimiserDesc o;
    check(validate(o), "default optimiser is valid");
    OptimiserDesc o2 = o; o2.gradFixedScale = 0.0f;
    check(!validate(o2), "zero gradFixedScale rejected");
    OptimiserDesc o3 = o; o3.learningRate = -1.0f;
    check(!validate(o3), "negative learning rate rejected");

    const MlpLayout L = MlpLayout::make(d);   // 3 -> 8 -> 8 -> 2
    check(L.layers == 3, "layer count is hiddenLayers + 1");
    check(L.layerSize[0] == 8 * 3 + 8 && L.layerSize[1] == 8 * 8 + 8 && L.layerSize[2] == 2 * 8 + 2,
          "layer slices hold W then b");
    check(L.total == 122, "weightCount 122 for 3 -> 8x2 -> 2 with biases");
    check(L.wOffset[1] == 32 && L.bOffset[1] == 32 + 64 && L.wOffset[2] == 104, "offsets follow the flat order");
    check(L.actTotal == 3 + 8 + 8 + 2 && L.actOffset[1] == 3 && L.actOffset[2] == 11 && L.actOffset[3] == 19,
          "activation vector: input, then each layer's output");
    MlpDesc nb = d; nb.bias = false;
    check(MlpLayout::make(nb).total == 24 + 64 + 16, "weightCount without biases");
}

// ---------------------------------------------------------------- scalar maths

void testActivations() {
    AVER_INFO("-- activations and losses");
    check(activate(Activation::None, -2.5f) == -2.5f, "identity");
    check(activate(Activation::ReLU, -1.0f) == 0.0f && activate(Activation::ReLU, 2.0f) == 2.0f, "relu");
    check(std::fabs(activate(Activation::Sigmoid, 0.0f) - 0.5f) < 1e-6f, "sigmoid(0) = 0.5");
    check(std::fabs(activate(Activation::Exp, 1.0f) - std::exp(1.0f)) < 1e-5f, "exp(1)");
    check(std::isfinite(activate(Activation::Exp, 1.0e6f)), "exp is clamped against overflow");
    check(activationDerivative(Activation::ReLU, 0.0f) == 0.0f && activationDerivative(Activation::ReLU, 3.0f) == 1.0f,
          "relu derivative from the output");
    check(std::fabs(activationDerivative(Activation::Sigmoid, 0.5f) - 0.25f) < 1e-7f, "sigmoid derivative");
    check(lossValue(Loss::L2, 3.0f, 1.0f) == 4.0f, "L2 loss");
    check(std::fabs(lossValue(Loss::RelativeL2, 3.0f, 1.0f) - 4.0f / 9.01f) < 1e-6f, "relative L2 loss");
    check(std::fabs(lossGradient(Loss::RelativeL2, 3.0f, 1.0f) - 4.0f / 9.01f) < 1e-6f,
          "relative L2 gradient holds the denominator constant");
}

// ---------------------------------------------------------------- finite-difference gradient check

// Compares MlpReference::backward with central differences of the loss, for one configuration.
// For RelativeL2 the numeric loss holds the denominator at its value at the unperturbed point --
// that is what "stopgrad" means, and it is what backward() differentiates.
void gradientCheck(Activation hidden, Activation output, Loss loss) {
    MlpDesc d;
    d.inputs = 3; d.outputs = 2; d.hiddenWidth = 8; d.hiddenLayers = 2;
    d.hidden = hidden; d.output = output; d.seed = 11;
    OptimiserDesc o; o.loss = loss;
    MlpReference ref(d, o);

    // Shrink the He init and add biases, so the net sits away from saturation and kinks and every
    // weight (biases included) has a nonzero gradient to check.
    Rng rng(0x1234u + static_cast<u32>(hidden) * 17u + static_cast<u32>(output));
    std::vector<f32> w = ref.weights();
    for (f32& x : w) x = 0.5f * x + rng.range(-0.2f, 0.2f);
    // A ReLU hidden layer can come out mostly dead for an unlucky draw (measured: hidden relu + output
    // sigmoid at this seed left fewer than a quarter of the gradients nonzero, with 0 outliers among
    // them), which says nothing about backward(). Lift the hidden biases so the units sit on the
    // active side of the kink; the check is about the maths, not about initialisation luck.
    if (hidden == Activation::ReLU) {
        const MlpLayout lay = MlpLayout::make(d);
        for (u32 l = 0; l + 1 < lay.layers; ++l)   // hidden layers only; the output layer is not ReLU
            for (u32 j = 0; j < lay.outDim[l]; ++j) w[lay.bOffset[l] + j] += 0.5f;
    }
    ref.setWeights(w);

    const f32 in[3] = {rng.range(-0.8f, 0.8f), rng.range(-0.8f, 0.8f), rng.range(-0.8f, 0.8f)};
    const f32 target[2] = {rng.range(0.1f, 0.9f), rng.range(0.1f, 0.9f)};

    std::vector<f32> grad(ref.weightCount());
    ref.backward(in, target, grad);

    f32 p0[2];
    ref.forward(in, p0);
    f32 denom[2];
    for (u32 c = 0; c < 2; ++c) denom[c] = (loss == Loss::RelativeL2) ? p0[c] * p0[c] + 0.01f : 1.0f;
    auto lossAt = [&]() {
        f32 p[2];
        ref.forward(in, p);
        f32 s = 0.0f;
        for (u32 c = 0; c < 2; ++c) s += (p[c] - target[c]) * (p[c] - target[c]) / denom[c];
        return s;
    };

    f32 gmax = 0.0f;
    for (f32 g : grad) gmax = std::max(gmax, std::fabs(g));
    constexpr f32 eps = 1.0e-3f;
    u32 outliers = 0, nonzero = 0;
    for (u32 k = 0; k < ref.weightCount(); ++k) {
        const f32 saved = ref.weights()[k];
        ref.weights()[k] = saved + eps;
        const f32 lp = lossAt();
        ref.weights()[k] = saved - eps;
        const f32 lm = lossAt();
        ref.weights()[k] = saved;
        const f32 numeric = (lp - lm) / (2.0f * eps);
        const f32 tol = 0.03f * std::max(std::fabs(grad[k]), std::fabs(numeric)) + 1.0e-4f * (1.0f + gmax);
        if (std::fabs(grad[k] - numeric) > tol) ++outliers;
        if (grad[k] != 0.0f) ++nonzero;
    }
    // A ReLU kink within eps of a pre-activation makes the numeric slope wrong for that one weight;
    // a real backward-pass bug breaks nearly all of them. So a small outlier budget, not zero.
    const u32 budget = ref.weightCount() * 3 / 100;
    check(outliers <= budget && nonzero > ref.weightCount() / 4,
          std::string("backward matches finite differences: hidden ") + actName(hidden) + ", output " +
              actName(output) + ", " + (loss == Loss::L2 ? "L2" : "relative L2") + " (" +
              std::to_string(outliers) + " outliers, " + std::to_string(nonzero) + " nonzero, of " +
              std::to_string(ref.weightCount()) + ")");
}

void testGradients() {
    AVER_INFO("-- finite-difference gradient check");
    const Activation acts[4] = {Activation::None, Activation::ReLU, Activation::Sigmoid, Activation::Exp};
    for (Activation h : acts)
        for (Activation out : {Activation::None, Activation::Sigmoid, Activation::Exp})
            for (Loss l : {Loss::L2, Loss::RelativeL2}) gradientCheck(h, out, l);

    // And without biases, which changes the layout the backward pass walks.
    MlpDesc d; d.inputs = 2; d.outputs = 1; d.hiddenWidth = 4; d.hiddenLayers = 1; d.bias = false;
    d.hidden = Activation::Sigmoid; d.seed = 5;
    MlpReference ref(d, OptimiserDesc{});
    const f32 in[2] = {0.3f, -0.6f}, target[1] = {0.7f};
    std::vector<f32> grad(ref.weightCount());
    ref.backward(in, target, grad);
    u32 bad = 0;
    for (u32 k = 0; k < ref.weightCount(); ++k) {
        const f32 saved = ref.weights()[k];
        f32 p[1];
        ref.weights()[k] = saved + 1e-3f; ref.forward(in, p); const f32 lp = (p[0] - target[0]) * (p[0] - target[0]);
        ref.weights()[k] = saved - 1e-3f; ref.forward(in, p); const f32 lm = (p[0] - target[0]) * (p[0] - target[0]);
        ref.weights()[k] = saved;
        if (std::fabs(grad[k] - (lp - lm) / 2e-3f) > 0.03f * std::fabs(grad[k]) + 1e-4f) ++bad;
    }
    check(bad == 0, "backward matches finite differences with bias disabled");
}

// ---------------------------------------------------------------- learning a toy function

f32 toyTarget(f32 x0, f32 x1) { return 0.5f + 0.5f * std::sin(2.5f * x0 + 1.5f * x1); }

void fillToyBatch(Rng& rng, u32 n, std::vector<f32>& rec, std::vector<f32>& tgt) {
    rec.resize(static_cast<usize>(n) * 2);
    tgt.resize(n);
    for (u32 i = 0; i < n; ++i) {
        const f32 x0 = rng.range(-1.0f, 1.0f), x1 = rng.range(-1.0f, 1.0f);
        rec[2 * i] = x0; rec[2 * i + 1] = x1;
        tgt[i] = toyTarget(x0, x1);
    }
}

void testTraining(Loss loss) {
    const char* name = loss == Loss::L2 ? "L2" : "relative L2";
    MlpDesc d;
    d.inputs = 2; d.outputs = 1; d.hiddenWidth = 16; d.hiddenLayers = 2; d.seed = 3;
    OptimiserDesc o;
    o.learningRate = 8e-3f;
    o.loss = loss;
    MlpReference ref(d, o);

    Rng evalRng(99);
    std::vector<f32> evalRec, evalTgt;
    fillToyBatch(evalRng, 512, evalRec, evalTgt);
    const f32 before = ref.evaluate(evalRec, evalTgt, 512);

    Rng rng(7);
    std::vector<f32> rec, tgt;
    bool finite = true;
    for (u32 stepNo = 0; stepNo < 800; ++stepNo) {
        fillToyBatch(rng, 128, rec, tgt);
        finite = finite && std::isfinite(ref.trainBatch(rec, tgt, 128));
    }
    const f32 after = ref.evaluate(evalRec, evalTgt, 512);
    const f32 afterEma = ref.evaluate(evalRec, evalTgt, 512, true);
    AVER_INFO("  toy {}: held-out loss {:.5f} -> {:.5f} (EMA {:.5f})", name, before, after, afterEma);
    check(finite, std::string("training stays finite (") + name + ")");
    check(after * 4.0f < before, std::string("800 steps cut the held-out loss at least 4x (") + name + ")");
    check(afterEma * 3.0f < before, std::string("the EMA weights learned it too (") + name + ")");
    check(ref.step() == 800, std::string("Adam counted 800 steps (") + name + ")");

    bool differ = false;
    for (u32 k = 0; k < ref.weightCount() && !differ; ++k) differ = ref.ema()[k] != ref.weights()[k];
    check(differ, std::string("EMA lags the master weights (") + name + ")");
}

// ---------------------------------------------------------------- Adam

void testAdamFirstStep() {
    AVER_INFO("-- Adam");
    MlpDesc d; d.inputs = 2; d.outputs = 1; d.hiddenWidth = 8; d.hiddenLayers = 1; d.seed = 21;
    OptimiserDesc o; o.learningRate = 1e-2f;
    MlpReference ref(d, o);
    Rng rng(5);
    std::vector<f32> rec, tgt;
    fillToyBatch(rng, 64, rec, tgt);
    for (u32 i = 0; i < 64; ++i)
        ref.accumulateRecord(std::span<const f32>(rec).subspan(2 * i, 2), std::span<const f32>(tgt).subspan(i, 1));
    const std::vector<i32> acc = ref.accumulator();
    const std::vector<f32> w0 = ref.weights();
    ref.adamStep(64);

    // Bias-corrected Adam's first update is mhat / (sqrt(vhat) + eps) = g / (|g| + eps): a full
    // learning-rate step against the gradient's sign, whatever the gradient's size. (Weights with a
    // tiny accumulator are skipped: eps is then no longer negligible against |g|.)
    u32 checked = 0, wrong = 0;
    for (u32 k = 0; k < ref.weightCount(); ++k) {
        if (std::abs(acc[k]) < 4000) continue;
        ++checked;
        const f32 delta = ref.weights()[k] - w0[k];
        const f32 expected = -o.learningRate * (acc[k] > 0 ? 1.0f : -1.0f);
        if (std::fabs(delta - expected) > 0.01f * o.learningRate) ++wrong;
    }
    check(checked >= 10, "enough weights carried a real gradient to test (" + std::to_string(checked) + ")");
    check(wrong == 0, "Adam's first step is learningRate against the gradient sign");
    bool cleared = true;
    for (i32 a : ref.accumulator()) cleared = cleared && a == 0;
    check(cleared, "the step clears the accumulator");
    check(ref.step() == 1, "step counter advanced");

    // A zero live count changes nothing but the clear.
    const std::vector<f32> wBefore = ref.weights();
    ref.accumulateRecord(std::span<const f32>(rec).subspan(0, 2), std::span<const f32>(tgt).subspan(0, 1));
    ref.adamStep(0);
    check(ref.weights() == wBefore && ref.step() == 1, "a zero-count step leaves weights and step alone");
    check(ref.accumulator() == std::vector<i32>(ref.weightCount(), 0), "and still clears the accumulator");

    check(adamBiasCorrection(0.9f, 1) == static_cast<f32>(1.0 - 0.9f) || std::fabs(adamBiasCorrection(0.9f, 1) - 0.1f) < 1e-6f,
          "bias correction at step 1 is 1 - beta");
}

// ---------------------------------------------------------------- init, determinism, file

void testInit() {
    AVER_INFO("-- init");
    MlpDesc d; d.inputs = 4; d.outputs = 3; d.hiddenWidth = 64; d.hiddenLayers = 2; d.seed = 42;
    const std::vector<f32> a = initWeights(d), b = initWeights(d);
    check(a.size() == MlpLayout::make(d).total, "init produces weightCount floats");
    check(a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(f32)) == 0,
          "same seed gives bit-identical weights");
    MlpDesc d2 = d; d2.seed = 43;
    check(initWeights(d2) != a, "a different seed gives different weights");
    MlpReference r1(d, OptimiserDesc{}), r2(d, OptimiserDesc{});
    check(r1.weights() == r2.weights() && r1.weights() == a, "MlpReference starts from initWeights");
    check(r1.ema() == r1.weights(), "EMA starts equal to the master weights");

    const MlpLayout L = MlpLayout::make(d);
    bool inBound = true, biasZero = true;
    for (u32 l = 0; l < L.layers; ++l) {
        const f32 bound = std::sqrt(6.0f / static_cast<f32>(L.inDim[l]));
        f32 mx = 0.0f, sum = 0.0f;
        for (u32 k = 0; k < L.outDim[l] * L.inDim[l]; ++k) {
            const f32 w = a[L.wOffset[l] + k];
            inBound = inBound && std::fabs(w) <= bound;
            mx = std::max(mx, std::fabs(w));
            sum += w;
        }
        for (u32 k = 0; k < L.outDim[l]; ++k) biasZero = biasZero && a[L.bOffset[l] + k] == 0.0f;
        const f32 n = static_cast<f32>(L.outDim[l] * L.inDim[l]);
        check(mx > 0.6f * bound && std::fabs(sum / n) < 0.15f * bound,
              "layer " + std::to_string(l) + " weights spread over the He range, centred on zero");
    }
    check(inBound, "every weight is inside its layer's He bound sqrt(6 / fan_in)");
    check(biasZero, "biases initialise to zero");
}

std::vector<char> slurp(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
void spit(const std::filesystem::path& p, const std::vector<char>& bytes) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void testWeightFile() {
    AVER_INFO("-- weight file");
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "aver_neural_mlp_test.avnn";

    MlpDesc d; d.inputs = 5; d.outputs = 3; d.hiddenWidth = 12; d.hiddenLayers = 3;
    d.hidden = Activation::Sigmoid; d.output = Activation::Exp; d.bias = true; d.seed = 9;
    std::vector<f32> w = initWeights(d);
    Rng rng(3);
    for (f32& x : w) x += 0.01f * rng.uniform();   // make biases nonzero too

    check(saveWeightFile(path.string(), d, w), "save succeeds");
    {
        const std::vector<char> bytes = slurp(path);
        u32 magic = 0, version = 0;
        std::memcpy(&magic, bytes.data(), 4);
        std::memcpy(&version, bytes.data() + 4, 4);
        check(bytes.size() == 40 + w.size() * 4, "file size is a 40-byte header plus the weights");
        check(bytes[0] == 'A' && bytes[1] == 'V' && bytes[2] == 'N' && bytes[3] == 'N', "magic bytes are AVNN");
        check(magic == kWeightFileMagic && version == 1, "magic and version words");
    }

    MlpDesc back; back.seed = 77;
    std::vector<f32> wBack;
    check(loadWeightFile(path.string(), back, wBack), "load succeeds");
    check(back.inputs == d.inputs && back.outputs == d.outputs && back.hiddenWidth == d.hiddenWidth &&
              back.hiddenLayers == d.hiddenLayers && back.hidden == d.hidden && back.output == d.output &&
              back.bias == d.bias,
          "the descriptor round-trips");
    check(back.seed == 77, "the seed is not stored: the caller's survives a load");
    check(wBack.size() == w.size() && std::memcmp(wBack.data(), w.data(), w.size() * sizeof(f32)) == 0,
          "the weights round-trip bit for bit");

    // A bias-free network round-trips too.
    MlpDesc nb = d; nb.bias = false;
    std::vector<f32> wnb = initWeights(nb);
    check(saveWeightFile(path.string(), nb, wnb) && loadWeightFile(path.string(), back, wBack) &&
              !back.bias && wBack == wnb,
          "a bias-free network round-trips");

    // Rejections.
    check(saveWeightFile(path.string(), d, w), "re-save for the rejection cases");
    const std::vector<char> good = slurp(path);
    auto rejects = [&](std::vector<char> bytes, const char* what) {
        spit(path, bytes);
        MlpDesc x; std::vector<f32> y;
        check(!loadWeightFile(path.string(), x, y), std::string("load rejects ") + what);
    };
    { auto b = good; b[0] = 'X'; rejects(b, "a wrong magic"); }
    { auto b = good; b[4] = 2; rejects(b, "an unknown version"); }
    { auto b = good; b.resize(b.size() - 4); rejects(b, "a truncated file"); }
    { auto b = good; b.push_back(0); rejects(b, "trailing bytes"); }
    { auto b = good; b.resize(20); rejects(b, "a truncated header"); }
    { auto b = good; b[36] ^= 1; rejects(b, "a weight count that disagrees with the shape"); }
    { auto b = good; b[8] = 0; b[9] = 0; b[10] = 0; b[11] = 0; rejects(b, "an invalid shape (0 inputs)"); }
    MlpDesc x; std::vector<f32> y;
    check(!loadWeightFile((fs::temp_directory_path() / "aver_neural_missing.avnn").string(), x, y),
          "load of a missing file fails");
    check(!saveWeightFile(path.string(), d, std::span<const f32>(w).subspan(1)), "save refuses a wrong-sized weight array");
    std::error_code ec;
    fs::remove(path, ec);
}

// ---------------------------------------------------------------- fixed point

void testFixedPoint() {
    AVER_INFO("-- fixed-point gradient accumulation");
    const OptimiserDesc o;   // clamp 16, scale 65536
    check(MlpReference::quantise(1.0f, o) == 65536, "1.0 quantises to the scale");
    check(MlpReference::quantise(1.0e9f, o) == 16 * 65536 && MlpReference::quantise(-1.0e9f, o) == -16 * 65536,
          "values past the clamp saturate at +-clamp * scale");
    check(MlpReference::quantise(0.5f / 65536.0f, o) == 0, "less than one step quantises to 0");
    check(MlpReference::quantise(1.9f / 65536.0f, o) == 1 && MlpReference::quantise(-1.9f / 65536.0f, o) == -1,
          "truncation is toward zero, both signs");
    check(MlpReference::quantise(std::numeric_limits<f32>::quiet_NaN(), o) == 0, "NaN quantises to 0");
    check(safeBatchLimit(o) == 2047, "worst-case safe batch at the defaults is 2047");
    OptimiserDesc fine = o; fine.gradFixedScale = 4096.0f;
    check(safeBatchLimit(fine) == 32767, "a smaller scale buys proportionally more headroom");

    MlpDesc d; d.inputs = 3; d.outputs = 2; d.hiddenWidth = 8; d.hiddenLayers = 2; d.seed = 17;
    MlpReference base(d, o);

    constexpr u32 n = 300;
    Rng rng(31);
    std::vector<f32> rec(n * 3), tgt(n * 2);
    for (f32& x : rec) x = rng.range(-1.0f, 1.0f);
    for (f32& x : tgt) x = rng.range(0.0f, 1.0f);
    auto feed = [&](MlpReference& r, const std::vector<u32>& order) {
        for (u32 i : order)
            r.accumulateRecord(std::span<const f32>(rec).subspan(i * 3, 3), std::span<const f32>(tgt).subspan(i * 2, 2));
    };

    std::vector<u32> forward(n), reverse(n), shuffled(n);
    for (u32 i = 0; i < n; ++i) { forward[i] = i; reverse[i] = n - 1 - i; shuffled[i] = i; }
    std::mt19937 gen(1234);
    std::shuffle(shuffled.begin(), shuffled.end(), gen);

    MlpReference a = base, b = base, c = base;
    feed(a, forward);
    feed(b, reverse);
    feed(c, shuffled);
    bool anyNonzero = false;
    for (i32 v : a.accumulator()) anyNonzero = anyNonzero || v != 0;
    check(anyNonzero, "the accumulator holds gradient");
    check(a.accumulator() == b.accumulator(), "forward and reversed record order give identical accumulators");
    check(a.accumulator() == c.accumulator(), "a shuffled record order gives the identical accumulator");

    // The GPU's structure: sum within groups of 64 records, then add the group totals. Integer
    // addition makes that grouping exact.
    std::vector<u32> total(base.weightCount(), 0u);
    for (u32 g0 = 0; g0 < n; g0 += 64) {
        MlpReference group = base;
        std::vector<u32> order;
        for (u32 i = g0; i < std::min(n, g0 + 64); ++i) order.push_back(i);
        feed(group, order);
        for (u32 k = 0; k < base.weightCount(); ++k) total[k] += static_cast<u32>(group.accumulator()[k]);
    }
    bool same = true;
    for (u32 k = 0; k < base.weightCount(); ++k) same = same && static_cast<i32>(total[k]) == a.accumulator()[k];
    check(same, "per-group partial sums, then summed, equal the sequential sum exactly");

    // And the whole training step is therefore order-independent.
    a.adamStep(n);
    b.adamStep(n);
    c.adamStep(n);
    check(a.weights() == b.weights() && a.weights() == c.weights() && a.ema() == c.ema(),
          "an optimiser step after any order gives bit-identical weights");

    // setWeights restarts everything.
    MlpReference r = base;
    feed(r, forward);
    r.adamStep(n);
    check(r.setWeights(base.weights()) && r.step() == 0 && r.weights() == base.weights() &&
              r.moment1() == base.moment1() && r.accumulator() == base.accumulator(),
          "setWeights resets weights, EMA, moments, accumulator and step");
    check(!r.setWeights(std::span<const f32>(base.weights()).subspan(1)), "setWeights refuses a wrong-sized array");
}

}  // namespace

int main() {
    testValidationAndLayout();
    testActivations();
    testGradients();
    AVER_INFO("-- training a toy function");
    testTraining(Loss::L2);
    testTraining(Loss::RelativeL2);
    testAdamFirstStep();
    testInit();
    testWeightFile();
    testFixedPoint();

    if (g_failures == 0) AVER_INFO("=== all neural MLP tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
