#include "aver/render/neural/ConvNetReference.hpp"

#include <algorithm>
#include <cmath>

// Keep in step with shaders/aver_neural_conv.hlsl: the contract in ConvNetReference.hpp is the spec.

namespace aver::render::neural {

namespace {

using Acts = std::vector<std::vector<f32>>;

bool fail(std::string* why, const char* msg) {
    if (why) *why = msg;
    return false;
}

inline i32 padOf(u32 kernel) { return kernel == 3u ? 1 : 0; }

// HLSL twin: CSConvForward. Canonical order: bias, then ci / ky / kx ascending, padding read as 0.
void forwardLayer(const ConvLayerDesc& L, const f32* W, const f32* B, const TensorShape& is, const f32* in,
                  const TensorShape& os, f32* out) {
    const i32 k = static_cast<i32>(L.kernel), s = static_cast<i32>(L.stride), pad = padOf(L.kernel);
    const i32 ih = static_cast<i32>(is.h), iw = static_cast<i32>(is.w);
    for (u32 n = 0; n < os.n; ++n)
        for (u32 co = 0; co < os.c; ++co)
            for (u32 oy = 0; oy < os.h; ++oy)
                for (u32 ox = 0; ox < os.w; ++ox) {
                    f32 acc = B ? B[co] : 0.0f;
                    for (u32 ci = 0; ci < is.c; ++ci) {
                        const f32* plane = in + (static_cast<usize>(n) * is.c + ci) * is.planeCount();
                        const f32* wk = W + (static_cast<usize>(co) * L.cin + ci) * static_cast<usize>(k * k);
                        for (i32 ky = 0; ky < k; ++ky)
                            for (i32 kx = 0; kx < k; ++kx) {
                                const i32 iy = static_cast<i32>(oy) * s - pad + ky;
                                const i32 ix = static_cast<i32>(ox) * s - pad + kx;
                                const bool inside = iy >= 0 && iy < ih && ix >= 0 && ix < iw;
                                const f32 x = inside ? plane[static_cast<usize>(iy) * iw + ix] : 0.0f;
                                acc += wk[ky * k + kx] * x;
                            }
                    }
                    out[((static_cast<usize>(n) * os.c + co) * os.h + oy) * os.w + ox] = activate(L.act, acc);
                }
}

// HLSL twins: CSConvBackwardData (dX, gather form) and CSConvBackwardWeights (dW / db; `tiled` is the
// GPU partial order, otherwise one plain running sum). dz is the gradient w.r.t. the pre-activation.
void layerBackward(const ConvLayerDesc& L, const f32* W, const TensorShape& is, const f32* x,
                   const TensorShape& os, const f32* dz, f32* gW, f32* gB, f32* dX, bool tiled) {
    const i32 k = static_cast<i32>(L.kernel), s = static_cast<i32>(L.stride), pad = padOf(L.kernel);
    const i32 ih = static_cast<i32>(is.h), iw = static_cast<i32>(is.w);
    const i32 oh = static_cast<i32>(os.h), ow = static_cast<i32>(os.w);
    const u32 tilesY = (os.h + kConvTile - 1u) / kConvTile, tilesX = (os.w + kConvTile - 1u) / kConvTile;

    // Adds one record's term(y, x) into the running sum g: plain row-major, or per 8x8 tile (the partial)
    // and then partial by partial, in the order the contract fixes.
    auto addRecord = [&](f32& g, auto&& term) {
        if (tiled) {
            for (u32 ty = 0; ty < tilesY; ++ty)
                for (u32 tx = 0; tx < tilesX; ++tx) {
                    f32 part = 0.0f;
                    const u32 y1 = std::min(os.h, (ty + 1u) * kConvTile), x1 = std::min(os.w, (tx + 1u) * kConvTile);
                    for (u32 y = ty * kConvTile; y < y1; ++y)
                        for (u32 xx = tx * kConvTile; xx < x1; ++xx) part += term(y, xx);
                    g += part;
                }
        } else {
            for (u32 y = 0; y < os.h; ++y)
                for (u32 xx = 0; xx < os.w; ++xx) g += term(y, xx);
        }
    };

    for (u32 co = 0; co < os.c; ++co) {
        for (u32 ci = 0; ci < is.c; ++ci)
            for (i32 ky = 0; ky < k; ++ky)
                for (i32 kx = 0; kx < k; ++kx) {
                    f32 g = 0.0f;
                    for (u32 n = 0; n < os.n; ++n) {
                        const f32* dzp = dz + (static_cast<usize>(n) * os.c + co) * os.planeCount();
                        const f32* xp = x + (static_cast<usize>(n) * is.c + ci) * is.planeCount();
                        addRecord(g, [&](u32 y, u32 xx) {
                            const i32 iy = static_cast<i32>(y) * s - pad + ky;
                            const i32 ix = static_cast<i32>(xx) * s - pad + kx;
                            const bool inside = iy >= 0 && iy < ih && ix >= 0 && ix < iw;
                            const f32 xv = inside ? xp[static_cast<usize>(iy) * iw + ix] : 0.0f;
                            return dzp[static_cast<usize>(y) * os.w + xx] * xv;
                        });
                    }
                    gW[(static_cast<usize>(co) * L.cin + ci) * static_cast<usize>(k * k) + ky * k + kx] = g;
                }
        if (gB) {
            f32 g = 0.0f;
            for (u32 n = 0; n < os.n; ++n) {
                const f32* dzp = dz + (static_cast<usize>(n) * os.c + co) * os.planeCount();
                addRecord(g, [&](u32 y, u32 xx) { return dzp[static_cast<usize>(y) * os.w + xx]; });
            }
            gB[co] = g;
        }
    }

    if (!dX) return;
    for (u32 n = 0; n < is.n; ++n)
        for (u32 ci = 0; ci < is.c; ++ci)
            for (i32 iy = 0; iy < ih; ++iy)
                for (i32 ix = 0; ix < iw; ++ix) {
                    f32 acc = 0.0f;
                    for (u32 co = 0; co < os.c; ++co) {
                        const f32* wk = W + (static_cast<usize>(co) * L.cin + ci) * static_cast<usize>(k * k);
                        const f32* dzp = dz + (static_cast<usize>(n) * os.c + co) * os.planeCount();
                        for (i32 ky = 0; ky < k; ++ky)
                            for (i32 kx = 0; kx < k; ++kx) {
                                const i32 ty = iy + pad - ky, tx = ix + pad - kx;
                                if (ty < 0 || tx < 0 || (ty % s) != 0 || (tx % s) != 0) continue;
                                const i32 oy = ty / s, ox = tx / s;
                                if (oy >= oh || ox >= ow) continue;
                                acc += wk[ky * k + kx] * dzp[static_cast<usize>(oy) * os.w + ox];
                            }
                    }
                    dX[((static_cast<usize>(n) * is.c + ci) * is.h + iy) * is.w + ix] = acc;
                }
}

bool inputOk(const ConvNetDesc& d, bool valid, const TensorShape& in) {
    return valid && in.c == d.inChannels && in.n >= 1 && in.h >= 1 && in.w >= 1;
}

// Forward over all layers with weights `w`; acts[l] is layer l's post-activation output.
void forwardActs(const ConvNetDesc& d, const ConvLayout& lay, const f32* w, const TensorShape& in,
                 const f32* input, Acts& acts) {
    acts.resize(lay.layers);
    TensorShape cur = in;
    const f32* src = input;
    for (u32 l = 0; l < lay.layers; ++l) {
        const TensorShape os = lay.outDims(l, cur);
        acts[l].assign(os.count(), 0.0f);
        forwardLayer(d.layers[l], w + lay.wOffset[l], d.layers[l].bias ? w + lay.bOffset[l] : nullptr, cur, src,
                     os, acts[l].data());
        cur = os;
        src = acts[l].data();
    }
}

// HLSL twins: CSConvActBackward + the per-layer backward kernels, last layer to first.
void backwardImpl(const ConvNetDesc& d, const ConvLayout& lay, const f32* w, const TensorShape& in,
                  const f32* input, const Acts& acts, const f32* dOut, f32* gradW, usize gradCount,
                  std::vector<f32>* dIn, bool tiled) {
    std::fill(gradW, gradW + gradCount, 0.0f);
    TensorShape shapes[kConvMaxLayers + 1];
    shapes[0] = in;
    for (u32 l = 0; l < lay.layers; ++l) shapes[l + 1] = lay.outDims(l, shapes[l]);

    std::vector<f32> dy(dOut, dOut + shapes[lay.layers].count());
    for (u32 l = lay.layers; l-- > 0;) {
        const ConvLayerDesc& L = d.layers[l];
        const TensorShape& is = shapes[l];
        const TensorShape& os = shapes[l + 1];
        const f32* y = acts[l].data();
        const f32* x = l > 0 ? acts[l - 1].data() : input;

        std::vector<f32> dz(os.count());
        for (usize i = 0; i < dz.size(); ++i)
            dz[i] = (L.act == Activation::ReLU) ? (y[i] > 0.0f ? dy[i] : 0.0f) : dy[i];

        std::vector<f32> dx;
        if (l > 0 || dIn) dx.resize(is.count());
        layerBackward(L, w + lay.wOffset[l], is, x, os, dz.data(), gradW + lay.wOffset[l],
                      L.bias ? gradW + lay.bOffset[l] : nullptr, dx.empty() ? nullptr : dx.data(), tiled);
        dy = std::move(dx);
    }
    if (dIn) *dIn = std::move(dy);
}

// L = sum pw * (p - t)^2 (fp64), and when dOut is given dL/dp = (2 * pw * (p - t)) / lossNorm (fp32).
f64 weightedLoss(const TensorShape& os, const f32* p, const f32* t, const f32* pw, f32 lossNorm, f32* dOut) {
    f64 sum = 0.0;
    for (u32 n = 0; n < os.n; ++n)
        for (u32 c = 0; c < os.c; ++c)
            for (u32 y = 0; y < os.h; ++y)
                for (u32 x = 0; x < os.w; ++x) {
                    const usize i = ((static_cast<usize>(n) * os.c + c) * os.h + y) * os.w + x;
                    const f32 w = pw[(static_cast<usize>(n) * os.h + y) * os.w + x];
                    const f64 e = static_cast<f64>(p[i]) - static_cast<f64>(t[i]);
                    sum += static_cast<f64>(w) * e * e;
                    if (dOut) dOut[i] = (2.0f * w * (p[i] - t[i])) / lossNorm;
                }
    return sum / static_cast<f64>(lossNorm);
}

}  // namespace

u32 convSharedBytes(const ConvLayerDesc& l) {
    return 4u * kConvCoBlock * std::min(l.cin, kConvCiChunk) * l.kernel * l.kernel;
}

bool validate(const ConvNetDesc& d, std::string* why) {
    if (d.inChannels < 1 || d.inChannels > kConvMaxChannels) return fail(why, "inChannels must be in [1, 64]");
    if (d.layers.empty() || d.layers.size() > kConvMaxLayers) return fail(why, "layer count must be in [1, 8]");
    u32 prev = d.inChannels;
    for (const ConvLayerDesc& L : d.layers) {
        if (L.cin != prev) return fail(why, "layer cin must equal the previous layer's cout (inChannels for the first)");
        if (L.cin < 1 || L.cin > kConvMaxChannels) return fail(why, "cin must be in [1, 64]");
        if (L.cout < 4 || L.cout > kConvMaxChannels || (L.cout % 4) != 0)
            return fail(why, "cout must be a multiple of 4 in [4, 64]");
        if (L.kernel != 1 && L.kernel != 3) return fail(why, "kernel must be 1 or 3");
        if (L.stride != 1 && L.stride != 2) return fail(why, "stride must be 1 or 2");
        if (L.kernel == 1 && L.stride != 1) return fail(why, "a 1x1 layer needs stride 1");
        if (L.act != Activation::None && L.act != Activation::ReLU) return fail(why, "activation must be None or ReLU");
        if (convSharedBytes(L) > kConvSharedLimitBytes) return fail(why, "layer weight block exceeds 16 KB of groupshared");
        prev = L.cout;
    }
    return true;
}

OptimiserDesc convDefaults() {
    OptimiserDesc o;
    o.gradFixedScale = 16777216.0f;
    o.gradClamp = 32.0f;
    return o;
}

ConvLayout ConvLayout::make(const ConvNetDesc& d) {
    ConvLayout L;
    L.layers = static_cast<u32>(std::min<usize>(d.layers.size(), kConvMaxLayers));
    u32 offset = 0;
    for (u32 l = 0; l < L.layers; ++l) {
        const ConvLayerDesc& c = d.layers[l];
        L.cin[l] = c.cin;
        L.cout[l] = c.cout;
        L.kernel[l] = c.kernel;
        L.stride[l] = c.stride;
        const u32 wCount = c.cout * c.cin * c.kernel * c.kernel;
        L.wOffset[l] = offset;
        L.bOffset[l] = offset + wCount;
        L.layerSize[l] = wCount + (c.bias ? c.cout : 0u);
        offset += L.layerSize[l];
    }
    L.total = offset;
    return L;
}

TensorShape ConvLayout::outDims(u32 layer, const TensorShape& in) const {
    if (layer >= layers) return in;
    return TensorShape{in.n, cout[layer], convOutSize(in.h, stride[layer]), convOutSize(in.w, stride[layer])};
}

std::vector<f32> initConvWeights(const ConvNetDesc& d) {
    const ConvLayout L = ConvLayout::make(d);
    std::vector<f32> w(L.total, 0.0f);   // biases and the head stay zero
    for (u32 l = 0; l + 1 < L.layers; ++l) {
        const u32 fanIn = L.cin[l] * L.kernel[l] * L.kernel[l];
        const f32 bound = std::sqrt(6.0f / static_cast<f32>(fanIn));
        const u32 n = L.cout[l] * fanIn;
        for (u32 k = 0; k < n; ++k) {
            const u32 idx = L.wOffset[l] + k;
            const f32 u = static_cast<f32>(initHash(d.seed * 0x9E3779B9u + idx) >> 8) * (1.0f / 16777216.0f);
            w[idx] = (2.0f * u - 1.0f) * bound;
        }
    }
    return w;
}

// ---------------------------------------------------------------- ConvNetReference

ConvNetReference::ConvNetReference(const ConvNetDesc& d, const OptimiserDesc& o) : desc_(d), opt_(o) {
    valid_ = validate(d) && validate(o);
    if (!valid_) return;
    layout_ = ConvLayout::make(d);
    w_ = initConvWeights(d);
    ema_ = w_;
    m_.assign(layout_.total, 0.0f);
    v_.assign(layout_.total, 0.0f);
    acc_.assign(layout_.total, 0);
}

bool ConvNetReference::setWeights(std::span<const f32> w) {
    if (!valid_ || w.size() != layout_.total) return false;
    w_.assign(w.begin(), w.end());
    ema_ = w_;
    std::fill(m_.begin(), m_.end(), 0.0f);
    std::fill(v_.begin(), v_.end(), 0.0f);
    std::fill(acc_.begin(), acc_.end(), 0);
    step_ = 0;
    return true;
}

TensorShape ConvNetReference::outputShape(const TensorShape& in) const {
    TensorShape s = in;
    for (u32 l = 0; l < layout_.layers; ++l) s = layout_.outDims(l, s);
    return s;
}

void ConvNetReference::forward(const TensorShape& in, std::span<const f32> input, std::span<f32> out,
                               bool useEma, std::vector<std::vector<f32>>* acts) const {
    if (!inputOk(desc_, valid_, in) || input.size() < in.count()) return;
    const TensorShape os = outputShape(in);
    if (out.size() < os.count()) return;
    Acts local;
    Acts& a = acts ? *acts : local;
    forwardActs(desc_, layout_, (useEma ? ema_ : w_).data(), in, input.data(), a);
    std::copy(a.back().begin(), a.back().end(), out.begin());
}

void ConvNetReference::backward(const TensorShape& in, std::span<const f32> input, std::span<const f32> dOut,
                                std::span<f32> gradW, std::vector<f32>* dIn) const {
    if (!inputOk(desc_, valid_, in) || input.size() < in.count() || gradW.size() < layout_.total) return;
    if (dOut.size() < outputShape(in).count()) return;
    Acts acts;
    forwardActs(desc_, layout_, w_.data(), in, input.data(), acts);
    backwardImpl(desc_, layout_, w_.data(), in, input.data(), acts, dOut.data(), gradW.data(), layout_.total, dIn,
                 false);
}

void ConvNetReference::backwardGpuOrder(const TensorShape& in, std::span<const f32> input,
                                        std::span<const f32> dOut, std::span<f32> gradW) const {
    if (!inputOk(desc_, valid_, in) || input.size() < in.count() || gradW.size() < layout_.total) return;
    if (dOut.size() < outputShape(in).count()) return;
    Acts acts;
    forwardActs(desc_, layout_, w_.data(), in, input.data(), acts);
    backwardImpl(desc_, layout_, w_.data(), in, input.data(), acts, dOut.data(), gradW.data(), layout_.total,
                 nullptr, true);
}

f32 ConvNetReference::accumulateBatch(const TensorShape& in, std::span<const f32> input,
                                      std::span<const f32> target, std::span<const f32> posWeight, f32 lossNorm) {
    if (!inputOk(desc_, valid_, in) || input.size() < in.count() || !(lossNorm > 0.0f)) return 0.0f;
    const TensorShape os = outputShape(in);
    if (target.size() < os.count() || posWeight.size() < static_cast<usize>(os.n) * os.planeCount()) return 0.0f;

    Acts acts;
    forwardActs(desc_, layout_, w_.data(), in, input.data(), acts);
    std::vector<f32> dOut(os.count());
    const f64 loss = weightedLoss(os, acts.back().data(), target.data(), posWeight.data(), lossNorm, dOut.data());

    std::vector<f32> grad(layout_.total);
    backwardImpl(desc_, layout_, w_.data(), in, input.data(), acts, dOut.data(), grad.data(), grad.size(), nullptr,
                 true);
    for (u32 k = 0; k < layout_.total; ++k) acc_[k] = quantise(grad[k], opt_);
    return static_cast<f32>(loss);
}

void ConvNetReference::adamStep() {
    if (!valid_) return;
    ++step_;
    ::aver::render::neural::adamStep(w_, ema_, m_, v_, acc_, opt_, step_, 1u);
}

f32 ConvNetReference::trainBatch(const TensorShape& in, std::span<const f32> input, std::span<const f32> target,
                                 std::span<const f32> posWeight, f32 lossNorm) {
    const f32 loss = accumulateBatch(in, input, target, posWeight, lossNorm);
    adamStep();
    return loss;
}

f32 ConvNetReference::evaluate(const TensorShape& in, std::span<const f32> input, std::span<const f32> target,
                               std::span<const f32> posWeight, f32 lossNorm, bool useEma) const {
    if (!inputOk(desc_, valid_, in) || input.size() < in.count() || !(lossNorm > 0.0f)) return 0.0f;
    const TensorShape os = outputShape(in);
    if (target.size() < os.count() || posWeight.size() < static_cast<usize>(os.n) * os.planeCount()) return 0.0f;
    Acts acts;
    forwardActs(desc_, layout_, (useEma ? ema_ : w_).data(), in, input.data(), acts);
    return static_cast<f32>(weightedLoss(os, acts.back().data(), target.data(), posWeight.data(), lossNorm, nullptr));
}

}  // namespace aver::render::neural
