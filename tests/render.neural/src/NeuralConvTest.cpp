// NeuralConvTest: the convolution layers' CPU reference (ConvNetReference, the spec for
// aver_neural_conv.hlsl), the shared optimiser, and the AVNN v2 file. Headless: no GPU, no device.
//
#include "aver/render/neural/ConvNetReference.hpp"
#include "aver/render/neural/MlpReference.hpp"
#include "aver/render/neural/NeuralOptimiser.hpp"
#include "aver/render/neural/WeightFile.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
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

// Deterministic RNG (xorshift32): tests must not depend on stdlib distributions.
struct Rng {
    u32 s;
    explicit Rng(u32 seed) : s(seed ? seed : 1u) {}
    u32 next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    f32 uniform() { return static_cast<f32>(next() >> 8) * (1.0f / 16777216.0f); }       // [0, 1)
    f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * uniform(); }
};

ConvLayerDesc layer(u32 cin, u32 cout, u32 k, u32 s, Activation a = Activation::ReLU, bool bias = true) {
    ConvLayerDesc l;
    l.cin = cin; l.cout = cout; l.kernel = k; l.stride = s; l.act = a; l.bias = bias;
    return l;
}

ConvNetDesc net(u32 inChannels, std::vector<ConvLayerDesc> layers, u32 seed = 1) {
    ConvNetDesc d;
    d.inChannels = inChannels;
    d.layers = std::move(layers);
    d.seed = seed;
    return d;
}

std::vector<f32> randomTensor(Rng& rng, usize count, f32 lo, f32 hi) {
    std::vector<f32> v(count);
    for (f32& x : v) x = rng.range(lo, hi);
    return v;
}

// Random weights everywhere (head and biases too); ReLU biases are lifted so units sit on the active side
// of the kink, which keeps finite differences honest.
void randomise(ConvNetReference& n, Rng& rng, f32 scale) {
    std::vector<f32> w = n.weights();
    for (f32& x : w) x = rng.range(-scale, scale);
    const ConvLayout& L = n.layout();
    for (u32 l = 0; l < L.layers; ++l)
        if (n.desc().layers[l].act == Activation::ReLU && n.desc().layers[l].bias)
            for (u32 co = 0; co < L.cout[l]; ++co) w[L.bOffset[l] + co] += 0.3f;
    n.setWeights(w);
}

bool bitEqual(const std::vector<f32>& a, const std::vector<f32>& b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(f32)) == 0);
}

// ---------------------------------------------------------------- validation and layout

void testValidation() {
    AVER_INFO("-- validation");
    const ConvNetDesc good = net(3, {layer(3, 8, 3, 2), layer(8, 8, 3, 1), layer(8, 4, 1, 1, Activation::None)});
    check(validate(good), "a 3 -> 8 -> 8 -> 4 network (3 input channels) is valid");

    auto bad = [&](auto mutate, const char* what) {
        ConvNetDesc m = good;
        mutate(m);
        std::string why;
        check(!validate(m, &why) && !why.empty(), std::string("rejected: ") + what);
    };
    bad([](ConvNetDesc& m) { m.layers.clear(); },                         "no layers");
    bad([](ConvNetDesc& m) { m.layers.assign(9, layer(8, 8, 3, 1)); m.layers[0].cin = 3; }, "more than 8 layers");
    bad([](ConvNetDesc& m) { m.inChannels = 0; },                         "no input channels");
    bad([](ConvNetDesc& m) { m.inChannels = 65; m.layers[0].cin = 65; },  "more than 64 input channels");
    bad([](ConvNetDesc& m) { m.layers[1].cin = 4; },                      "cin chain broken");
    bad([](ConvNetDesc& m) { m.layers[0].cin = 4; },                      "first cin differs from inChannels");
    bad([](ConvNetDesc& m) { m.layers[0].cout = 6; m.layers[1].cin = 6; }, "cout not a multiple of 4");
    bad([](ConvNetDesc& m) { m.layers[2].cout = 0; },                     "cout 0");
    bad([](ConvNetDesc& m) { m.layers[1].cout = 68; m.layers[2].cin = 68; }, "cout over 64");
    bad([](ConvNetDesc& m) { m.layers[0].kernel = 5; },                   "kernel 5");
    bad([](ConvNetDesc& m) { m.layers[0].kernel = 0; },                   "kernel 0");
    bad([](ConvNetDesc& m) { m.layers[2].stride = 2; },                   "1x1 with stride 2");
    bad([](ConvNetDesc& m) { m.layers[0].stride = 3; },                   "stride 3");
    bad([](ConvNetDesc& m) { m.layers[0].stride = 0; },                   "stride 0");
    bad([](ConvNetDesc& m) { m.layers[1].act = Activation::Sigmoid; },    "sigmoid activation");
    bad([](ConvNetDesc& m) { m.layers[1].act = Activation::Exp; },        "exp activation");
    ConvNetDesc wide = net(64, {layer(64, 64, 3, 1), layer(64, 64, 3, 2), layer(64, 4, 1, 1, Activation::None)});
    check(validate(wide), "the widest supported network (64 channels, 3x3) is valid");
    check(validate(net(1, std::vector<ConvLayerDesc>(8, layer(1, 4, 3, 1)))) == false,
          "8 layers with a broken chain is rejected");
    ConvNetDesc eight = net(5, {layer(5, 4, 3, 1)});
    for (int i = 0; i < 7; ++i) eight.layers.push_back(layer(4, 4, 3, 1));
    check(validate(eight), "8 layers is valid");

    check(convSharedBytes(layer(64, 64, 3, 1)) == 4608, "convSharedBytes caps the input chunk at 16 channels (4608)");
    check(convSharedBytes(layer(4, 4, 1, 1)) == 128 && convSharedBytes(layer(3, 8, 3, 2)) == 4 * 8 * 3 * 9,
          "convSharedBytes = 4 * 8 * min(cin, 16) * k * k");
    check(convSharedBytes(layer(64, 64, 3, 1)) <= kConvSharedLimitBytes, "every supported layer fits 16 KB");

    const OptimiserDesc cd = convDefaults();
    check(validate(cd) && cd.gradFixedScale == 16777216.0f && cd.gradClamp == 32.0f,
          "convDefaults: scale 2^24, clamp 32, accepted by validate(OptimiserDesc)");
    check(safeBatchLimit(cd) == 3, "worst-case safe batch at the conv defaults is 3 (partials, not records, carry the sum)");

    ConvNetReference invalid(net(3, {layer(4, 8, 3, 1)}), cd);
    check(!invalid.valid() && invalid.weightCount() == 0, "an invalid descriptor gives an empty, invalid network");
    std::vector<f32> in(27, 1.0f), out(27, 5.0f);
    invalid.forward(TensorShape{1, 3, 3, 3}, in, out);
    check(out[0] == 5.0f, "forward on an invalid network does nothing");
}

