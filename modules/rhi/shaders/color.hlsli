
// ACES filmic tonemap fit.
//
// CLAMPED AT ZERO FIRST, AND THAT IS A CORRECTNESS FIX RATHER THAN TIDINESS. The fit is a ratio of
// two quadratics and it is SIGN-BLIND: feed it a negative and the numerator's x*(2.51x + 0.03) goes
// POSITIVE as soon as x < -0.012, while the denominator's 2.43x^2 + 0.59x + 0.14 has no real root
// and stays positive throughout. So the ratio climbs toward 2.51/2.43 = 1.033 and saturates.
//
//     acesTonemap(-1.0) = (-1 * -2.48) / (-1 * -1.84 + 0.14) = 2.48 / 1.98 = 1.25 -> 1.0
//
// A channel at -1 comes out at FULL BRIGHTNESS. Negative radiance is unphysical, so it is always a
// bug upstream -- but it used to be a bug that rendered as a saturated, confident-looking HUE
// instead of as something obviously broken. That is exactly how a misregistered refraction
// subtraction (see averBlendedOutputBackdrop) showed up as magenta blocks in a swimming pool whose
// material can only ever tint cyan, and it cost hours: the colour looked like a shading decision,
// not like arithmetic that had gone below zero.
//
// max(x, 0) makes the same failure render BLACK, which reads as broken on sight. It cannot change
// any valid frame, because valid radiance is already non-negative -- verified by re-rendering the
// pool and glass cameras after this change and getting a bit-identical image.
float3 acesTonemap(float3 x){ x = max(x, 0.0); return saturate((x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14)); }
// ---- THE SAME CURVE, WITH THE COLOUR SCIENCE PUT BACK (Stephen Hill's ACES fit) ---------------
//
// WHAT IS WRONG WITH THE ONE ABOVE, and it is not the curve: acesTonemap applies a scalar S-curve to
// R, G and B INDEPENDENTLY. A saturated colour has channels of very different magnitude, the large
// one compresses hard while the small one barely moves, and the ratio between them collapses -- so
// everything bright drifts toward white. That is the "washed out" this engine has been described as
// having, and no amount of exposure tuning fixes it, because the desaturation happens INSIDE the
// curve rather than before it.
//
// The real ACES pipeline does not apply its curve in the rendering primaries at all. It rotates into
// a working space where the tone curve behaves, applies it, and rotates back -- and those two
// rotations are what preserve the hue and the saturation. The matrices below are exactly that pair,
// with the RRT and ODT saturation folded in, from Stephen Hill's widely used fit.
//
// mul(M, v) AND NOT mul(v, M), which is the opposite of this engine's convention everywhere else.
// HLSL's mul treats the second argument as a COLUMN vector, which is the orientation these matrices
// are written in; the row-vector convention the geometry code uses (see averTransformNormal) applies
// to transforms authored that way, not to these. Swapping them silently transposes the colour
// rotation and produces a plausible-looking but wrong hue shift.
static const float3x3 kAcesInput = float3x3(
    0.59719, 0.35458, 0.04823,
    0.07600, 0.90834, 0.01566,
    0.02840, 0.13383, 0.83777);
static const float3x3 kAcesOutput = float3x3(
     1.60475, -0.53108, -0.07367,
    -0.10208,  1.10813, -0.00605,
    -0.00327, -0.07276,  1.07602);

float3 averRrtOdtFit(float3 v) {
    float3 a = v * (v + 0.0245786) - 0.000090537;
    float3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}

// SAME max(x, 0) GUARD AND THE SAME REASON as acesTonemap above -- the fit is a ratio of quadratics
// here too, and a negative channel would climb rather than clamp. The output saturate() is what
// bounds the result; the input guard is what stops a bug upstream rendering as a confident colour.
float3 acesFittedTonemap(float3 x) {
    x = max(x, 0.0);
    // TWO, AND IT IS DERIVED RATHER THAN TASTE. The two fits do not agree on where middle grey lands:
    // acesTonemap(0.18) = 0.267 while this one, ungained, gives 0.106 -- the matrixed form is close to
    // a stop darker, so swapping curves would darken every frame by ~40% and read as a regression
    // rather than as better colour. Solving acesFittedTonemap(k * 0.18) == acesTonemap(0.18) gives
    // k = 2.010, and 2 then tracks the old curve across the WHOLE range, not just at grey:
    //
    //     input   0.05    0.18    0.50    1.00    2.00    4.00
    //     old    0.0443  0.2669  0.6163  0.8038  0.9149  0.9734
    //     new    0.0425  0.2653  0.6191  0.8036  0.9090  0.9630
    //
    // So this changes SATURATION and hue handling and leaves brightness and highlight rolloff where
    // they were, which is the whole point: the complaint was washed-out colour, not exposure.
    x *= 2.0;
    x = mul(kAcesInput, x);
    x = averRrtOdtFit(x);
    x = mul(kAcesOutput, x);
    return saturate(x);
}

