
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