void testLayout() {
    AVER_INFO("-- layout, outDims, init");
    const ConvNetDesc d = net(3, {layer(3, 8, 3, 2), layer(8, 8, 3, 1), layer(8, 4, 1, 1, Activation::None, false)}, 5);
    const ConvLayout L = ConvLayout::make(d);
    check(L.layers == 3, "layer count");
    check(L.layerSize[0] == 8 * 3 * 9 + 8 && L.layerSize[1] == 8 * 8 * 9 + 8 && L.layerSize[2] == 4 * 8,
          "layer slices hold W then b (b omitted when bias is off)");
    check(L.total == 224 + 584 + 32, "weightCount 840");
    check(L.wOffset[0] == 0 && L.bOffset[0] == 216 && L.wOffset[1] == 224 && L.bOffset[1] == 224 + 576 &&
              L.wOffset[2] == 808 && L.bOffset[2] == 840,
          "offsets follow the flat OIHW-then-bias order");
    check(L.cin[1] == 8 && L.cout[2] == 4 && L.kernel[2] == 1 && L.stride[0] == 2, "per-layer shape arrays");

    // outDims: ceil(h / stride), including odd sizes and 1x1.
    const TensorShape a = L.outDims(0, TensorShape{2, 3, 13, 7});
    check(a == TensorShape{2, 8, 7, 4}, "13x7 through stride 2 gives 7x4");
    check(L.outDims(1, a) == TensorShape{2, 8, 7, 4}, "stride 1 keeps the size");
    check(L.outDims(0, TensorShape{1, 3, 1, 1}) == TensorShape{1, 8, 1, 1}, "1x1 through stride 2 stays 1x1");
    check(L.outDims(0, TensorShape{1, 3, 2, 5}) == TensorShape{1, 8, 1, 3}, "2x5 through stride 2 gives 1x3");
    check(L.outDims(0, TensorShape{1, 3, 56, 56}) == TensorShape{1, 8, 28, 28}, "56x56 through stride 2 gives 28x28");
    check(convOutSize(7, 2) == 4 && convOutSize(8, 2) == 4 && convOutSize(9, 1) == 9, "convOutSize");
    ConvNetReference ref(d, convDefaults());
    check(ref.outputShape(TensorShape{2, 3, 13, 7}) == TensorShape{2, 4, 7, 4}, "outputShape chains the layers");

    // Init: He-uniform from initHash, biases 0, the head all zero.
    const std::vector<f32> w = initConvWeights(d);
    check(w.size() == L.total && bitEqual(w, ref.weights()) && bitEqual(w, ref.ema()),
          "ConvNetReference starts from initConvWeights (EMA equal)");
    check(bitEqual(w, initConvWeights(d)), "same seed gives bit-identical weights");
    ConvNetDesc d2 = d; d2.seed = 6;
    check(!bitEqual(w, initConvWeights(d2)), "a different seed gives different weights");

    const f32 bound0 = std::sqrt(6.0f / (3.0f * 9.0f));
    const u32 idx = 5;
    const f32 u = static_cast<f32>(initHash(d.seed * 0x9E3779B9u + idx) >> 8) * (1.0f / 16777216.0f);
    check(w[idx] == (2.0f * u - 1.0f) * bound0, "weight 5 is (2u - 1) * sqrt(6 / fan_in) with u from initHash(seed, index)");
    bool inBound = true, biasZero = true, headZero = true;
    f32 mx[2] = {0.0f, 0.0f};
    for (u32 l = 0; l + 1 < L.layers; ++l) {
        const f32 bound = std::sqrt(6.0f / static_cast<f32>(L.cin[l] * L.kernel[l] * L.kernel[l]));
        for (u32 k = 0; k < L.cout[l] * L.cin[l] * L.kernel[l] * L.kernel[l]; ++k) {
            const f32 v = std::fabs(w[L.wOffset[l] + k]);
            inBound = inBound && v <= bound;
            mx[l] = std::max(mx[l], v);
        }
        for (u32 co = 0; co < L.cout[l]; ++co) biasZero = biasZero && w[L.bOffset[l] + co] == 0.0f;
        check(mx[l] > 0.6f * bound, "layer " + std::to_string(l) + " weights spread over the He range");
    }
    for (u32 k = 0; k < L.layerSize[2]; ++k) headZero = headZero && w[L.wOffset[2] + k] == 0.0f;
    check(inBound, "every weight is inside its layer's He bound");
    check(biasZero, "biases initialise to zero");
    check(headZero, "the head layer is zero-initialised");
    std::vector<f32> in(3 * 5 * 5, 0.5f), out(4 * 3 * 3, 7.0f);
    ref.forward(TensorShape{1, 3, 5, 5}, in, out);
    bool allZero = true;
    for (f32 v : out) allZero = allZero && v == 0.0f;
    check(allZero, "a fresh network outputs zeros (zero head)");
}

// ---------------------------------------------------------------- forward against an independent naive conv

struct NaiveTensor {
    u32 n, c, h, w;
    std::vector<double> v;
    double& at(u32 ni, u32 ci, u32 y, u32 x) { return v[((static_cast<usize>(ni) * c + ci) * h + y) * w + x]; }
};

// Written for clarity, not to mirror the reference: fp64, loops over taps outside the channels.
NaiveTensor naiveConv(NaiveTensor in, const std::vector<f32>& w, usize wOff, bool bias, u32 cout, u32 k, u32 s,
                      bool relu) {
    const u32 oh = (in.h + s - 1) / s, ow = (in.w + s - 1) / s;
    const int pad = (k == 3) ? 1 : 0;
    NaiveTensor out{in.n, cout, oh, ow, std::vector<double>(static_cast<usize>(in.n) * cout * oh * ow, 0.0)};
    const usize bOff = wOff + static_cast<usize>(cout) * in.c * k * k;
    for (u32 n = 0; n < in.n; ++n)
        for (u32 co = 0; co < cout; ++co)
            for (u32 oy = 0; oy < oh; ++oy)
                for (u32 ox = 0; ox < ow; ++ox) {
                    double sum = bias ? static_cast<double>(w[bOff + co]) : 0.0;
                    for (u32 ky = 0; ky < k; ++ky)
                        for (u32 kx = 0; kx < k; ++kx) {
                            const int iy = static_cast<int>(oy * s + ky) - pad, ix = static_cast<int>(ox * s + kx) - pad;
                            if (iy < 0 || ix < 0 || iy >= static_cast<int>(in.h) || ix >= static_cast<int>(in.w)) continue;
                            for (u32 ci = 0; ci < in.c; ++ci)
                                sum += static_cast<double>(w[wOff + ((static_cast<usize>(co) * in.c + ci) * k + ky) * k + kx]) *
                                       in.at(n, ci, static_cast<u32>(iy), static_cast<u32>(ix));
                        }
                    if (relu && !(sum > 0.0)) sum = 0.0;
                    out.at(n, co, oy, ox) = sum;
                }
    return out;
}