// LUMINANCE-ONLY TONEMAPPING, which is the only one of the three that survives a large exposure.
//
// WHAT THE OTHER TWO CANNOT DO. Both curves above map each channel through an S-curve whose shoulder
// flattens toward 1. Multiply the scene by enough and every channel lands on that flat part, so a
// warm stone at (0.9, 0.7, 0.5) and a cold one at (0.5, 0.7, 0.9) both come out near white: the
// curve has compressed away the very differences that made them different colours. It is not a bug
// in either fit -- it is what a shoulder IS -- and it is why an under-lit scene that needs a big
// exposure to be readable arrives on screen grey. MEASURED on PTTest Sponza, chroma (mean R-B) by
// exposure through acesFittedTonemap: 1x -> 3.32, 2x -> 4.47, 5x -> 3.28, 8x -> 1.40.
//
// WHAT THIS DOES INSTEAD: tonemap the LUMINANCE, then put the original chromaticity back by scaling
// the colour by the ratio the luminance moved. Hue and saturation are preserved exactly, by
// construction, at any exposure -- the curve is asked only to decide how BRIGHT a pixel is, which is
// the question it can actually answer, and never what colour it is.
//
// THE DESATURATION TERM IS NOT OPTIONAL AND IS NOT TASTE. Preserving chromaticity perfectly means a
// pixel whose luminance maps to near 1 keeps its full saturation, and a fully saturated colour at
// luminance 1 has channels outside [0,1] -- so it clips, and clipping is where hue shifts and
// banding come from. Real film desaturates as it approaches the highlight for exactly this reason.
// So the chromaticity is blended toward white as tonemapped luminance approaches 1, which keeps the
// result in gamut while leaving everything below the highlight untouched. The blend is on Lo^2 so it
// stays out of the way through the whole midtone range and only arrives at the very top.
//
// NOT THE DEFAULT, and deliberately: every recorded gate baseline in scripts/ was measured through
// mode 0 or 1, and this changes colour everywhere the shoulder was doing work. It is offered as
// mode 2 (--tonemap 2) so it can be judged against the other two on the same frame.
float3 acesLumaTonemap(float3 x) {
    x = max(x, 0.0);
    // The same 2.0 gain acesFittedTonemap applies, and for the same derived reason: without it this
    // curve puts middle grey about a stop below where the other two put it, so a comparison between
    // modes would be measuring brightness rather than colour. See that function's own table.
    x *= 2.0;
    // The Rec.709 coefficients averLuminance uses, written out because that helper is DECLARED
    // BELOW this function and HLSL has no forward declarations. Kept identical to it on purpose;
    // if one ever changes, change both.
    const float Lin = max(dot(x, float3(0.2126, 0.7152, 0.0722)), 1e-6);
    // The scalar form of the same RRT/ODT fit the matrixed curve uses, so the three modes agree
    // about brightness and differ only in what they do to colour.
    const float Lo  = saturate(averRrtOdtFit(Lin.xxx).x);
    // The ratio, not a re-normalisation: multiplying by Lo/Lin moves the pixel along its own
    // chromaticity line to the new luminance and does nothing else.
    float3 c = x * (Lo / Lin);
    // DESATURATE ONLY AS MUCH AS THE GAMUT DEMANDS, AND NOT A DROP MORE.
    //
    // This was `lerp(c, Lo, saturate(Lo*Lo))`, which is wrong in a way that only shows at high
    // exposure: Lo*Lo is already 0.64 at Lo = 0.8, so it blended nearly two thirds of the way to
    // grey in the MIDTONES, not in the highlights. MEASURED on Sponza -- chroma (mean R-B) at
    // exposure 8/16/32 was 2.37 / 0.82 / -0.98, i.e. the curve turned the picture grey and then
    // BLUE exactly as the old per-channel shoulder did. It reintroduced the defect this mode was
    // written to remove.
    //
    // The only real constraint is that no channel may leave [0,1]. Blending toward the neutral Lo
    // by t gives max channel m + t(Lo - m), so setting that to 1 gives the exact t that brings the
    // brightest channel to white and leaves everything dimmer than that completely untouched. A
    // pixel whose channels already fit keeps its full saturation at any exposure, which is the
    // entire point of tonemapping luminance in the first place.
    const float m = max(max(c.r, c.g), c.b);
    if (m > 1.0) c = lerp(c, Lo.xxx, saturate((m - 1.0) / max(m - Lo, 1e-4)));
    return saturate(c);
}

// Picks the curve. `mode` rides PostCB::misc.w; 0 is the original per-channel fit, kept because it is
// what every recorded gate baseline was measured against, 1 is the matrixed fit above, and 2 is the
// luminance-only form that holds its colour under a large exposure.
float3 averTonemap(float3 x, float mode) {
    if (mode > 1.5) return acesLumaTonemap(x);
    return mode > 0.5 ? acesFittedTonemap(x) : acesTonemap(x);
}

// Encodes linear colour to gamma 2.2.
float3 toGamma(float3 c){ return pow(max(c,0.0), 1.0/2.2); }
// Decodes gamma-2.2 colour to linear.
float3 srgbToLin(float3 c){ return pow(max(c,0.0), 2.2); }

// The exact inverse of acesTonemap, for colours authored for display rather than as radiance.
float3 averInverseTonemap(float3 y) {
    y = clamp(y, 0.0, 1.0329 - 1e-4);
    float3 a = 2.43 * y - 2.51;
    float3 b = 0.59 * y - 0.03;
    float3 c = 0.14 * y;
    float3 disc = sqrt(max(b * b - 4.0 * a * c, 0.0));
    return (-b - disc) / (2.0 * a);
}

// Rec. 709 relative luminance.
float averLuminance(float3 c){ return dot(c, float3(0.2126, 0.7152, 0.0722)); }