// Runs the descriptor through the reference and through the naive conv; returns the worst relative error.
f64 forwardError(const ConvNetDesc& d, const TensorShape& shape, u32 seed, TensorShape* outShape = nullptr) {
    ConvNetReference ref(d, convDefaults());
    Rng rng(seed);
    randomise(ref, rng, 0.5f);
    const std::vector<f32> x = randomTensor(rng, shape.count(), -1.0f, 1.0f);
    const TensorShape os = ref.outputShape(shape);
    if (outShape) *outShape = os;
    std::vector<f32> out(os.count());
    ref.forward(shape, x, out);

    NaiveTensor cur{shape.n, shape.c, shape.h, shape.w, std::vector<double>(x.begin(), x.end())};
    usize off = 0;   // the test's own offset arithmetic, independent of ConvLayout
    for (const ConvLayerDesc& l : d.layers) {
        cur = naiveConv(cur, ref.weights(), off, l.bias, l.cout, l.kernel, l.stride, l.act == Activation::ReLU);
        off += static_cast<usize>(l.cout) * l.cin * l.kernel * l.kernel + (l.bias ? l.cout : 0u);
    }
    if (cur.v.size() != out.size()) return 1.0e9;
    double scale = 1.0;
    for (double v : cur.v) scale = std::max(scale, std::fabs(v));
    double worst = 0.0;
    for (usize i = 0; i < out.size(); ++i) worst = std::max(worst, std::fabs(cur.v[i] - static_cast<double>(out[i])) / scale);
    return worst;
}

void testForward() {
    AVER_INFO("-- forward against a naive conv");
    TensorShape os;
    const ConvNetDesc d = net(3, {layer(3, 8, 3, 2), layer(8, 8, 3, 1), layer(8, 4, 1, 1, Activation::None)}, 4);
    f64 e = forwardError(d, TensorShape{2, 3, 13, 7}, 11, &os);
    check(os == TensorShape{2, 4, 7, 4}, "13x7 input: output is 7x4");
    check(e < 1e-5, "13x7, strides 2,1 + 1x1 head match the naive conv (rel err " + std::to_string(e) + ")");
    e = forwardError(d, TensorShape{1, 3, 1, 1}, 12, &os);
    check(os == TensorShape{1, 4, 1, 1} && e < 1e-5, "a 1x1 image (every tap but the centre is padding) matches");
    e = forwardError(d, TensorShape{1, 3, 2, 3}, 13);
    check(e < 1e-5, "a 2x3 image matches (stride 2 over an odd edge)");
    const ConvNetDesc pointwise = net(5, {layer(5, 8, 1, 1), layer(8, 4, 1, 1, Activation::None, false)});
    e = forwardError(pointwise, TensorShape{2, 5, 9, 5}, 14);
    check(e < 1e-5, "1x1 kernels, no head bias, match (rel err " + std::to_string(e) + ")");
    const ConvNetDesc deep = net(4, {layer(4, 8, 3, 2), layer(8, 8, 3, 2, Activation::None), layer(8, 4, 3, 1, Activation::ReLU, false),
                                     layer(4, 4, 1, 1, Activation::None)});
    e = forwardError(deep, TensorShape{3, 4, 17, 11}, 15, &os);
    check(os == TensorShape{3, 4, 5, 3} && e < 1e-5, "a deeper net with mixed bias/activation matches (17x11 -> 5x3)");

    // Border behaviour pinned by hand: one 3x3 layer, one channel of weight 1 at tap (0,0) reads up-left.
    ConvNetDesc one = net(1, {layer(1, 4, 3, 1, Activation::None, false)});
    ConvNetReference ref(one, convDefaults());
    std::vector<f32> w(ref.weightCount(), 0.0f);
    w[0] = 1.0f;   // co 0, ci 0, ky 0, kx 0
    ref.setWeights(w);
    std::vector<f32> x(9), y(4 * 9);
    for (u32 i = 0; i < 9; ++i) x[i] = static_cast<f32>(i + 1);
    ref.forward(TensorShape{1, 1, 3, 3}, x, y);
    check(y[0] == 0.0f && y[1] == 0.0f && y[2] == 0.0f && y[3] == 0.0f && y[4] == 1.0f && y[8] == 5.0f,
          "tap (0,0) reads input (oy-1, ox-1), zero outside the frame");

    // ReLU written z > 0 ? z : 0: NaN becomes 0.
    const ConvNetDesc relu = net(1, {layer(1, 4, 1, 1, Activation::ReLU, false)});
    ConvNetReference rr(relu, convDefaults());
    std::vector<f32> wr(4, 1.0f);
    rr.setWeights(wr);
    std::vector<f32> xr = {std::numeric_limits<f32>::quiet_NaN(), -2.0f, 3.0f, 0.0f}, yr(16);
    rr.forward(TensorShape{1, 1, 2, 2}, xr, yr);
    check(yr[0] == 0.0f && yr[1] == 0.0f && yr[2] == 3.0f && yr[3] == 0.0f, "ReLU maps NaN, negatives and zero to 0");

    // The loss: weighted L2 over the head output, per-position weight shared across channels.
    ConvNetReference lossNet(d, convDefaults());
    Rng rng(77);
    randomise(lossNet, rng, 0.5f);
    const TensorShape in{2, 3, 13, 7};
    const std::vector<f32> xi = randomTensor(rng, in.count(), -1.0f, 1.0f);
    const TensorShape o = lossNet.outputShape(in);
    std::vector<f32> p(o.count());
    lossNet.forward(in, xi, p);
    const std::vector<f32> t = randomTensor(rng, o.count(), -1.0f, 1.0f);
    const std::vector<f32> pw = randomTensor(rng, static_cast<usize>(o.n) * o.h * o.w, 0.0f, 2.0f);
    double expect = 0.0;
    for (u32 n = 0; n < o.n; ++n)
        for (u32 c = 0; c < o.c; ++c)
            for (u32 yy = 0; yy < o.h; ++yy)
                for (u32 xx = 0; xx < o.w; ++xx) {
                    const usize i = ((static_cast<usize>(n) * o.c + c) * o.h + yy) * o.w + xx;
                    const double err = static_cast<double>(p[i]) - static_cast<double>(t[i]);
                    expect += static_cast<double>(pw[(static_cast<usize>(n) * o.h + yy) * o.w + xx]) * err * err;
                }
    const f32 norm = static_cast<f32>(o.n * o.h * o.w);
    expect /= static_cast<double>(norm);
    const f32 got = lossNet.evaluate(in, xi, t, pw, norm, false);
    check(std::fabs(got - expect) < 1e-5 * (1.0 + expect), "evaluate is sum pw * (p - t)^2 / lossNorm");
    std::vector<f32> pw2 = pw;
    for (f32& v : pw2) v *= 2.0f;
    check(std::fabs(lossNet.evaluate(in, xi, t, pw2, norm, false) - 2.0f * got) < 1e-5f * (1.0f + got),
          "doubling the position weights doubles the loss");
    check(std::fabs(lossNet.evaluate(in, xi, t, pw, 2.0f * norm, false) - 0.5f * got) < 1e-5f * (1.0f + got),
          "lossNorm divides the loss");
}

// ---------------------------------------------------------------- finite differences

// Central differences of a linear functional of the head output (fp64) against backward: dW (incl. db) and dX.
void fdCheck(const ConvNetDesc& d, const char* label, u32 seed) {
    ConvNetReference ref(d, convDefaults());
    Rng rng(seed);
    randomise(ref, rng, 0.5f);
    const TensorShape in{2, d.inChannels, 7, 6};
    std::vector<f32> x = randomTensor(rng, in.count(), -1.0f, 1.0f);
    const TensorShape os = ref.outputShape(in);
    const std::vector<f32> coef = randomTensor(rng, os.count(), -1.0f, 1.0f);

    std::vector<f32> gw(ref.weightCount()), dIn;
    ref.backward(in, x, coef, gw, &dIn);

    std::vector<f32> out(os.count());
    auto lossAt = [&]() {
        ref.forward(in, x, out);
        double s = 0.0;
        for (usize i = 0; i < out.size(); ++i) s += static_cast<double>(coef[i]) * static_cast<double>(out[i]);
        return s;
    };

    constexpr f32 eps = 2.0e-3f;
    auto compare = [](f32 analytic, double numeric) {
        const f32 tol = 0.03f * std::max(std::fabs(analytic), static_cast<f32>(std::fabs(numeric))) + 3.0e-3f;
        return std::fabs(analytic - static_cast<f32>(numeric)) <= tol;
    };

    // Weights, split into W and b so a broken bias path cannot hide in the bulk.
    std::vector<bool> isBias(ref.weightCount(), false);
    const ConvLayout& L = ref.layout();
    for (u32 l = 0; l < L.layers; ++l)
        if (d.layers[l].bias)
            for (u32 co = 0; co < L.cout[l]; ++co) isBias[L.bOffset[l] + co] = true;
    u32 wOut = 0, wN = 0, bOut = 0, bN = 0, wNonzero = 0;
    for (u32 k = 0; k < ref.weightCount(); ++k) {
        const f32 saved = ref.weights()[k];
        ref.weights()[k] = saved + eps;
        const double lp = lossAt();
        ref.weights()[k] = saved - eps;
        const double lm = lossAt();
        ref.weights()[k] = saved;
        const bool ok = compare(gw[k], (lp - lm) / (2.0 * eps));
        if (isBias[k]) { ++bN; bOut += ok ? 0u : 1u; } else { ++wN; wOut += ok ? 0u : 1u; if (gw[k] != 0.0f) ++wNonzero; }
    }
    u32 xOut = 0, xNonzero = 0;
    for (usize i = 0; i < x.size(); ++i) {
        const f32 saved = x[i];
        x[i] = saved + eps;
        const double lp = lossAt();
        x[i] = saved - eps;
        const double lm = lossAt();
        x[i] = saved;
        if (!compare(dIn[i], (lp - lm) / (2.0 * eps))) ++xOut;
        if (dIn[i] != 0.0f) ++xNonzero;
    }
    // A ReLU kink within eps of a pre-activation can make one numeric slope wrong.
    check(wOut <= wN * 3 / 100 && wNonzero > wN / 4,
          std::string("dW matches finite differences: ") + label + " (" + std::to_string(wOut) + " outliers of " +
              std::to_string(wN) + ")");
    check(bN > 0 && bOut <= std::max(1u, bN / 10),
          std::string("db matches finite differences: ") + label + " (" + std::to_string(bOut) + " outliers of " +
              std::to_string(bN) + ")");
    check(xOut <= static_cast<u32>(x.size()) * 3 / 100 && xNonzero > x.size() / 4,
          std::string("dX matches finite differences: ") + label + " (" + std::to_string(xOut) + " outliers of " +
              std::to_string(x.size()) + ")");
}

void testGradients() {
    AVER_INFO("-- finite-difference gradients");
    fdCheck(net(3, {layer(3, 4, 3, 1, Activation::ReLU), layer(4, 8, 3, 2, Activation::ReLU),
                    layer(8, 4, 1, 1, Activation::None)}, 9),
            "3x3 s1 ReLU, 3x3 s2 ReLU, 1x1 None head", 101);
    fdCheck(net(2, {layer(2, 4, 3, 2, Activation::None), layer(4, 4, 3, 1, Activation::ReLU, false),
                    layer(4, 4, 1, 1, Activation::ReLU)}, 9),
            "3x3 s2 None, 3x3 ReLU no bias, 1x1 ReLU head", 202);
}

// ---------------------------------------------------------------- patch versus full frame

void testPatchVersusFrame() {
    AVER_INFO("-- patch versus full frame");
    // Strides 2,1,2,1 + a 1x1 head: 1/4 resolution, receptive radius 9 input pixels.
    const ConvNetDesc d = net(4, {layer(4, 8, 3, 2), layer(8, 8, 3, 1), layer(8, 8, 3, 2), layer(8, 8, 3, 1),
                                  layer(8, 4, 1, 1, Activation::None)}, 3);
    ConvNetReference ref(d, convDefaults());
    Rng rng(555);
    randomise(ref, rng, 0.4f);

    const TensorShape patch{1, 4, 56, 56};
    const TensorShape pOut = ref.outputShape(patch);
    check(pOut == TensorShape{1, 4, 14, 14}, "a 56x56 patch gives a 14x14 output grid");

    constexpr u32 fh = 104, fw = 120, oy = 24, ox = 16;   // 8-pixel aligned offset of the patch in the frame
    const TensorShape frame{1, 4, fh, fw};
    const std::vector<f32> fin = randomTensor(rng, frame.count(), -1.0f, 1.0f);
    std::vector<f32> pin(patch.count());
    for (u32 c = 0; c < 4; ++c)
        for (u32 y = 0; y < 56; ++y)
            for (u32 x = 0; x < 56; ++x)
                pin[(static_cast<usize>(c) * 56 + y) * 56 + x] = fin[(static_cast<usize>(c) * fh + oy + y) * fw + ox + x];

    const TensorShape fOut = ref.outputShape(frame);
    std::vector<f32> po(pOut.count()), fo(fOut.count());
    ref.forward(patch, pin, po);
    ref.forward(frame, fin, fo);

    const u32 cy0 = oy / 4, cx0 = ox / 4;   // the patch's first output cell in the frame's grid
    bool coreEqual = true, edgeDiffers = false;
    for (u32 c = 0; c < 4; ++c)
        for (u32 cy = 0; cy < 14; ++cy)
            for (u32 cx = 0; cx < 14; ++cx) {
                const f32 a = po[(static_cast<usize>(c) * 14 + cy) * 14 + cx];
                const f32 b = fo[(static_cast<usize>(c) * fOut.h + cy0 + cy) * fOut.w + cx0 + cx];
                const bool core = cy >= 3 && cy <= 10 && cx >= 3 && cx <= 10;
                if (core) coreEqual = coreEqual && std::memcmp(&a, &b, sizeof(f32)) == 0;
                if (cy == 0 && cx == 0 && std::memcmp(&a, &b, sizeof(f32)) != 0) edgeDiffers = true;
            }
    check(coreEqual, "core tiles 3..10 of the patch are bit-identical to the frame's outputs");
    check(edgeDiffers, "the patch border differs from the frame (zero padding versus real neighbours)");
}

// ---------------------------------------------------------------- toy data

// One smooth field per record seen through 4 channels of different gain; the target is each channel
// averaged over 4x4 cells (1/4 resolution), so the net must learn to pool.
void toyBatch(Rng& rng, u32 n, u32 size, std::vector<f32>& in, std::vector<f32>& target, std::vector<f32>& pw) {
    const u32 cells = size / 4;
    const f32 gain[4] = {1.0f, 0.8f, 0.6f, 0.4f};
    in.assign(static_cast<usize>(n) * 4 * size * size, 0.0f);
    target.assign(static_cast<usize>(n) * 4 * cells * cells, 0.0f);
    pw.assign(static_cast<usize>(n) * cells * cells, 1.0f);
    for (u32 r = 0; r < n; ++r) {
        const f32 a = rng.range(0.03f, 0.12f), b = rng.range(0.03f, 0.12f), ph = rng.range(0.0f, 6.28f);
        for (u32 c = 0; c < 4; ++c) {
            f32* plane = &in[(static_cast<usize>(r) * 4 + c) * size * size];
            for (u32 y = 0; y < size; ++y)
                for (u32 x = 0; x < size; ++x)
                    plane[y * size + x] = 0.5f + gain[c] * 0.5f * std::sin(a * static_cast<f32>(y) + b * static_cast<f32>(x) + ph);
            f32* tp = &target[(static_cast<usize>(r) * 4 + c) * cells * cells];
            for (u32 cy = 0; cy < cells; ++cy)
                for (u32 cx = 0; cx < cells; ++cx) {
                    f32 s = 0.0f;
                    for (u32 y = 0; y < 4; ++y)
                        for (u32 x = 0; x < 4; ++x) s += plane[(cy * 4 + y) * size + cx * 4 + x];
                    tp[cy * cells + cx] = s / 16.0f;
                }
        }
    }
}

ConvNetDesc toyDesc() {
    return net(4, {layer(4, 8, 3, 2), layer(8, 8, 3, 2), layer(8, 4, 1, 1, Activation::None)}, 3);
}

OptimiserDesc toyOptimiser() {
    OptimiserDesc o = convDefaults();
    o.learningRate = 5e-3f;
    return o;
}

// ---------------------------------------------------------------- determinism, partial order, Adam

void testDeterminism() {
    AVER_INFO("-- determinism");
    ConvNetReference a(toyDesc(), toyOptimiser()), b(toyDesc(), toyOptimiser());
    Rng ra(21), rb(21);
    std::vector<f32> in, tgt, pw;
    bool lossesEqual = true;
    for (u32 s = 0; s < 10; ++s) {
        toyBatch(ra, 2, 24, in, tgt, pw);
        const f32 la = a.trainBatch(TensorShape{2, 4, 24, 24}, in, tgt, pw, 2.0f * 36.0f);
        toyBatch(rb, 2, 24, in, tgt, pw);
        const f32 lb = b.trainBatch(TensorShape{2, 4, 24, 24}, in, tgt, pw, 2.0f * 36.0f);
        lossesEqual = lossesEqual && std::memcmp(&la, &lb, sizeof(f32)) == 0;
    }
    check(lossesEqual, "the per-step losses of two runs are bit-identical");
    check(bitEqual(a.weights(), b.weights()) && bitEqual(a.ema(), b.ema()) && bitEqual(a.moment1(), b.moment1()) &&
              bitEqual(a.moment2(), b.moment2()),
          "same batches twice give bit-identical weights, EMA and moments after 10 steps");
    check(a.step() == 10 && !bitEqual(a.weights(), initConvWeights(toyDesc())), "ten steps counted, weights moved");
    bool cleared = true;
    for (i32 v : a.accumulator()) cleared = cleared && v == 0;
    check(cleared, "the step clears the accumulator");
}

void testPartialOrder() {
    AVER_INFO("-- GPU-order accumulation");
    // One layer, so dz = dOut and x = the input: the test writes the spec's partial order out by hand and
    // demands bit equality with the reference's accumulator.
    const ConvNetDesc d = net(4, {layer(4, 4, 3, 1, Activation::None)}, 2);
    OptimiserDesc o = convDefaults();
    ConvNetReference ref(d, o);
    Rng rng(808);
    randomise(ref, rng, 0.5f);
    const TensorShape in{2, 4, 20, 12};   // 3 x 2 tiles of 8x8 per record, partial edge tiles
    const std::vector<f32> x = randomTensor(rng, in.count(), -1.0f, 1.0f);
    const TensorShape os = ref.outputShape(in);
    const std::vector<f32> t = randomTensor(rng, os.count(), -1.0f, 1.0f);
    const std::vector<f32> pw = randomTensor(rng, static_cast<usize>(os.n) * os.h * os.w, 0.5f, 1.5f);
    const f32 norm = static_cast<f32>(os.n * os.h * os.w);

    std::vector<f32> p(os.count());
    ref.forward(in, x, p);
    std::vector<f32> dz(os.count());
    for (u32 n = 0; n < os.n; ++n)
        for (u32 c = 0; c < os.c; ++c)
            for (u32 y = 0; y < os.h; ++y)
                for (u32 xx = 0; xx < os.w; ++xx) {
                    const usize i = ((static_cast<usize>(n) * os.c + c) * os.h + y) * os.w + xx;
                    dz[i] = (2.0f * pw[(static_cast<usize>(n) * os.h + y) * os.w + xx] * (p[i] - t[i])) / norm;
                }

    const f32 loss = ref.accumulateBatch(in, x, t, pw, norm);
    const f32 evalLoss = ref.evaluate(in, x, t, pw, norm, false);
    check(std::fabs(loss - evalLoss) <= 1e-6f * (1.0f + std::fabs(evalLoss)), "accumulateBatch returns the loss before the step");

    const ConvLayout& L = ref.layout();
    const u32 tilesY = (os.h + 7) / 8, tilesX = (os.w + 7) / 8;
    u32 mismatches = 0, nonzero = 0;
    for (u32 co = 0; co < 4; ++co) {
        for (u32 ci = 0; ci < 4; ++ci)
            for (u32 ky = 0; ky < 3; ++ky)
                for (u32 kx = 0; kx < 3; ++kx) {
                    f32 g = 0.0f;
                    for (u32 n = 0; n < os.n; ++n)
                        for (u32 ty = 0; ty < tilesY; ++ty)
                            for (u32 tx = 0; tx < tilesX; ++tx) {
                                f32 part = 0.0f;
                                for (u32 y = ty * 8; y < std::min(os.h, ty * 8 + 8); ++y)
                                    for (u32 xx = tx * 8; xx < std::min(os.w, tx * 8 + 8); ++xx) {
                                        const int iy = static_cast<int>(y) - 1 + static_cast<int>(ky);
                                        const int ix = static_cast<int>(xx) - 1 + static_cast<int>(kx);
                                        const bool inside = iy >= 0 && ix >= 0 && iy < static_cast<int>(in.h) && ix < static_cast<int>(in.w);
                                        const f32 xv = inside ? x[((static_cast<usize>(n) * 4 + ci) * in.h + iy) * in.w + ix] : 0.0f;
                                        part += dz[((static_cast<usize>(n) * 4 + co) * os.h + y) * os.w + xx] * xv;
                                    }
                                g += part;
                            }
                    const usize k = L.wOffset[0] + ((static_cast<usize>(co) * 4 + ci) * 3 + ky) * 3 + kx;
                    if (ref.accumulator()[k] != quantise(g, o)) ++mismatches;
                    if (ref.accumulator()[k] != 0) ++nonzero;
                }
        f32 g = 0.0f;
        for (u32 n = 0; n < os.n; ++n)
            for (u32 ty = 0; ty < tilesY; ++ty)
                for (u32 tx = 0; tx < tilesX; ++tx) {
                    f32 part = 0.0f;
                    for (u32 y = ty * 8; y < std::min(os.h, ty * 8 + 8); ++y)
                        for (u32 xx = tx * 8; xx < std::min(os.w, tx * 8 + 8); ++xx)
                            part += dz[((static_cast<usize>(n) * 4 + co) * os.h + y) * os.w + xx];
                    g += part;
                }
        if (ref.accumulator()[L.bOffset[0] + co] != quantise(g, o)) ++mismatches;
    }
    check(mismatches == 0 && nonzero > 100, "accumulator = quantise(sum of 8x8 tile partials, p ascending), bit for bit (" +
                                                std::to_string(nonzero) + " nonzero weights)");

    // Multi-layer: the GPU-order gradient and the plain gradient are the same maths, summed differently.
    const ConvNetDesc deep = net(4, {layer(4, 8, 3, 2), layer(8, 8, 3, 1), layer(8, 4, 1, 1, Activation::None)}, 6);
    ConvNetReference dn(deep, o);
    randomise(dn, rng, 0.5f);
    const TensorShape din{2, 4, 40, 36};
    const std::vector<f32> dx = randomTensor(rng, din.count(), -1.0f, 1.0f);
    const TensorShape dout = dn.outputShape(din);
    const std::vector<f32> dy = randomTensor(rng, dout.count(), -1.0f, 1.0f);
    std::vector<f32> plain(dn.weightCount()), tiled(dn.weightCount());
    dn.backward(din, dx, dy, plain);
    dn.backwardGpuOrder(din, dx, dy, tiled);
    f32 gmax = 0.0f;
    for (f32 v : plain) gmax = std::max(gmax, std::fabs(v));
    f32 worst = 0.0f;
    bool differs = false;
    for (usize k = 0; k < plain.size(); ++k) {
        worst = std::max(worst, std::fabs(plain[k] - tiled[k]) / gmax);
        differs = differs || plain[k] != tiled[k];
    }
    AVER_INFO("  partial-order vs plain gradient: worst difference / max|g| {:.3e} (max|g| {:.3f})", worst, gmax);
    check(worst < 1e-5f, "the partial-order gradient equals the plain gradient to 1e-5 of max|g|");
    check(differs, "and the two orders really are different sums (not the same code path)");

    // accumulateBatch quantises that gradient: loss gradient from the reference's own forward.
    ConvNetReference batch(deep, o);
    batch.setWeights(dn.weights());
    std::vector<f32> pp(dout.count());
    batch.forward(din, dx, pp);
    const std::vector<f32> tt = randomTensor(rng, dout.count(), -1.0f, 1.0f);
    const std::vector<f32> ww = randomTensor(rng, static_cast<usize>(dout.n) * dout.h * dout.w, 0.0f, 2.0f);
    const f32 nrm = static_cast<f32>(dout.n * dout.h * dout.w);
    std::vector<f32> dOut(dout.count());
    for (u32 n = 0; n < dout.n; ++n)
        for (u32 c = 0; c < dout.c; ++c)
            for (u32 y = 0; y < dout.h; ++y)
                for (u32 xx = 0; xx < dout.w; ++xx) {
                    const usize i = ((static_cast<usize>(n) * dout.c + c) * dout.h + y) * dout.w + xx;
                    dOut[i] = (2.0f * ww[(static_cast<usize>(n) * dout.h + y) * dout.w + xx] * (pp[i] - tt[i])) / nrm;
                }
    std::vector<f32> expectG(batch.weightCount());
    batch.backwardGpuOrder(din, dx, dOut, expectG);
    batch.accumulateBatch(din, dx, tt, ww, nrm);
    bool same = true;
    for (u32 k = 0; k < batch.weightCount(); ++k) same = same && batch.accumulator()[k] == quantise(expectG[k], o);
    check(same, "accumulateBatch on a deep net = quantise of backwardGpuOrder of the weighted-L2 gradient");
}

void testAdamParity() {
    AVER_INFO("-- Adam parity (shared adamStep)");
    // MlpReference::adamStep versus the free function on copies of the same state, over several steps.
    MlpDesc md; md.inputs = 3; md.outputs = 2; md.hiddenWidth = 8; md.hiddenLayers = 2; md.seed = 4;
    OptimiserDesc o; o.learningRate = 1e-2f; o.l2 = 1e-3f;
    MlpReference mlp(md, o);
    std::vector<f32> w = mlp.weights(), ema = mlp.ema(), m = mlp.moment1(), v = mlp.moment2();
    std::vector<i32> acc(mlp.weightCount(), 0);
    Rng rng(3);
    bool same = true;
    for (u32 step = 1; step <= 5; ++step) {
        for (u32 r = 0; r < 20; ++r) {
            const f32 in[3] = {rng.range(-1.0f, 1.0f), rng.range(-1.0f, 1.0f), rng.range(-1.0f, 1.0f)};
            const f32 tg[2] = {rng.range(0.0f, 1.0f), rng.range(0.0f, 1.0f)};
            mlp.accumulateRecord(in, tg);
        }
        acc = mlp.accumulator();
        mlp.adamStep(20);
        adamStep(w, ema, m, v, acc, o, step, 20);
        same = same && bitEqual(w, mlp.weights()) && bitEqual(ema, mlp.ema()) && bitEqual(m, mlp.moment1()) &&
               bitEqual(v, mlp.moment2());
        for (i32 a : acc) same = same && a == 0;
    }
    check(same, "MlpReference::adamStep and NeuralOptimiser adamStep agree bit for bit over 5 steps");
    check(MlpReference::quantise(1.9e-5f, o) == quantise(1.9e-5f, o) && quantise(1.0e9f, o) == 16 * 65536 &&
              quantise(std::numeric_limits<f32>::quiet_NaN(), o) == 0,
          "MlpReference::quantise is the shared quantise");

    // ConvNetReference::adamStep is the same function with liveCount 1.
    ConvNetReference conv(toyDesc(), toyOptimiser());
    std::vector<f32> in, tgt, pw;
    toyBatch(rng, 2, 24, in, tgt, pw);
    conv.accumulateBatch(TensorShape{2, 4, 24, 24}, in, tgt, pw, 72.0f);
    std::vector<f32> cw = conv.weights(), cema = conv.ema(), cm = conv.moment1(), cv = conv.moment2();
    std::vector<i32> cacc = conv.accumulator();
    adamStep(cw, cema, cm, cv, cacc, conv.optimiser(), 1, 1);
    conv.adamStep();
    check(bitEqual(cw, conv.weights()) && bitEqual(cema, conv.ema()) && bitEqual(cm, conv.moment1()) &&
              bitEqual(cv, conv.moment2()) && conv.step() == 1,
          "ConvNetReference::adamStep == adamStep(step, liveCount 1)");

    // A zero live count changes nothing but the clear.
    std::vector<f32> w0 = cw;
    std::vector<i32> some(cw.size(), 123);
    adamStep(cw, cema, cm, cv, some, conv.optimiser(), 2, 0);
    check(bitEqual(w0, cw) && some == std::vector<i32>(cw.size(), 0), "liveCount 0 only clears the accumulator");

    // The shared helpers still behave as the MLP test expects.
    check(adamBiasCorrection(0.9f, 1) > 0.0999f && adamBiasCorrection(0.9f, 1) < 0.1001f, "bias correction at step 1 is 1 - beta");
    check(validate(o) && !validate(OptimiserDesc{.gradClamp = 0.0f}), "validate(OptimiserDesc) lives in NeuralOptimiser");
}

// ---------------------------------------------------------------- AVNN v2

std::vector<char> slurp(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
void spit(const std::filesystem::path& p, const std::vector<char>& bytes) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
u32 word(const std::vector<char>& b, usize byteOffset) {
    u32 v;
    std::memcpy(&v, b.data() + byteOffset, 4);
    return v;
}
void setWord(std::vector<char>& b, usize byteOffset, u32 v) { std::memcpy(b.data() + byteOffset, &v, 4); }
// Rewrites the trailing CRC so a deliberately edited file is rejected for its edit, not its checksum.
void fixCrc(std::vector<char>& b) {
    const u32 crc = crc32Ieee(std::span<const u8>(reinterpret_cast<const u8*>(b.data()), b.size() - 4));
    setWord(b, b.size() - 4, crc);
}

void testWeightFile() {
    AVER_INFO("-- AVNN v2 weight file");
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "aver_neural_conv_test.avnn";

    const u8 digits[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    check(crc32Ieee(digits) == 0xCBF43926u, "CRC-32 IEEE of \"123456789\" is 0xCBF43926");

    const ConvNetDesc d = net(5, {layer(5, 8, 3, 2), layer(8, 8, 3, 1, Activation::ReLU, false), layer(8, 4, 1, 1, Activation::None)}, 9);
    ConvNetReference ref(d, convDefaults());
    Rng rng(17);
    randomise(ref, rng, 0.5f);
    const std::vector<f32>& w = ref.weights();

    ConvIoAffine io;
    io.inScale = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
    io.inBias = {-1.0f, -2.0f, 0.5f, 0.25f, 0.0f};
    io.outScale = {0.5f, 0.25f, 2.0f, 8.0f};
    io.outBias = {0.1f, 0.2f, 0.3f, 0.4f};
    check(saveConvWeightFile(path.string(), d, w, &io), "save with an io affine succeeds");

    u32 version = 0, kind = 99;
    check(peekWeightFile(path.string(), version, kind) && version == 2 && kind == kWeightKindConvNet, "peek reports version 2, kind ConvNet");
    {
        const std::vector<char> bytes = slurp(path);
        const u32 headerBytes = word(bytes, 12);
        check(bytes[0] == 'A' && bytes[1] == 'V' && bytes[2] == 'N' && bytes[3] == 'N' && word(bytes, 0) == kWeightFileMagic,
              "magic bytes are AVNN");
        check(headerBytes == 4 * (8 + 8 * 3) + 4 * (2 * 5 + 2 * 4), "headerBytes = fixed words + layer records + affine");
        check(bytes.size() == headerBytes + 4 + w.size() * 4 + 4, "file size = header + totalWeights + weights + crc");
        check(word(bytes, headerBytes) == w.size(), "totalWeights follows the header");
    }

    ConvNetDesc back; back.seed = 77;
    std::vector<f32> wBack;
    ConvIoAffine ioBack;
    check(loadConvWeightFile(path.string(), back, wBack, &ioBack), "load succeeds");
    bool descSame = back.inChannels == d.inChannels && back.layers.size() == d.layers.size();
    for (usize l = 0; descSame && l < d.layers.size(); ++l)
        descSame = back.layers[l].cin == d.layers[l].cin && back.layers[l].cout == d.layers[l].cout &&
                   back.layers[l].kernel == d.layers[l].kernel && back.layers[l].stride == d.layers[l].stride &&
                   back.layers[l].act == d.layers[l].act && back.layers[l].bias == d.layers[l].bias;
    check(descSame, "the descriptor round-trips");
    check(back.seed == 77, "the seed is not stored: the caller's survives a load");
    check(bitEqual(wBack, w), "the weights round-trip bit for bit");
    check(ioBack.inScale == io.inScale && ioBack.inBias == io.inBias && ioBack.outScale == io.outScale &&
              ioBack.outBias == io.outBias,
          "the io affine round-trips");

    // No affine: written without, and a caller's affine comes back cleared.
    check(saveConvWeightFile(path.string(), d, w), "save without an affine succeeds");
    ConvIoAffine junk = io;
    check(loadConvWeightFile(path.string(), back, wBack, &junk) && junk.inScale.empty() && junk.outBias.empty(),
          "a file without an affine clears the caller's");
    check(loadConvWeightFile(path.string(), back, wBack, nullptr) && bitEqual(wBack, w), "io may be null");
    check(slurp(path).size() == 4 * (8 + 8 * 3) + 4 + w.size() * 4 + 4, "no affine, no affine bytes");

    // Refusals on save.
    check(!saveConvWeightFile(path.string(), d, std::span<const f32>(w).subspan(1)), "save refuses a wrong-sized weight array");
    ConvIoAffine badIo = io; badIo.outScale.pop_back();
    check(!saveConvWeightFile(path.string(), d, w, &badIo), "save refuses an affine of the wrong length");
    ConvNetDesc invalid = d; invalid.layers[0].kernel = 5;
    check(!saveConvWeightFile(path.string(), invalid, w), "save refuses an invalid descriptor");

    // Rejections on load.
    check(saveConvWeightFile(path.string(), d, w, &io), "re-save for the rejection cases");
    const std::vector<char> good = slurp(path);
    const u32 headerBytes = word(good, 12);
    auto rejects = [&](std::vector<char> bytes, const char* what) {
        spit(path, bytes);
        ConvNetDesc x; std::vector<f32> y;
        check(!loadConvWeightFile(path.string(), x, y), std::string("load rejects ") + what);
    };
    { auto b = good; b[0] = 'X'; fixCrc(b); rejects(b, "a wrong magic"); }
    { auto b = good; setWord(b, 4, 3); fixCrc(b); rejects(b, "an unknown version"); }
    { auto b = good; setWord(b, 4, 1); fixCrc(b); rejects(b, "version 1 (the MLP format)"); }
    { auto b = good; setWord(b, 8, 0); fixCrc(b); rejects(b, "a wrong kind"); }
    { auto b = good; b.resize(b.size() - 4); rejects(b, "a truncated file"); }
    { auto b = good; b.resize(b.size() - 4); fixCrc(b); rejects(b, "a truncated file with a repaired CRC"); }
    { auto b = good; b.resize(20); rejects(b, "a truncated header"); }
    { auto b = good; b.insert(b.end() - 4, 4, '\0'); fixCrc(b); rejects(b, "trailing bytes"); }
    { auto b = good; b.push_back(0); rejects(b, "a stray byte after the CRC"); }
    { auto b = good; b[headerBytes + 8] ^= 0x40; rejects(b, "a flipped weight bit (bad CRC)"); }
    { auto b = good; b[b.size() - 1] ^= 1; rejects(b, "a corrupted CRC word"); }
    { auto b = good; setWord(b, 36, 6); fixCrc(b); rejects(b, "an invalid shape (cout 6)"); }
    { auto b = good; setWord(b, 20, 0); fixCrc(b); rejects(b, "zero layers"); }
    { auto b = good; setWord(b, 24, 4); fixCrc(b); rejects(b, "an unknown flag bit"); }
    { auto b = good; setWord(b, headerBytes, word(b, headerBytes) + 1); fixCrc(b); rejects(b, "a total that disagrees with the shape"); }
    { auto b = good; setWord(b, 32 + 24, 7); fixCrc(b); rejects(b, "a layer weightCount that disagrees with its shape"); }
    { auto b = good; setWord(b, 12, headerBytes + 4); fixCrc(b); rejects(b, "a headerBytes that disagrees with the layout"); }
    ConvNetDesc x; std::vector<f32> y;
    check(!loadConvWeightFile((fs::temp_directory_path() / "aver_neural_conv_missing.avnn").string(), x, y),
          "load of a missing file fails");
    check(x.layers.empty() && y.empty(), "outputs are untouched by failed loads");

    // v1 coexistence: the MLP file keeps loading through the old API and is told apart by peek.
    MlpDesc md; md.inputs = 5; md.outputs = 3; md.hiddenWidth = 12; md.hiddenLayers = 2;
    md.hidden = Activation::Sigmoid; md.output = Activation::Exp; md.seed = 9;
    const std::vector<f32> mw = initWeights(md);
    check(saveWeightFile(path.string(), md, mw), "a v1 MLP file saves through the old API");
    check(peekWeightFile(path.string(), version, kind) && version == 1 && kind == kWeightKindMlp,
          "peek reports version 1, kind MLP");
    MlpDesc mback; std::vector<f32> mwBack;
    check(loadWeightFile(path.string(), mback, mwBack) && mback.inputs == 5 && mback.hidden == Activation::Sigmoid && bitEqual(mwBack, mw),
          "the v1 file still loads through loadWeightFile");
    check(!loadConvWeightFile(path.string(), x, y), "loadConvWeightFile rejects a v1 file");
    check(saveConvWeightFile(path.string(), d, w), "a v2 file saves over it");
    check(!loadWeightFile(path.string(), mback, mwBack), "loadWeightFile rejects a v2 file");
    check(!peekWeightFile((fs::temp_directory_path() / "aver_neural_conv_missing.avnn").string(), version, kind),
          "peek of a missing file fails");
    spit(path, std::vector<char>{'n', 'o', 'p', 'e', '!', '!', '!', '!'});
    check(!peekWeightFile(path.string(), version, kind), "peek rejects a file that is not AVNN");

    std::error_code ec;
    fs::remove(path, ec);
}

// ---------------------------------------------------------------- the toy task

void testToyTask() {
    AVER_INFO("-- toy task: learn a 1/4-resolution mean");
    ConvNetReference ref(toyDesc(), toyOptimiser());
    constexpr u32 size = 56, cells = 14, batch = 2;
    const TensorShape inShape{batch, 4, size, size};
    check(ref.outputShape(inShape) == TensorShape{batch, 4, cells, cells}, "the net maps 56x56 to 14x14");
    const f32 norm = static_cast<f32>(batch * cells * cells);

    Rng evalRng(4242);
    std::vector<f32> evIn, evTgt, evPw;
    toyBatch(evalRng, 4, size, evIn, evTgt, evPw);
    const TensorShape evShape{4, 4, size, size};
    const f32 evNorm = static_cast<f32>(4 * cells * cells);
    const f32 before = ref.evaluate(evShape, evIn, evTgt, evPw, evNorm, false);

    Rng rng(7);
    std::vector<f32> in, tgt, pw;
    bool finite = true;
    constexpr u32 steps = 400;
    for (u32 s = 0; s < steps; ++s) {
        toyBatch(rng, batch, size, in, tgt, pw);
        finite = finite && std::isfinite(ref.trainBatch(inShape, in, tgt, pw, norm));
    }
    const f32 after = ref.evaluate(evShape, evIn, evTgt, evPw, evNorm, false);
    const f32 afterEma = ref.evaluate(evShape, evIn, evTgt, evPw, evNorm, true);
    AVER_INFO("  toy: held-out loss {:.5f} -> {:.5f} (EMA {:.5f}) after {} steps", before, after, afterEma, steps);
    check(finite, "training stays finite");
    check(before > 0.1f, "the untrained (zero-head) net starts with a real loss");
    check(after * 10.0f < before, "the held-out loss falls at least 10x");
    check(afterEma * 3.0f < before, "the EMA weights learned it too");
    check(ref.step() == steps, "Adam counted every step");
    check(!bitEqual(ref.ema(), ref.weights()), "EMA lags the master weights");
}

}  // namespace

int main() {
    testValidation();
    testLayout();
    testForward();
    testGradients();
    testPatchVersusFrame();
    testDeterminism();
    testPartialOrder();
    testAdamParity();
    testWeightFile();
    testToyTask();

    if (g_failures == 0) AVER_INFO("=== all neural conv tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
