// The HLSL every GPU consumer shares: the constant-buffer layouts the backend uploads, the vertex
// structures it feeds, the colour-space helpers, the sky and the camera post.
#include "aver/rhi/RHIResources.hpp"
#include "aver/rhi/RHI.hpp"

#include <cmath>
#include <string>

namespace aver::rhi {

// Writes sunDirection from an elevation and an azimuth bearing, both in degrees.
void SkyAtmosphere::setSunAngles(f32 elevationDeg, f32 azimuthDeg) {
    constexpr f32 kDeg = 3.14159265358979f / 180.0f;
    const f32 el = elevationDeg * kDeg, az = azimuthDeg * kDeg;
    const f32 ce = std::cos(el);
    sunDirection[0] = ce * std::cos(az);
    sunDirection[1] = ce * std::sin(az);
    sunDirection[2] = std::sin(el);
}

// Reads sunDirection back as an elevation and an azimuth bearing, both in degrees.
void SkyAtmosphere::sunAngles(f32& elevationDeg, f32& azimuthDeg) const {
    constexpr f32 kRad = 180.0f / 3.14159265358979f;
    const f32 x = sunDirection[0], y = sunDirection[1], z = sunDirection[2];
    const f32 len = std::sqrt(x * x + y * y + z * z);
    if (len < 1e-6f) { elevationDeg = 0.0f; azimuthDeg = 0.0f; return; }
    elevationDeg = std::asin(z / len) * kRad;
    azimuthDeg = std::atan2(y, x) * kRad;
}

// Converts a colour temperature in Kelvin to linear sRGB, normalised so the brightest channel is 1.
void blackbodySrgb(f32 kelvin, f32 outRgb[3]) {
    const f32 t = kelvin < 1000.0f ? 1000.0f : (kelvin > 15000.0f ? 15000.0f : kelvin);
    const f32 t2 = t * t;
    const f32 u = (0.860117757f + 1.54118254e-4f * t + 1.28641212e-7f * t2) /
                  (1.0f + 8.42420235e-4f * t + 7.08145163e-7f * t2);
    const f32 v = (0.317398726f + 4.22806245e-5f * t + 4.20481691e-8f * t2) /
                  (1.0f - 2.89741816e-5f * t + 1.61456053e-7f * t2);
    const f32 d = 2.0f * u - 8.0f * v + 4.0f;
    const f32 x = 3.0f * u / d;
    const f32 y = 2.0f * v / d;
    const f32 z = 1.0f - x - y;

    const f32 Y = 1.0f;
    const f32 X = (y > 1e-6f) ? (Y / y) * x : 0.0f;
    const f32 Z = (y > 1e-6f) ? (Y / y) * z : 0.0f;

    f32 rgb[3] = {
         3.2404542f * X - 1.5371385f * Y - 0.4985314f * Z,
        -0.9692660f * X + 1.8760108f * Y + 0.0415560f * Z,
         0.0556434f * X - 0.2040259f * Y + 1.0572252f * Z,
    };
    f32 m = 0.0f;
    for (f32 c : rgb) if (c > m) m = c;
    for (int i = 0; i < 3; ++i) {
        const f32 c = m > 1e-6f ? rgb[i] / m : 1.0f;
        outRgb[i] = c < 0.0f ? 0.0f : c;
    }
}

namespace {

// Colour space and tonemapping, shared by the scene prelude and the post chain.
const char* kColorHlsl = R"(
// ACES filmic tonemap fit.
float3 acesTonemap(float3 x){ return saturate((x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14)); }
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
)";

} // namespace

// The HLSL below hardcodes these; the C++ side reads the constants. Neither can move alone.
static_assert(kMeshShaderTrisPerGroup == 64, "AVER_MS_TRIS in the prelude is written out as 64");
static_assert(kMeshGeometryConstantRegister == 5, "MeshCB in the prelude is written out as b5");
static_assert(kObjectConstantRegister == 1, "PerObject in the prelude is written out as b1");
static_assert(kObjectConstantDwords == 32,
              "PerObject below is 32 dwords: world 16, base colour 4, material 4, model 4, emissive 4");
static_assert(kClusterAmplificationGroupSize == 32, "AVER_MSC_GROUP in the cluster shader is written out as 32");
static_assert(kFeatureFrameConstantRegister == 4, "ClusterFrameCB in the cluster shader is written out as b4");

// The shared HLSL prelude. Composed once and cached for the life of the process.
const char* sharedShaderPrelude() {
    static const std::string s = std::string(kColorHlsl) + R"(
// The engine's per-frame block. MIRRORS PerFrameCB field for field.
cbuffer PerFrame : register(b0) {
    float4x4 gViewProj;
    float4x4 gInvViewProj;
    float4   gCamPos;      // xyz
    float4   gLightDir;    // xyz = direction TO light
    float4   gLightColor;  // rgb
    float4   gAmbient;     // rgb
    float4   gSkyZenith;   // rgb
    float4   gSkyHorizon;  // rgb
    float4   gFogColor;    // rgb, a = density at gFogParams.y
    // ---- the authored atmosphere ----
    float4   gSkyParams;   // x atmosphere height, y sky-light intensity, z sun intensity, w cos(sun radius)
    float4   gGroundColor; // rgb below the horizon, a = how much of it replaces the sky
    float4   gFogParams;   // x height falloff, y fog height, z start distance, w max opacity
    // Fed every frame regardless of consumer, same as the fog/atmosphere fields around them, but
    // the RAYMARCH that reads them does NOT live in this shared prelude the way fog/atmosphere's
    // own math does: unlike fog and sky ambient, which ordinary surface shading genuinely needs
    // (averApplyFog, averSkyIrradiance -- called from Voxi, PBR, everywhere), nothing outside the
    // sky pass itself ever reads cloud data, so the cloud functions live in D3D12Device.cpp's
    // kShaderHLSL, compiled only into PSky, not pasted into every module's shader the way they
    // used to be. These two fields are the one remaining trace of clouds in the universal prelude,
    // kept here rather than split into their own cbuffer because moving them means every
    // consumer's PerFrameCB-mirroring C++ struct has to agree on a new layout in lockstep -- real
    // but separable follow-up work, not bundled into the same change as the function move.
    float4   gCloudParams; // x coverage, y density, z layer bottom, w layer top
    float4   gCloudMotion; // xy wind offset, z 1/feature size, w enabled
    // ---- the PHYSICAL atmosphere (rhi::AtmosphereProfile) ----
    float4   gAtmoRayleigh; // rgb scattering per km, w scale height km
    float4   gAtmoMie;      // x scatter, y extinction, z scale height km, w phase g
    float4   gAtmoOzone;    // rgb absorption per km, w tent half-width km
    float4   gAtmoPlanet;   // x planet radius km, y atmosphere top radius km, z world->km, w on/off
    float4   gAtmoTune;     // x ozone centre km, y multi-scatter gain, z view steps, w aerial steps
    float4   gAtmoSunE0;    // rgb sun irradiance ABOVE the air, w ground albedo
    // averFogInscatterRef()'s answer, baked ONCE PER FRAME on the CPU (Atmosphere.cpp's mirror of
    // this same function) rather than marched per pixel -- see averFogInscatterRef below for why
    // that is exactly correct and not an approximation: the function has no view-direction or
    // world-position input, so every pixel that used to compute it was computing the same answer.
    float4   gFogInscatterRef; // rgb; w unused
    // x > 0.5 puts the shading model in a WHITE FURNACE: every direction carries radiance y and the
    // ground carries it too. z > 0.5 is the SUN-ON variant: the environment drops to zero and the
    // sun stays lit, which is the only configuration in which the direct term is under test at all.
    // A measuring instrument, not a look -- see averFurnaceOn.
    float4   gFurnace;
    // Nine L2 spherical-harmonic coefficients of the sky, rgb, w unused. Baked once per frame on
    // the CPU by atmoSkyRadianceSH -- the same division of labour gFogInscatterRef uses, and for
    // the same reason: nothing about them varies per pixel. Only written while the PHYSICAL
    // atmosphere is on; averSkyIrradiance reads them only under averAtmoOn() for that reason.
    float4   gSkySh[9];
};
// The per-draw block: transform plus shading constants. 32 dwords, matching kObjectConstantDwords.
cbuffer PerObject : register(b1) {
    float4x4 gWorld;
    float4   gBaseColor;
    float4   gMaterial;      // x=metallic, y=roughness, z=unlit(0/1) for plainShadeSurface only
    uint     gShadingModel;  // which shading model evaluates this draw
    // Unread. Declared so kObjectConstantDwords and the root signatures at b1 do not move.
    float    gReflectance;
    float    gF90;
    float    _objPad;
    float4   gEmissive;      // rgb, radiance this surface emits on its own
};

static const float PI = 3.14159265;

// The sun's radiance: the authored colour, decoded, times the authored intensity.
// Whether the shading model is in furnace mode, and the radiance every direction then carries.
//
// A WHITE FURNACE is the standard energy oracle: put a surface of albedo 1 inside a uniform
// environment of radiance L and its outgoing radiance must be exactly L, from every direction, at
// every orientation, and -- the part that makes it sharp -- REGARDLESS OF GEOMETRY. A surface
// enclosed by other surfaces must still read L, because the enclosure is emitting L as well. Any
// missing or double-counted factor shows up as a ratio: dropping 1/PI reads PI*L, dropping the
// cosine reads 2L, applying an occlusion term that forgets the occluder is also a light source
// reads darker.
bool  averFurnaceOn() { return gFurnace.x > 0.5; }
// Declared HERE rather than beside averSunRadiance where it is conceptually at home, because
// averFurnaceL calls it and HLSL has no forward declarations -- a use-before-declaration compiles
// in C++ and fails in DXC, at RUNTIME, on a build that reported success.
bool  averFurnaceSun() { return gFurnace.z > 0.5; }
// Zero in the sun-on variant: an environment carrying radiance would add an ambient term and
// the direct term would no longer be alone in the answer.
float averFurnaceL()  { return averFurnaceSun() ? 0.0 : gFurnace.y; }

// The sun is OFF in the plain furnace: a directional source is not part of a uniform environment,
// and leaving it lit would swamp the very thing being measured. In the SUN-ON variant it is the
// only thing lit.
float3 averSunRadiance() {
    if (averFurnaceOn() && !averFurnaceSun()) return 0.0;
    return srgbToLin(gLightColor.rgb) * gSkyParams.z;
}

// ---- the physical atmosphere -----------------------------------------------------------------
// MIRRORS modules/rhi/src/Atmosphere.cpp function for function.

// Whether the physical atmosphere is switched on.
bool averAtmoOn() { return gAtmoPlanet.w > 0.5; }

// sin from cos, guarded against a cosine that drifted outside [-1,1].
float averAtmoSin(float c) { return sqrt(saturate(1.0 - c * c)); }

// exp(y*y)*erfc(y) for y >= 0, from Numerical Recipes' Chebyshev fit.
float averAtmoErfcx(float y) {
    float t = 2.0 / (2.0 + max(y, 0.0));
    float p = -1.26551223 + t * (1.00002368 + t * (0.37409196 + t * (0.09678418 +
               t * (-0.18628806 + t * (0.27886807 + t * (-1.13520398 + t * (1.48851587 +
               t * (-0.82215223 + t * 0.17087277))))))));
    return t * exp(p);
}

// Chapman airmass: the slant column as a multiple of the vertical column at that altitude.
float averAtmoChapman(float xr, float cosZenith) {
    float half = sqrt(xr * 0.5);
    float up   = sqrt(PI * xr * 0.5);
    if (cosZenith >= 0.0) return up * averAtmoErfcx(cosZenith * half);
    float sinChi = averAtmoSin(cosZenith);
    return 2.0 * sqrt(PI * xr * sinChi * 0.5) * exp(xr * (1.0 - sinChi)) -
           up * averAtmoErfcx(-cosZenith * half);
}

// Unit-peak density of the ozone tent at an altitude.
float averAtmoOzoneDensity(float altKm) {
    return saturate(1.0 - abs(altKm - gAtmoTune.x) / max(gAtmoOzone.w, 1e-4));
}

// Vertical column of the ozone tent from altKm upward, in km.
float averAtmoOzoneColumn(float altKm) {
    float w = max(gAtmoOzone.w, 1e-4), c = gAtmoTune.x;
    if (altKm >= c + w) return 0.0;
    if (altKm >= c)     { float s = (c + w - altKm) / w; return w * s * s * 0.5; }
    if (altKm >= c - w) { float u = (c - altKm) / w;     return w * (u - u * u * 0.5) + w * 0.5; }
    return w;
}

// Slant factor for the ozone layer, treated as a thin shell and capped at tangency.
float averAtmoOzoneAirmass(float rKm, float cosZenith) {
    float w  = max(gAtmoOzone.w, 1e-4);
    float ro = gAtmoPlanet.x + gAtmoTune.x;
    float s  = rKm * averAtmoSin(cosZenith) / ro;
    float maxAir = 1.2 * sqrt(2.0 * ro * w) / w;
    float capped = clamp(1.0 / sqrt(1.0 - clamp(s * s, 0.0, 0.999999)), 1.0, maxAir);
    return cosZenith >= 0.0 ? capped : 2.0 * maxAir - capped;
}

// Per-channel optical depth from an altitude out to space. Returns a wall of extinction when the
// ray meets the planet first.
float3 averAtmoOpticalDepth(float altKm, float cosZenith) {
    float z = max(altKm, 0.0);
    float r = gAtmoPlanet.x + z;
    if (cosZenith < 0.0 && r * averAtmoSin(cosZenith) < gAtmoPlanet.x) return 1e9;

    float colR = gAtmoRayleigh.w * exp(-z / gAtmoRayleigh.w) * averAtmoChapman(r / gAtmoRayleigh.w, cosZenith);
    float colM = gAtmoMie.z      * exp(-z / gAtmoMie.z)      * averAtmoChapman(r / gAtmoMie.z, cosZenith);
    float colO = averAtmoOzoneColumn(z) * averAtmoOzoneAirmass(r, cosZenith);
    return gAtmoRayleigh.rgb * colR + gAtmoMie.y * colM + gAtmoOzone.rgb * colO;
}

// What the air leaves of the sun at an altitude. Zero in the planet's shadow, with the terminator
// softened across the sun's own angular radius.
float3 averAtmoSunTransmittance(float altKm, float sunCosZenith) {
    float z = max(altKm, 0.0);
    float ratio = gAtmoPlanet.x / (gAtmoPlanet.x + z);
    float cosHorizon = -sqrt(saturate(1.0 - ratio * ratio));
    float halfWidth = sqrt(saturate(1.0 - gSkyParams.w * gSkyParams.w)) + 1e-5;
    float shadow = smoothstep(cosHorizon - halfWidth, cosHorizon + halfWidth, sunCosZenith);
    if (shadow <= 0.0) return 0.0;
    return exp(-averAtmoOpticalDepth(z, max(sunCosZenith, cosHorizon))) * shadow;
}

// The scattering integral along a segment bounded by `tMaxKm`, for sky and aerial alike.
float3 averAtmoScatter(float r0, float cosV, float cosS, float cosVS, float tMaxKm, int steps,
                       out float3 transmittance) {
    transmittance = 1.0;
    float3 total = 0.0;
    if (tMaxKm <= 0.0) return total;

    float pR = 3.0 / (16.0 * PI) * (1.0 + cosVS * cosVS);
    float g  = gAtmoMie.w;
    float g2 = g * g;
    float dn = 1.0 + g2 - 2.0 * g * cosVS;
    float pM = 3.0 * (1.0 - g2) * (1.0 + cosVS * cosVS) /
               (8.0 * PI * (2.0 + g2) * max(dn * sqrt(max(dn, 1e-4)), 1e-6));

    float p = 1.0 + abs(cosV);
    float invN = 1.0 / steps;

    [loop] for (int i = 0; i < steps; ++i) {
        float lo = tMaxKm * pow(i * invN, p);
        float hi = tMaxKm * pow((i + 1) * invN, p);
        float dt = hi - lo;
        if (dt <= 0.0) continue;
        float t = 0.5 * (lo + hi);
        float r = sqrt(r0 * r0 + 2.0 * r0 * cosV * t + t * t);
        float alt = max(r - gAtmoPlanet.x, 0.0);
        float sunCosHere = (r0 * cosS + t * cosVS) / max(r, 1e-4);

        float dR = exp(-alt / gAtmoRayleigh.w);
        float dM = exp(-alt / gAtmoMie.z);
        float dO = averAtmoOzoneDensity(alt);

        float3 sunT  = averAtmoSunTransmittance(alt, sunCosHere);
        float3 scatR = gAtmoRayleigh.rgb * dR;
        float  scatM = gAtmoMie.x * dM;
        float3 ext   = scatR + gAtmoMie.y * dM + gAtmoOzone.rgb * dO;

        // TWO ATTEMPTS AT REBALANCING THE MULTI-SCATTER TERM'S CHROMATICITY WERE MADE AND REVERTED
        // HERE -- averLuminance(sunT) broke AtmosphereTest's fog-reference smoothness sweep, and
        // sunT.r (red channel only) broke a DIFFERENT sweep, the dome-fit exponent through sunrise.
        // Both changes were directionally correct (gAtmoTune.y is documented as "an isotropic
        // stand-in for multiple scattering," and multiplying that stand-in by the same single-bounce
        // sunT the direct phase term uses double-counts the sun's own airmass reddening) but this
        // function is shared by the sky, the fog reference, aerial perspective and every ordinary lit
        // surface in an atmoOn scene, each with its own smoothness contract, and two different fixes
        // each satisfying one broke another. See averSkyPhysical for where this idea landed instead --
        // applied locally to what the sky actually draws, not to the shared integral every physically
        // lit pixel in the scene depends on.
        float3 source = ((scatR * pR + scatM * pM) +
                         (scatR + scatM) * gAtmoTune.y / (4.0 * PI)) * sunT * gAtmoSunE0.rgb;
        float3 stepT = exp(-ext * dt);
        total += transmittance * source * (1.0 - stepT) / max(ext, 1e-12);
        transmittance *= stepT;
    }
    return total;
}

// The camera's altitude in kilometres.
//
// FLOORED AT A METRE RATHER THAN AT ZERO. At exactly zero the camera sits ON the planet surface, and
// the geometry below the horizon degenerates: the ray-sphere near root is t = 0 for EVERY downward
// direction, so nothing registers as hitting the ground, and the march instead runs the full
// atmosphere-shell length straight down THROUGH the planet, at a density clamped to sea level the
// whole way. That is what turned the lower half of the view into a bright, speckled field the moment
// a character fell past z = 0 -- which only became reachable when the implicit ground came out. A
// metre of standoff costs nothing above ground and keeps the intersection well-posed below it.
float averAtmoCamAlt()    { return max(gCamPos.z * gAtmoPlanet.z, 1e-3); }
// The camera's radius from the planet centre, in kilometres.
float averAtmoCamRadius() { return gAtmoPlanet.x + averAtmoCamAlt(); }

// Aerial perspective: the same integral bounded by the surface being shaded instead of by the sky.
float3 averAtmoAerial(float3 wpos, out float3 transmittance) {
    transmittance = 1.0;
    float3 v = wpos - gCamPos.xyz;
    float len = length(v);
    if (len < 1e-4) return 0.0;
    float3 dir = v / len;
    float3 L = normalize(gLightDir.xyz);
    return averAtmoScatter(averAtmoCamRadius(), dir.z, L.z, dot(dir, L),
                           len * gAtmoPlanet.z, (int)gAtmoTune.w, transmittance);
}

// The authored dome above the horizon: a horizon-to-zenith blend in linear light.
float3 averSkyAbove(float3 dir) {
    return lerp(srgbToLin(gSkyHorizon.rgb), srgbToLin(gSkyZenith.rgb),
                pow(saturate(dir.z * 0.5 + 0.5), gSkyParams.x));
}

// The reference sky colour that the fog and the dome's lower half both resolve to.
//
// TWO TERMS, NOT ONE, COMPUTED ONCE PER FRAME ON THE CPU, NOT PER PIXEL. Straight up is the ambient
// half. Blended with it: straight OUT along the sun's own azimuth, levelled to the true horizon --
// the half that actually answers "what colour is the light hitting the fog right now." See
// Atmosphere.cpp's atmoFogInscatterRef -- the ACTUAL implementation now lives there, mirrored from
// this comment's own reasoning below, and gFogInscatterRef is that function's answer for this frame.
//
// WHY THE ZENITH TERM ALONE NEVER ANSWERED THAT. bac1d87 blended fog toward the true view ray's
// colour once real atmosphere lay between the camera and the fogged point, using aerial-perspective
// transmittance as the gate. It works, but the gate never opens where fog actually is: that
// transmittance is governed by the REAL Rayleigh/Mie coefficients, which need tens of kilometres to
// move visibly, while a level's own FOG density is an authored, artistic knob almost always tuned to
// read as fog within a few hundred metres to a couple of kilometres. At every distance the exponential
// fog is actually doing anything, the blend sits at its zero end -- this reference, alone -- and this
// reference was a straight-up march: near-blind to the sun's AZIMUTH (a zenith ray can't tell "sun to
// my left" from "sun in front of me") and only weakly moved by its elevation. So the fog's colour was
// never really tracking the sun, at any distance a player would call "foggy."
//
// THE FIX ASKS A DIFFERENT QUESTION. Real fog scatters whatever light actually falls on it, and that
// depends on how far the SUN's own light has travelled to get here -- not on how far the CAMERA is
// looking. A horizontal ray toward the sun's azimuth is exactly the direction that path is longest:
// as the sun drops toward the horizon that march's own far distance (the atmosphere-shell exit)
// grows and the path reddens, at the sun's elevation, independent of the fog's distance from the
// camera. cosVS falls out of the geometry for free: dot(dirToSun, L) reduces to length(L.xy), the
// sine of the sun's OWN elevation -- near 0 (phase near its Rayleigh minimum) when the sun is high,
// near 1 (the Mie forward-scatter peak) as it nears the horizon, with no separate "how low is the
// sun" gate needed. The blend weight is a tuned constant: how much of that direct, sun-warmed light
// mixes into the ambient reference, since real multiple scattering (which this single-scatter model
// does not simulate) is what actually carries a sunset's warmth to fog that isn't looking straight
// at the sun.
//
// WHY THIS IS SAFE TO HOIST OUT OF THE PIXEL SHADER, NOT AN APPROXIMATION OF IT. Read the two
// paragraphs above again: nowhere in them does a view direction or a world position appear. This
// function's ENTIRE input is the sun direction, the atmosphere profile and the camera's altitude --
// every one of those is already fixed for the whole frame by the time any pixel shades. Every pixel
// that called this was computing the exact same answer, which used to cost two full 32-step
// atmosphere marches (each with a Chapman-function transmittance term inside every step) PER CALL,
// and this function was called up to three times for a single pixel: once from averFogInscatter
// directly, and up to twice more from inside averSkyPhysical's ground veil (once for the pixel's own
// sky ray, and averSkyPhysical itself calls this function too). Baking it once on the CPU and
// reading it back here is not a quality trade-off anywhere in the frame -- it is the exact same
// 32-step march this file already trusted, run once instead of once-per-pixel-times-up-to-three,
// matching the pattern gSkyZenith/gSkyHorizon already established for the authored dome.
float3 averFogInscatterRef() {
    return gFogInscatterRef.rgb;
}

// What the ground below the horizon radiates: albedo times the sun and sky falling on it.
float3 averGroundRadiance() {
    // The ground is part of the environment, so in a furnace it carries L like everything else.
    // Leaving it physical would make the lower hemisphere darker and every downward-facing probe
    // fail for a reason that has nothing to do with the shading model.
    if (averFurnaceOn()) return averFurnaceL();
    float3 albedo = srgbToLin(gGroundColor.rgb);
    float  ndl    = saturate(normalize(gLightDir.xyz).z);
    float3 E = averSunRadiance() * ndl + PI * averSkyAbove(float3(0, 0, 0.5)) * gAmbient.r;
    return albedo * E / PI;
}

// The whole authored dome: sky above the horizon, fading to the lit ground below it.
float3 skyColorFull(float3 dir)
{
    float3 above = averSkyAbove(dir);
    float g = smoothstep(0.0, 0.35, saturate(-dir.z)) * gGroundColor.a;
    return lerp(above, averGroundRadiance(), g);
}
// The dome, as every shading path names it.

// The sky along one view ray, marched through the physical model.
//
// THE RAY IS NO LONGER CLAMPED TO THE UPPER HEMISPHERE. It used to be -- cosV = max(dir.z, 0) -- so
// EVERY direction below the horizon was marched as though it pointed exactly AT the horizon, which
// is the longest and haziest path this model can produce, and the whole lower hemisphere got that
// one value as a flat slab. On top of that the result was crossfaded to averGroundRadiance() across
// smoothstep(0, 0.35): a fixed ~20-degree band, painted at that width whatever the camera was doing,
// and to a ground disc composited as though no air stood in front of it at all. Together those are
// the bright band across the horizon and the warm brown below it -- two separate wrongs filling half
// the view.
//
// Marched as authored now. A ray that meets the planet STOPS at the planet, so the air in front of
// the ground is the air actually there: metres of it looking steeply down, tens of kilometres just
// under the horizon. The ground is then composited the way every other surface in this file is --
// background * transmittance + inscatter, exactly what averApplyFog does -- instead of replacing the
// sky outright. Distant ground veils to the horizon's own colour, near ground does not, so the
// effect follows the view instead of being stamped on at a constant angle. It also means the haze
// only takes over the horizon once there is real distance to look through, which is the scale
// argument: at room and arena size there is nothing between you and the ground to scatter, and the
// band that used to sit there was never earned.
//
// ABOVE THE HORIZON THIS IS BIT-IDENTICAL TO WHAT IT REPLACED. For dir.z >= 0 the planet's near root
// is behind the camera, so tMax is still the far root of the atmosphere shell and cosV still equals
// dir.z; the old ground crossfade evaluated to zero there too. Only dir.z < 0 changes, which is why
// the fog's zenith sample and every probe that shades a surface are untouched.
float3 averSkyPhysical(float3 dir) {
    // FURNACE FIRST, exactly as skyColor does it. A furnace is a UNIFORM environment of radiance L,
    // and this function is the sky the backends draw whenever the physical atmosphere is on -- so
    // without this guard, turning the atmosphere on left the furnace shading against a real sky:
    // every surface correctly reading L, in front of a background that was not L. The shading was
    // right and the picture was a lie, which is the worse of the two failures because it makes the
    // oracle unreadable by eye and stops the background being usable as the reference the plates are
    // compared against.
    if (averFurnaceOn()) return averFurnaceL();
    float3 L   = normalize(gLightDir.xyz);
    float  alt = averAtmoCamAlt();
    float  r0  = gAtmoPlanet.x + alt;
    float  b   = r0 * dir.z;

    // Where the ray stops: the planet if it meets it, otherwise the top of the air.
    //
    // THE RADIUS TERMS ARE FACTORED, NEVER SQUARED AND SUBTRACTED. r0 and the planet radius are both
    // about 6360 km, so r0*r0 is ~4.05e7 and one float32 ulp up there is about 4 km^2 -- BIGGER than
    // the quantity being asked for, which is only 25 km^2 for a camera two metres up and 2.5 km^2 at
    // twenty centimetres. Computed as (r0*r0 - R*R) the result is mostly rounding error, and that
    // error, amplified by the near-root subtraction below, is what dithered the entire lower
    // hemisphere into speckle and concentric arcs at eye height. (r0-R)(r0+R) cannot cancel: the
    // first factor IS the altitude, already known exactly.
    float cP    = alt * (r0 + gAtmoPlanet.x);
    float discP = b * b - cP;
    // Vieta's form for the near root: t_near = c / (-b + sqrt(disc)). The textbook -b - sqrt(disc)
    // subtracts two nearly equal positives and discards most of the mantissa precisely when the
    // ground is close, which is whenever a camera is near it.
    float tGround = (dir.z < 0.0 && discP >= 0.0) ? cP / max(-b + sqrt(discP), 1e-9) : -1.0;
    bool  hitsGround = tGround > 0.0;

    // A DOWNWARD RAY IS MARCHED ALONG THE HORIZON, NOT DOWN TO WHERE IT MEETS THE PLANET, and that
    // one substitution is what closes the seam. Marched to the planet, the integral below the horizon
    // is a genuinely SHORTER path than the one just above it -- from 200 m up, grazing down hits
    // ground about 50 km out while grazing up leaves through hundreds of kilometres of air -- so the
    // inscatter steps DOWN across dir.z = 0 by the difference between those two path lengths. That
    // step is the bright rim: not a band drawn at the horizon, a discontinuity in what is being
    // integrated on either side of it. Pinning cosV to 0 for the whole lower hemisphere makes the two
    // sides evaluate the identical integral at dir.z = 0, so they meet exactly, by construction and
    // at every sun elevation, with nothing to tune.
    //
    // THIS IS NOT THE OLD cosV = max(dir.z, 0) CLAMP COMING BACK. That clamp is above this function's
    // history for good reason, but what made it a flat slab was never the clamp -- it was that the
    // clamp was ALL there was: one value for the entire lower hemisphere, crossfaded to a ground disc
    // over a fixed 20-degree band. The phase term here still gets the TRUE direction (dot(dir, L)
    // below, not the clamped one) even though cosV itself is pinned, so the SCATTERING ANGLE -- and
    // with it the sun's azimuth and the warm glow near it -- is not lost, only the march's distance
    // profile is. A flat colour below the horizon, not a flat scene: two different claims, and only
    // the old version made both of them.
    float cosV  = hitsGround ? 0.0 : dir.z;
    float bV    = r0 * cosV;
    float cA    = (r0 - gAtmoPlanet.y) * (r0 + gAtmoPlanet.y);
    float discA = bV * bV - cA;
    if (discA < 0.0) return 0.0;
    float tMax  = -bV + sqrt(discA);

    float3 T;
    float3 sky = averAtmoScatter(r0, cosV, L.z, dot(dir, L), tMax, (int)gAtmoTune.z, T);

    // RESTORES SOME OF THE BLUE A FULLER MULTIPLE-SCATTERING TREATMENT WOULD KEEP -- APPLIED HERE,
    // LOCALLY, NOT INSIDE averAtmoScatter. Real air scatters blue harder than red, but by the time
    // sunlight has travelled far enough through real air to matter for a grazing, hazy view, it has
    // ALSO already lost more of its own blue than red getting here at all (real airmass reddening --
    // even a comfortable 60-degree sun elevation is an airmass of ~1.16, enough to knock blue
    // transmittance to ~0.7 against red's ~0.95). This model's single scattering bounce cannot
    // recover that loss the way real light -- bounced many times, through many different columns of
    // air that each lost a different amount of blue on ITS way to the sun -- does, and that missing
    // recovery is what was reported as a flat cream horizon instead of a hazy blue-grey one.
    //
    // TWO ATTEMPTS TO FIX THIS INSIDE averAtmoScatter ITSELF WERE TRIED AND REVERTED (see that
    // function's own comment): both broke a DIFFERENT AtmosphereTest smoothness sweep, because that
    // function is the one thing the sky, the fog reference, aerial perspective and every ordinary lit
    // surface in an atmoOn scene all share, each with its own tight, already-verified smoothness
    // contract this session had no way to check exhaustively in one sitting. This applies the same
    // idea -- borrow the least-depleted channel's survival back for the isotropic bounce light this
    // model can't otherwise represent -- but only to what this one function draws, so it can only
    // ever affect the sky, never the things that share the integral above with it.
    //
    // groundSunT.r - groundSunT is how far each channel trails red's own survival at this altitude
    // and sun angle -- zero near noon, largest toward sunrise/sunset.
    //
    // haze IS SQUARED, NOT USED LINEARLY. saturate(1-luminance(T)) reaches its 1.0 ceiling well
    // before the view is fully horizontal, so a linear haze applied the correction at near-FULL
    // strength across a wide band approaching the horizon -- a flat wash with a hard edge where it
    // capped out, reported back as "too bright, make it consistent with the sky" once that wash
    // read as visibly brighter than the moodier, cloud-shadowed sky sitting right above it. Squaring
    // it keeps the correction small until the view is GENUINELY grazing and lets it taper in over a
    // wider range instead of snapping to full strength, so the horizon band gains depth -- a
    // gradient toward blue, not a flat coat of it -- and the visible seam a hard ceiling draws is
    // gone along with the wash.
    float3 groundSunT = averAtmoSunTransmittance(max(alt, 0.0), L.z);
    float3 shortfall  = max(groundSunT.r - groundSunT, 0.0);
    float  haze       = saturate(1.0 - averLuminance(T));
    sky += averLuminance(sky) * shortfall * (haze * haze) * gAtmoTune.y * 1.5;

    // BELOW THE HORIZON YOU SEE THE SKY, VEILED BY WHATEVER AIR IS IN THE WAY -- not a lit ground
    // disc, and NOT averGroundRadiance either. That function stayed brown through every fog fix
    // before this one for the same reason it would stay brown here: it is an authored-sky construct,
    // the raw warm sun at full strength with no air in front of it, and a neutral 0.24 albedo under a
    // 1.0/0.98/0.92 sun at 100k lux lands on warm cream no matter what haze is asked to blend toward
    // it. An early version of this fix DID blend toward it, to avoid the lower hemisphere reading as
    // one flat colour -- and that brought the cream straight back the moment the blend had any real
    // weight, the exact bug this comment already warned about one paragraph up. It is also simply the
    // wrong thing to draw at this scale: the ground you actually see in a game is the level's own
    // geometry, and the dome's ground only means anything zoomed out far enough for the planet to be
    // a planet.
    //
    // So there is no ground term here at all. `sky` already IS the right answer, on its own: cosV is
    // pinned to 0 for the entire lower hemisphere (the fix above), so every downward direction
    // integrates the identical horizon-grazing path and returns the identical, correctly-coloured
    // haze -- flat, yes, but flat and RIGHT is a better answer than graded and brown, and it is the
    // same haze colour the sky immediately above the horizon is already drawing, so there is no seam
    // to paper over with a second blend.
    return sky;
}

// skyColor lives BELOW averSkyPhysical, not beside skyColorFull where it reads more naturally,
// because it now calls it and HLSL has no forward declarations -- a function must be defined
// before the line that uses it or the runtime DXC compile fails, on a build that reported
// success. Moved rather than duplicated.
float3 skyColor(float3 dir){
    if (averFurnaceOn()) return averFurnaceL();
    // THE SKY A MIRROR SEES IS THE SKY YOU SEE, and until now it provably was not.
    //
    // SkyModel::Physical is the DEFAULT (RHI.hpp), so the dome on screen is a marched atmosphere:
    // azimuth-aware, brighter toward the sun, Rayleigh-graded. This function -- the one EVERY
    // reflection in the engine falls back to, for glass, water, metal and the ray-traced reflection
    // miss -- returned averSkyAbove instead: lerp(horizon, zenith, f(dir.z)). Elevation only. No
    // azimuth term at all.
    //
    // So a near-mirror reflected a smooth vertical gradient while the sky behind it had structure,
    // and the two did not match. That is the whole reason authored glass in this engine has read as
    // flat tinted plastic no matter how correct its Fresnel was: the reflection was real, weighted
    // properly and undimmed by alpha -- it was a reflection of NOTHING. Diagnosed from a PTTest
    // capture, not from the code: the panes were uniform sheets under a visibly graded sky.
    //
    // GATED ON averAtmoOn(), the same switch averFogInscatter already branches on a few lines below,
    // so an authored sky keeps the authored dome in its reflections and nothing changes for it.
    //
    // THE COST IS A MARCH, AND IT IS NOT FREE. This file records averSkyPhysical measuring 85% of
    // the scene pass and 41% of the frame before the fog path learned to defer it behind a w > 0.01
    // threshold. There is no equivalent threshold available here: a reflection either needs the sky
    // or does not, and the caller already decided that. Measure the frame before and after rather
    // than assuming this is small -- and if it is not, the answer is a cheaper directional
    // approximation of the same atmosphere, not a return to a gradient that does not match the sky.
    if (averAtmoOn()) return averSkyPhysical(dir);
    return skyColorFull(dir);
}

// ---- fog ---------------------------------------------------------------------------------------

// Exponential height fog opacity at a world point, solved analytically along the view ray.
float averFogFactor(float3 wpos) {
    float3 a = gCamPos.xyz;
    float3 v = wpos - a;
    float len = length(v);
    float start = gFogParams.z;
    if (len <= start) return 0.0;

    float k  = gFogParams.x;
    float d0 = gFogColor.a;
    float tau;
    if (k <= 1e-8) {
        tau = d0 * (len - start);
    } else {
        float3 dir = v / max(len, 1e-6);
        a += dir * start;
        float seg = len - start;
        float dz = dir.z * seg;
        float kdz = k * dz;
        float f = abs(kdz) > 1e-4 ? (1.0 - exp(-kdz)) / kdz : 1.0;
        tau = d0 * exp(-(a.z - gFogParams.y) * k) * seg * f;
    }
    return saturate(1.0 - exp(-tau)) * gFogParams.w;
}

// The fog's in-scatter target: the sky above the clouds NEARBY, blending toward the sky ALONG THE
// RAY once there is enough real atmosphere between the camera and wpos for that to be the honest
// answer.
//
// UNDER A PHYSICAL SKY this blends averFogInscatterRef() (zenith -- the same atmosphere the dome is
// drawn with, sampled straight up) toward averSkyPhysical(dir) (the same atmosphere, marched along
// the TRUE view ray, ground-hit handling and all) by `t`, the aerial-perspective transmittance
// averApplyFog already computed getting here. Passed in rather than recomputed: it is the SAME
// march, so asking for it twice would cost twice for the same answer.
//
// WHY T IS THE RIGHT BLEND WEIGHT, NOT A DISTANCE OR A THRESHOLD. t is the fraction of light that
// survives the real Rayleigh/Mie/ozone extinction between camera and wpos -- it is already exactly
// "how much atmosphere is actually in the way," continuous and smooth by construction (it is
// exp(-opticalDepth)), so the blend has no seam and no scale to hand-tune: near the camera t is ~1
// and this returns the flat zenith reference nearly unchanged, the same answer the old
// always-zenith version gave and for the same reason -- there is no air to speak of between here and
// there, so there is nothing for the sky to disagree with itself about. Only once a real stretch of
// atmosphere separates camera and point does t fall and the true ray's own colour -- sun-relative
// glow, horizon veil, all of it -- fade in. That is "slowly visible at large scale" for free, off a
// quantity already being computed, not a new one invented to produce it.
//
// AND IT IS ALREADY SHAPED BY THE PLANET'S ACTUAL RADIUS, not a separate curvature term bolted on.
// t comes from averAtmoScatter, whose density profile along the ray is a function of the altitude
// r = sqrt(r0^2 + 2 r0 cosV t + t^2) -- literally the geometry of a straight ray over a sphere of
// radius r0 = gAtmoPlanet.x + camera altitude. Shrink the simulated planet and that altitude climbs
// faster per unit of ground distance, extinction accumulates sooner, t falls off sooner, and the true
// sky's colour shows up at a shorter range -- a small moon's horizon curves away underfoot and reveals
// the grazing sky quickly; an Earth-scale planet's is gentle and distant. That correspondence was
// already sitting in this file; this is the first place fog asked for it.
//
// UNDER AN AUTHORED sky there is no physical atmosphere to derive from, so the level's own FOG
// color tints skyColorFull's authored gradient along the true view ray -- and under a PHYSICAL sky
// it does the exact same job, which it did not used to. RHI.hpp's own comment on fogColor calls it
// "a TINT on the in-scattered sky, not a replacement," and that was true for the authored branch and
// silently false for this one: the physical branch returned the raw Rayleigh/Mie answer with no way
// for a level to put its own colour on it at all, which is the gap behind "make the fog properly
// blue, like UE's" -- FogInscatteringColor there is exactly this multiply, applied unconditionally,
// and it was the one knob a level author had that this function was not honouring. A level that
// never sets FOG keeps gFogColor at its default white, so this multiply is a no-op for every scene
// that does not ask for it; SkyForge's own `FOG ... color 0.7 0.78 0.88` now finally reaches the
// physical fog it was always written to tint.
float3 averFogInscatter(float3 wpos, float3 t) {
    float3 dir = normalize(wpos - gCamPos.xyz);
    if (averAtmoOn()) {
        float  w   = saturate(1.0 - averLuminance(t));
        // BRANCHED FOR EXACTLY THE REASON averApplyFog BRANCHES AROUND THIS WHOLE FUNCTION -- the same
        // mistake, one level further down, and the far more expensive instance of it. HLSL evaluates
        // both sides of a lerp, so every fogged pixel in the frame was paying for a full
        // averSkyPhysical view-ray march (gAtmoTune.z steps, a Chapman-function transmittance term
        // inside every one) and then multiplying the answer by w.
        //
        // AND w IS ZERO WHEREVER FOG IS ACTUALLY VISIBLE. That is not a guess: it is the argument the
        // comment above averFogInscatterRef already makes at length -- w is driven by aerial
        // transmittance, which is governed by real Rayleigh/Mie coefficients and needs tens of
        // kilometres to move, while authored fog is tuned to read within a few hundred metres to a
        // couple of kilometres. So the march was computed and then discarded, per pixel, per frame.
        //
        // MEASURED, ElectricDreams at 2750x1639, 4x MSAA, rt and gi off: scene draw 8.9ms -> 1.3ms.
        // It was 85% of the scene pass and 41% of the entire frame. Nothing else in the pass came
        // close -- the cascade PCF, the environment term and the BRDF each measured at 0.0ms beside it.
        //
        // The march is NOT gone, it is deferred to where it earns its cost: once real air stands
        // between camera and surface, w lifts off zero and the sky along the ray genuinely stops
        // matching the reference. The threshold is small enough that the value it skips differs from
        // the value it returns by at most 0.2% of the gap between them, which is well under a bit at
        // 8-bit output and cannot produce a visible seam where a pixel crosses it.
        float3 physical = averFogInscatterRef();
        // 0.01 IS MEASURED, NOT PICKED. Sweeping the threshold across this scene, every value from
        // 0.01 to 0.9 gives the same 1.9ms and 0.002 gives 8.9ms: w for effectively every pixel here
        // lands between those two, which is the comment above averFogInscatterRef being right about
        // the magnitude. At 0.01 the term skipped is at most one hundredth of the gap between the
        // reference and the marched sky -- a fraction of one 8-bit step, so a pixel crossing the
        // threshold cannot band.
        if (w > 0.01) physical = lerp(physical, averSkyPhysical(dir), w);
        return physical * srgbToLin(gFogColor.rgb);
    }
    return skyColorFull(dir) * srgbToLin(gFogColor.rgb);
}

// Applies the air between the camera and a surface: physical atmosphere, then height fog.
float3 averApplyFog(float3 color, float3 wpos) {
    float3 T = 1.0;
    if (averAtmoOn()) {
        float3 inscatter = averAtmoAerial(wpos, T);
        color = color * T + inscatter;
    }

    // BRANCHED, NOT LERPED UNCONDITIONALLY. HLSL evaluates both sides of a lerp eagerly, so every
    // pixel nearer than the fog's own start distance -- averFogFactor's early `len <= start` return
    // at RHIShaders.cpp -- used to pay for averFogInscatter anyway (its dir normalize, and under a
    // physical sky a full averSkyPhysical march for the large-scale blend) for a contribution
    // multiplied by exactly zero. averFogFactor is smooth and spatially coherent -- it is a
    // continuous exponential of distance and height, not a hard cutoff -- so divergence within a
    // GPU wave should stay confined to the fog's own boundary, not scatter across the image.
    float fogF = averFogFactor(wpos);
    if (fogF > 0.001) color = lerp(color, averFogInscatter(wpos, T), fogF);
    return color;
}


// The sky as light: the cosine-weighted average radiance over the hemisphere about N.
// Transforms a NORMAL by a world matrix. Not the same operation as transforming a direction.
//
// A normal must go through the inverse transpose of the upper 3x3, not the matrix itself. Under
// rotation and UNIFORM scale the two agree once you normalise, which is why `mul(float4(n,0), w)`
// looked correct everywhere for so long -- and it stays correct for an axis-aligned box under any
// scale, because a box's normals lie along the principal axes and a diagonal scale only lengthens
// them. It is wrong for every normal with components on more than one axis: a sphere, a cone, a
// tree canopy, anything rotated, once the entity's scale is non-uniform. Steeper scales tilt the
// shading further, and it reads as a lighting bug rather than a transform one.
//
// This uses the COFACTOR matrix rather than a real inverse. cofactor(M) == det(M) * inverse(M)^T,
// and the caller normalises, so the det factor divides straight back out -- no determinant, no
// division, no singular case to guard. A mirrored transform has det < 0 and flips the normal, which
// is what a mirrored mesh should do.
//
// Row-vector convention throughout: a tangent goes t' = t * M, so a normal goes n' = n * (M^-1)^T,
// and (M^-1)^T is cofactor(M) up to that scale.
float3 averTransformNormal(float3 n, float4x4 w) {
    float3 c0 = float3(w[0][0], w[0][1], w[0][2]);
    float3 c1 = float3(w[1][0], w[1][1], w[1][2]);
    float3 c2 = float3(w[2][0], w[2][1], w[2][2]);
    // Rows of the cofactor matrix are the cross products of the other two rows.
    float3x3 cof = float3x3(cross(c1, c2), cross(c2, c0), cross(c0, c1));
    return mul(n, cof);
}

// The sky as irradiance, from the nine coefficients, DIVIDED BY PI so the result is a mean
// radiance rather than an irradiance -- which is the unit every caller of averSkyIrradiance
// already expects (see the E = sunIrradiance*ndl + PI*skyRadiance*ambient note above: the sky
// term is a radiance and the PI cancels). The three constants are the cosine-convolution
// factors PI, 2PI/3 and PI/4 with the basis constants folded in and the PI divided back out.
//
// SELF-CHECK, and it is worth keeping in mind when this looks wrong: a UNIFORM sky of radiance L
// projects to c[0] = L * 0.282095 * 4PI and nothing else, and this returns exactly L. That is
// what keeps the white furnace reading 1.000 through this path.
float3 averShIrradiance(float3 n) {
    float3 e = gSkySh[0].rgb * 0.282095;
    e += (gSkySh[1].rgb * n.y + gSkySh[2].rgb * n.z + gSkySh[3].rgb * n.x) * 0.325735;
    e += (gSkySh[4].rgb * (n.x * n.y) + gSkySh[5].rgb * (n.y * n.z) +
          gSkySh[7].rgb * (n.x * n.z)) * 0.273137;
    e += gSkySh[6].rgb * ((3.0 * n.z * n.z - 1.0) * 0.078848);
    e += gSkySh[8].rgb * ((n.x * n.x - n.y * n.y) * 0.136569);
    // L2 can undershoot on a sky with a strong, small bright region; ambient light is never
    // negative, and a negative here would subtract light from whatever it is added to.
    return max(e, 0.0);
}

float3 averSkyIrradiance(float3 N) {
    if (averFurnaceOn()) return averFurnaceL();
    // THE AZIMUTH IS THE POINT. Everything below this line reads only N.z, because the authored
    // dome it samples has no azimuthal term to read -- so a wall facing the rising sun and one
    // facing away were handed identical ambient light. Under the physical atmosphere the sky
    // genuinely does vary with azimuth, and nine coefficients carry that for about twenty ALU.
    //
    // The authored dome keeps the old path deliberately: it IS azimuthally symmetric, so there is
    // nothing for the coefficients to add, and projecting it would move every probe in every
    // non-physical-sky project to say the same thing more expensively.
    if (averAtmoOn()) return averShIrradiance(N);
    float meanZ = N.z * 0.5;
    float3 dome = skyColorFull(float3(0.0, 0.0, meanZ));
    float belowFraction = saturate(0.5 - N.z * 0.5) * gGroundColor.a;
    return lerp(dome, averGroundRadiance(), belowFraction * 0.5);
}

// ---- PBR mesh with sky ambient + distance fog ----
// Scene vertex as the input assembler delivers it.
struct VSIn  { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; };
// Interpolants the scene pixel shaders read.
struct VSOut { float4 pos : SV_POSITION; float3 nrmWS : NORMAL; float3 wpos : TEXCOORD0; float2 uv : TEXCOORD1; };

// Transforms one scene vertex to clip space, keeping world position and normal.
VSOut VSMain(VSIn i) {
    VSOut o;
    float4 wp = mul(float4(i.pos, 1.0), gWorld);
    o.wpos = wp.xyz;
    o.pos = mul(wp, gViewProj);
    o.nrmWS = averTransformNormal(i.nrm, gWorld);
    o.uv = i.uv;
    return o;
}

// ---- the image you get with NO material module: FROZEN, do not evolve ----
// Schlick Fresnel.
float3 plainFresnelSchlick(float ct, float3 F0){ return F0 + (1.0-F0)*pow(saturate(1.0-ct),5.0); }
// GGX normal distribution.
float plainDistGGX(float ndh, float a){ float a2=a*a; float d=ndh*ndh*(a2-1.0)+1.0; return a2/(PI*d*d+1e-6); }
// Schlick geometry term for one direction.
float plainGeomSchlick(float nd, float k){ return nd/(nd*(1.0-k)+k); }

// Shades one surface with Cook-Torrance GGX plus sky ambient and fog. Returns linear radiance.
float4 plainShadeSurface(VSOut i, float sunVis, float3 indirectRadiance, float ao) {
    float3 N = normalize(i.nrmWS);
    float3 V = normalize(gCamPos.xyz - i.wpos);
    // TWO-SIDED SHADING. A leaf, a frond or a grass blade is one sheet of triangles drawn with
    // culling off, so half of what you see is the BACK of a surface whose normal points away from
    // you. Shading that with the unflipped normal puts N.L below zero, the saturate clamps it to
    // black, and every plant in the world renders as a dark silhouette against lit ground -- which
    // is exactly what this engine did until now. Nothing anywhere handled it: SV_IsFrontFace does
    // not appear in a single shader in this tree.
    //
    // dot(N, V) RATHER THAN SV_IsFrontFace, deliberately. The face flag is the textbook answer but
    // needs a new PS input threaded through every entry point in this file and the mesh-shader
    // variants beside it; flipping on the view vector needs one line and answers the same question
    // for the case that matters -- "am I looking at the back of this sheet". On a correctly wound
    // closed mesh the back faces are culled before they reach here, so this cannot fire there.
    if (dot(N, V) < 0.0) N = -N;
    float3 L = normalize(gLightDir.xyz);
    float3 H = normalize(V + L);
    float metallic = saturate(gMaterial.x);
    float rough = clamp(gMaterial.y, 0.045, 1.0);

    if (gMaterial.z > 0.5) {
        return float4(gBaseColor.rgb, gBaseColor.a);
    }

    float3 albedo = srgbToLin(gBaseColor.rgb);
    float3 lightC = srgbToLin(gLightColor.rgb) * 3.0;
    float ndv = saturate(dot(N, V));
    float ndl = saturate(dot(N, L));
    float3 F0 = lerp(0.04.xxx, albedo, metallic);

    float a = rough * rough;
    float k = (rough + 1.0); k = k * k / 8.0;
    float D = plainDistGGX(saturate(dot(N, H)), a);
    float G = plainGeomSchlick(ndv, k) * plainGeomSchlick(ndl, k);
    float3 F = plainFresnelSchlick(saturate(dot(H, V)), F0);
    float3 spec = (D * G * F) / (4.0 * ndv * ndl + 1e-4);
    float3 kd = (1.0 - F) * (1.0 - metallic);
    float3 direct = (kd * albedo / PI + spec) * lightC * ndl * sunVis;

    float3 ambient = kd * albedo * skyColor(N) * gAmbient.r;
    float3 R = reflect(-V, N);
    float3 envSpec = skyColor(R) * plainFresnelSchlick(ndv, F0) * (1.0 - rough);

    float3 indirect = kd * albedo * indirectRadiance;
    ambient *= ao;
    float3 color = direct + ambient + indirect + envSpec * 0.35;

    color = averApplyFog(color, i.wpos);

    return float4(color, gBaseColor.a);
}


// ---- procedural sky (fullscreen triangle via SV_VertexID) ----
// Interpolants the sky pixel shaders read.
struct SkyOut { float4 pos : SV_POSITION; float2 ndc : TEXCOORD0; };
// Emits one vertex of the fullscreen sky triangle from SV_VertexID.
SkyOut VSky(uint id : SV_VertexID) {
    SkyOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.ndc = uv * 2.0 - 1.0;
    o.pos = float4(o.ndc, 1.0, 1.0);
    return o;
}

#if AVER_MS
// ================= Mesh shader geometry path =================
// One group per 64 triangles, read from root SRVs and expanded unindexed. The two SRV registers
// arrive as -D macros from rhi::meshGeometryDefines.
#if !defined(AVER_MS_VTX_REG) || !defined(AVER_MS_IDX_REG)
#error "AVER_MS needs AVER_MS_VTX_REG / AVER_MS_IDX_REG from rhi::meshGeometryDefines"
#endif
// Two-step so the argument is expanded before it is pasted; one step pastes the macro NAME.
#define AVER_REG_JOIN2(a, b) a##b
#define AVER_REG_JOIN(a, b) AVER_REG_JOIN2(a, b)

// Vertex stride on the mesh-shader path: must match rhi::MeshVertex byte for byte.
struct MeshVtx { float3 pos; float3 nrm; float2 uv; };
StructuredBuffer<MeshVtx> gVerts   : register(AVER_REG_JOIN(t, AVER_MS_VTX_REG));
ByteAddressBuffer         gIndices : register(AVER_REG_JOIN(t, AVER_MS_IDX_REG));
cbuffer MeshCB : register(b5) { uint gTriCount; uint3 _msPad; };

#define AVER_MS_TRIS 64

// Triangles this group owns.
uint msTriCount(uint gid) { return min(AVER_MS_TRIS, gTriCount - gid * AVER_MS_TRIS); }

// Pulls this group's triangles from the vertex and index buffers and emits them transformed.
[numthreads(AVER_MS_TRIS, 1, 1)]
[outputtopology("triangle")]
void MSMain(uint gid : SV_GroupID, uint gtid : SV_GroupThreadID,
            out vertices VSOut verts[AVER_MS_TRIS * 3],
            out indices uint3 tris[AVER_MS_TRIS]) {
    uint count = msTriCount(gid);
    SetMeshOutputCounts(count * 3, count);   // must run for the whole group, before any output
    if (gtid >= count) return;

    uint3 idx = gIndices.Load3((gid * AVER_MS_TRIS + gtid) * 12);
    uint o = gtid * 3;
    [unroll] for (uint k = 0; k < 3; ++k) {
        MeshVtx v = gVerts[idx[k]];
        float4 wp = mul(float4(v.pos, 1.0), gWorld);
        VSOut ov;
        ov.wpos  = wp.xyz;
        ov.pos   = mul(wp, gViewProj);
        ov.nrmWS = averTransformNormal(v.nrm, gWorld);
        ov.uv    = v.uv;
        verts[o + k] = ov;
    }
    tris[gtid] = uint3(o, o + 1, o + 2);
}
#endif // AVER_MS

#if AVER_MS_CLUSTER
// ================= Per-cluster GPU LOD: amplification + mesh shader =================
// One amplification-shader thread per cluster (kClusterAmplificationGroupSize per group), running
// EXACTLY the CPU reference's local cut test unchanged -- aver::trifactor::inLocalCut/coneCull/
// Frustum::intersectsSphere (modules/trifactor/src/ClusterAdapt.cpp, ClusterSelect.cpp): same
// comparison directions, same distance/near-sentinel handling, same cone sign convention -- then
// DispatchMesh's the survivors, one mesh-shader group per surviving cluster. Reuses AVER_MS_VTX_REG/
// AVER_MS_IDX_REG from rhi::meshGeometryDefines (same macro names AVER_MS's own block uses; the two
// paths are never compiled into the same shader, so there is no collision) rather than inventing a
// second -D convention for the same "vertices/indices sit past this layout's own SRV table" idea.
#if !defined(AVER_MS_VTX_REG) || !defined(AVER_MS_IDX_REG)
#error "AVER_MS_CLUSTER needs AVER_MS_VTX_REG / AVER_MS_IDX_REG from rhi::meshGeometryDefines"
#endif
#define AVER_MSC_JOIN2(a, b) a##b
#define AVER_MSC_JOIN(a, b) AVER_MSC_JOIN2(a, b)

// Byte-for-byte aver::trifactor::MeshClusterView (ClusterAdapt.hpp) -- built and uploaded by
// aver::trifactor::buildMeshClusterGpuData, MESH-LOCAL, never repacked on the way to the GPU. Field
// order and types must not move without moving both sides together.
struct ClusterBounds {
    float3 sphereCenter; float sphereRadius;
    float3 coneApex; float3 coneAxis; float coneCutoff;
    float ownErrorCm; float parentErrorCm;
    uint triangleCount; uint level;
};
// aver::trifactor::GpuMeshletDesc (ClusterAdapt.hpp): where one cluster's geometry lives in
// gClusterVerts/gClusterTris below.
struct ClusterMeshletDesc { uint vertexOffset; uint triangleOffset; uint vertexCount; uint triangleCount; };

StructuredBuffer<ClusterBounds>      gClusterBounds : register(t0);
StructuredBuffer<ClusterMeshletDesc> gClusterDesc   : register(t1);
StructuredBuffer<uint>               gClusterVerts  : register(t2);   // global vertex index per slot
StructuredBuffer<uint>               gClusterTris   : register(t3);   // packed local tri: a|(b<<8)|(c<<16)

// Vertex stride on this path: must match rhi::MeshVertex byte for byte, same contract AVER_MS's own
// MeshVtx has, for the same reason -- this is a SEPARATE declaration (not a shared one) because the
// two mesh-shader paths are never compiled together and each keeps its own name.
struct MeshVtxC { float3 pos; float3 nrm; float2 uv; };
StructuredBuffer<MeshVtxC> gVertsC : register(AVER_MSC_JOIN(t, AVER_MS_VTX_REG));
// AVER_MS_IDX_REG is reserved by the backend's mesh-geometry convention (declaredSrvCount + 1) and
// dispatchMeshClusters binds a real address there (this mesh's own index buffer) so the root
// signature's SRV root parameter is never left unset -- but nothing in THIS shader ever declares a
// resource at that register: the cluster path has no use for a whole-mesh flat index buffer, only
// gClusterVerts/gClusterTris above. A root signature may over-provision what a shader actually reads;
// it may not under-provide what a shader reads, and this shader reads nothing there at all.

// How many clusters this dispatch covers -- dispatchMeshClusters' clusterCount argument.
cbuffer ClusterCountCB : register(b5) { uint gClusterCount; uint3 _mscPad; };

// Per-instance data the amplification shader needs beyond gWorld/gViewProj/gCamPos (already reachable
// from PerObject/PerFrame, b1/b0, since every declared root parameter is ALL-stage visible): the
// pixel budget (already clamped above zero by the CPU caller, kMinClusterBudgetPx), the projection
// scale (aver::trifactor::projScale), a uniform-scale factor for the cluster bounds (the same
// approximation SandboxApp's CPU path already uses transforming a cluster sphere by an instance's
// world matrix), and the 6 world-space frustum planes -- PRECOMPUTED ON THE CPU by the exact same
// aver::trifactor::Frustum::fromViewProj this design is told to reuse unchanged. Uploading the planes
// rather than re-deriving them per GPU thread means the GPU test runs against the identical six
// numbers the CPU reference tested against, not a second formula that merely agrees with it.
cbuffer ClusterFrameCB : register(b4) {
    float  gBudgetPx;
    float  gProjScale;
    float  gWorldScale;
    uint   _cfcPad;
    float4 gFrustumPlane[6];
};

#define AVER_MSC_GROUP 32
#define AVER_MSC_MAX_VERTS 64
#define AVER_MSC_MAX_TRIS 124

// One AS group's survivors, handed to the mesh-shader groups it spawns.
struct ClusterPayload { uint clusterId[AVER_MSC_GROUP]; };
groupshared ClusterPayload gsPayload;
groupshared uint gsSurvivorCount;

// THE LOD cut, PORTED UNCHANGED from aver::trifactor::inLocalCut (ClusterAdapt.cpp): same nearest-
// point-of-sphere distance (floored at 0), same 1e-3 near-camera sentinel (1e30, finite, matching the
// CPU's own comment on why not IEEE inf), same STRICT-LESS on own error, GREATER-OR-EQUAL on parent
// error. `budgetPx` arrives already clamped above zero by the CPU caller -- this does not re-clamp,
// staying a literal mirror of inLocalCut's own body, which also trusts its caller's clamp.
bool clusterInCut(ClusterBounds c, float3 centerWS, float radiusWS, float3 eye, float budgetPx, float projScale) {
    float d = max(length(centerWS - eye) - radiusWS, 0.0);
    float ownPx = (d <= 1e-3) ? 1e30 : c.ownErrorCm * projScale / d;
    if (ownPx >= budgetPx) return false;
    float parentPx = (d <= 1e-3) ? 1e30 : c.parentErrorCm * projScale / d;
    return parentPx >= budgetPx;
}

// Cone backface test, PORTED UNCHANGED from aver::trifactor::coneCull (ClusterSelect.cpp): cull iff
// dot(normalize(apex - eye), axis) >= cutoff -- the ">= cull" direction, not "<=", per that file's own
// measured-not-assumed sign convention. `axisWS` here already folds in the CPU path's own
// getSafeNormal() (zero-length axis -> never cull), matching the transform this shader itself does
// just before calling this, one line below.
//
// cutoff <= -1.0 is the OTHER "never cull" sentinel this port honours, byte-for-byte with coneCull's
// own check -- see that function's comment for the full account (already-cooked assets on disk still
// carry ClusterBuilder.cpp's pre-fix -127/-1.0f encoding; this repairs them at read time with no
// re-cook, without touching the formula for any real, non-degenerate cutoff value).
bool clusterConeCull(float3 axisWS, float3 apexWS, float3 eye, float cutoff) {
    if (dot(axisWS, axisWS) < 0.25) return false;   // degenerate axis: never cull
    if (cutoff <= -1.0) return false;                // -127 sentinel (old cook encoding): never cull
    float3 toApex = apexWS - eye;
    float lenSq = dot(toApex, toApex);
    if (lenSq < 1e-8) return false;                  // eye at/near apex: never cull
    float3 dir = toApex * rsqrt(lenSq);
    return dot(dir, axisWS) >= cutoff;
}

[numthreads(AVER_MSC_GROUP, 1, 1)]
void ASMain(uint gtid : SV_GroupThreadID, uint dtid : SV_DispatchThreadID) {
    if (gtid == 0) gsSurvivorCount = 0;
    GroupMemoryBarrierWithGroupSync();

    bool survive = false;
    if (dtid < gClusterCount) {
        ClusterBounds c = gClusterBounds[dtid];
        float3 centerWS = mul(float4(c.sphereCenter, 1.0), gWorld).xyz;
        float radiusWS = c.sphereRadius * gWorldScale;

        // Frustum: conservative -- a sphere straddling a plane counts as inside, matching
        // aver::trifactor::Frustum::intersectsSphere exactly (same planes, same "false only if
        // definitely outside" logic).
        bool inFrustum = true;
        [unroll] for (uint p = 0; p < 6; ++p) {
            if (dot(gFrustumPlane[p].xyz, centerWS) + gFrustumPlane[p].w < -radiusWS) { inFrustum = false; break; }
        }

        if (inFrustum) {
            float3 apexWS = mul(float4(c.coneApex, 1.0), gWorld).xyz;
            // Plain direction transform (not the cofactor normal transform averTransformNormal does):
            // coneAxis is a direction along the surface, not a surface normal, matching how the CPU
            // path's xformVec transforms it.
            float3 axisRaw = mul(c.coneAxis, (float3x3)gWorld);
            float axisLenSq = dot(axisRaw, axisRaw);
            float3 axisWS = axisLenSq > 1e-8 ? axisRaw * rsqrt(axisLenSq) : float3(0, 0, 0);
            if (!clusterConeCull(axisWS, apexWS, gCamPos.xyz, c.coneCutoff))
                survive = clusterInCut(c, centerWS, radiusWS, gCamPos.xyz, gBudgetPx, gProjScale);
        }
    }

    uint slot = 0;
    if (survive) {
        InterlockedAdd(gsSurvivorCount, 1, slot);
        if (slot < AVER_MSC_GROUP) gsPayload.clusterId[slot] = dtid;
    }
    GroupMemoryBarrierWithGroupSync();

    // Every thread calls DispatchMesh once, uniformly, with the same arguments -- required by the
    // mesh-shader pipeline, same discipline SetMeshOutputCounts already uses in AVER_MS's MSMain.
    uint count = min(gsSurvivorCount, (uint)AVER_MSC_GROUP);
    DispatchMesh(count, 1, 1, gsPayload);
}

// One mesh-shader group per surviving cluster: expands that cluster's own local vertex/triangle
// block, unindexed, respecting the 64-vertex/124-triangle caps the format guarantees
// (ClusterBuilder.hpp's kMaxClusterVertices/kMaxClusterTriangles) -- getting SetMeshOutputCounts
// wrong here drops triangles silently rather than erroring, so it is called with the desc's own
// counts, verbatim, before any vertex/index write.
[numthreads(AVER_MSC_MAX_VERTS, 1, 1)]
[outputtopology("triangle")]
void MSClusterMain(uint gid : SV_GroupID, uint gtid : SV_GroupThreadID,
                    in payload ClusterPayload pld,
                    out vertices VSOut verts[AVER_MSC_MAX_VERTS],
                    out indices uint3 tris[AVER_MSC_MAX_TRIS]) {
    uint clusterIdx = pld.clusterId[gid];
    ClusterMeshletDesc d = gClusterDesc[clusterIdx];
    SetMeshOutputCounts(d.vertexCount, d.triangleCount);

    if (gtid < d.vertexCount) {
        uint globalVtx = gClusterVerts[d.vertexOffset + gtid];
        MeshVtxC v = gVertsC[globalVtx];
        float4 wp = mul(float4(v.pos, 1.0), gWorld);
        VSOut ov;
        ov.wpos  = wp.xyz;
        ov.pos   = mul(wp, gViewProj);
        ov.nrmWS = averTransformNormal(v.nrm, gWorld);
        ov.uv    = v.uv;
        verts[gtid] = ov;
    }
    // Up to 124 triangles, 64 threads: a thread may emit a second triangle 64 slots ahead of its
    // first, exactly the loop AVER_MSC_MAX_VERTS's own value sizes the stride for.
    for (uint t = gtid; t < d.triangleCount; t += AVER_MSC_MAX_VERTS) {
        uint packed = gClusterTris[d.triangleOffset + t];
        tris[t] = uint3(packed & 0xFF, (packed >> 8) & 0xFF, (packed >> 16) & 0xFF);
    }
}

// Shades a cluster the SAME way the IA/whole-mesh scene path does (D3D12Device.cpp's own
// PSMainPlain calls the identical plainShadeSurface(i, 1.0, float3(0,0,0), 1.0)) -- so a cluster
// drawn via this path is not visually distinguishable from one drawn via drawMesh() by its
// shading, only by which triangles exist. PSMainPlain itself lives in D3D12Device.cpp's
// backend-internal kShaderHLSL and is not reachable from a feature module's own shader source
// (sceneShaderSource() is never exposed outside that file), so this is a separate definition
// calling the SAME shared prelude function, not a rename of the same symbol.
// PSClusterMain HAS MOVED, to sandbox/src/ClusterMaterialShader.hpp, and the move is the fix.
//
// It used to be one line here: plainShadeSurface(i, 1.0, float3(0,0,0), 1.0). That shades from
// gBaseColor/gMaterial -- per-object constants -- and never samples a material texture, so every
// textured surface on this path drew as a flat dark silhouette. It could not have done anything
// else from inside this prelude: rhi::sharedShaderPrelude() is compiled BEFORE
// pbr::materialShaderPrelude(), so averEvalMaterial does not exist yet at this point in the file.
//
// A pixel shader that wants materials therefore cannot live here, and moving it out is the whole
// change. ASMain and MSClusterMain stay: they touch no material state, and the geometry they emit
// was never the problem -- MSClusterMain already fills VSOut::uv, which is exactly what the lit
// shader needed all along.
#endif // AVER_MS_CLUSTER
)";
    return s.c_str();
}

// Builds the -D list that pins the prelude's mesh-geometry SRV registers past a layout's own.
std::string meshGeometryDefines(const PipelineLayout& layout) {
    const u32 base = declaredSrvCount(layout);
    return "AVER_MS_VTX_REG=" + std::to_string(base) +
           ";AVER_MS_IDX_REG=" + std::to_string(base + 1);
}

// The camera post chain's HLSL. Composed once and cached.
const char* postShaderSource() {
    static const std::string s = std::string(kColorHlsl) + R"(
// ================= the camera post chain =================

// Constants for every pass in the chain.
cbuffer AverPost : register(b0) {
    float4 gPostTone;    // x exposure, y bloom intensity, z bloom threshold, w bloom knee
    float4 gPostDst;     // xy destination size in texels, zw its reciprocal
    float4 gPostSrc;     // xy source size in texels,      zw its reciprocal
    float4 gPostAdapt;   // x min log2 luminance, y 1/log2 range, z adaption alpha, w pixels sampled
    float4 gPostLimit;   // x exposure min, y exposure max, z histogram low cut, w high cut
    float4 gPostMisc;    // x middle grey, y auto-exposure on, z bloom filter radius, w unused
};

Texture2D<float4>     gPostSceneTex : register(t0);
Texture2D<float4>     gPostBloomTex : register(t1);
ByteAddressBuffer     gPostExpRead  : register(t2);
SamplerState          gPostSamp     : register(s0);

// Buffers, not textures: a typed UAV load of RGBA16F is an optional D3D12 feature.
RWByteAddressBuffer   gPostHist     : register(u0);
RWByteAddressBuffer   gPostExp      : register(u1);

// ---- bloom ---------------------------------------------------------------------------------
// A half-resolution RGBA16F pyramid of graphics passes, each reading one mip and writing the next.

// Karis' firefly weight, applied only on the first downsample.
float3 averBloomKaris(float3 c) { return c / (1.0 + averLuminance(c)); }

// The exposure this frame settled on; the authored value when auto-exposure is off.
float averPostExposure() {
    return gPostMisc.y > 0.5 ? asfloat(gPostExpRead.Load(0)) : gPostTone.x;
}

// Soft-knee threshold: the quadratic ramp that decides how much of a pixel bloom takes.
float3 averBloomPrefilter(float3 c) {
    float br = max(c.r, max(c.g, c.b));
    float knee = max(gPostTone.w, 1e-4);
    float soft = clamp(br - gPostTone.z + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    float weight = max(soft, br - gPostTone.z) / max(br, 1e-4);
    return c * weight;
}

// Interpolants every post pixel shader reads.
struct AverPostVSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

// Emits one vertex of the fullscreen post triangle from SV_VertexID.
AverPostVSOut PostVS(uint id : SV_VertexID) {
    AverPostVSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// Halves the scene into the pyramid's first level, Karis-averaged and soft-knee thresholded.
float4 PSBloomPrefilter(AverPostVSOut i) : SV_TARGET {
    float2 o = gPostSrc.zw;
    float3 a = gPostSceneTex.SampleLevel(gPostSamp, i.uv + float2(-o.x, -o.y), 0).rgb;
    float3 b = gPostSceneTex.SampleLevel(gPostSamp, i.uv + float2( o.x, -o.y), 0).rgb;
    float3 c = gPostSceneTex.SampleLevel(gPostSamp, i.uv + float2(-o.x,  o.y), 0).rgb;
    float3 d = gPostSceneTex.SampleLevel(gPostSamp, i.uv + float2( o.x,  o.y), 0).rgb;
    float3 sum = averBloomKaris(a) + averBloomKaris(b) + averBloomKaris(c) + averBloomKaris(d);
    return float4(averBloomPrefilter(sum * 0.25 * averPostExposure()), 1.0);
}

// Jimenez' 13-tap downsample.
float4 PSBloomDown(AverPostVSOut i) : SV_TARGET {
    float2 uv = i.uv;
    float2 o = gPostSrc.zw;

    float3 a = gPostSceneTex.SampleLevel(gPostSamp, uv + float2(-2*o.x,  2*o.y), 0).rgb;
    float3 b = gPostSceneTex.SampleLevel(gPostSamp, uv + float2(     0,  2*o.y), 0).rgb;
    float3 c = gPostSceneTex.SampleLevel(gPostSamp, uv + float2( 2*o.x,  2*o.y), 0).rgb;
    float3 d = gPostSceneTex.SampleLevel(gPostSamp, uv + float2(-2*o.x,      0), 0).rgb;
    float3 e = gPostSceneTex.SampleLevel(gPostSamp, uv,                          0).rgb;
    float3 f = gPostSceneTex.SampleLevel(gPostSamp, uv + float2( 2*o.x,      0), 0).rgb;
    float3 g = gPostSceneTex.SampleLevel(gPostSamp, uv + float2(-2*o.x, -2*o.y), 0).rgb;
    float3 h = gPostSceneTex.SampleLevel(gPostSamp, uv + float2(     0, -2*o.y), 0).rgb;
    float3 j = gPostSceneTex.SampleLevel(gPostSamp, uv + float2( 2*o.x, -2*o.y), 0).rgb;
    float3 k = gPostSceneTex.SampleLevel(gPostSamp, uv + float2(  -o.x,    o.y), 0).rgb;
    float3 l = gPostSceneTex.SampleLevel(gPostSamp, uv + float2(   o.x,    o.y), 0).rgb;
    float3 m = gPostSceneTex.SampleLevel(gPostSamp, uv + float2(  -o.x,   -o.y), 0).rgb;
    float3 n = gPostSceneTex.SampleLevel(gPostSamp, uv + float2(   o.x,   -o.y), 0).rgb;

    float3 sum = e * 0.125;
    sum += (a + c + g + j) * 0.03125;
    sum += (b + d + f + h) * 0.0625;
    sum += (k + l + m + n) * 0.125;
    return float4(sum, 1.0);
}

// 9-tap tent upsample, added into the level above by the blender.
float4 PSBloomUp(AverPostVSOut i) : SV_TARGET {
    float2 o = gPostMisc.z * gPostDst.zw;
    float2 uv = i.uv;

    float3 sum = gPostSceneTex.SampleLevel(gPostSamp, uv, 0).rgb * 0.25;
    sum += gPostSceneTex.SampleLevel(gPostSamp, uv + float2(-o.x,  0.0), 0).rgb * 0.125;
    sum += gPostSceneTex.SampleLevel(gPostSamp, uv + float2( o.x,  0.0), 0).rgb * 0.125;
    sum += gPostSceneTex.SampleLevel(gPostSamp, uv + float2( 0.0, -o.y), 0).rgb * 0.125;
    sum += gPostSceneTex.SampleLevel(gPostSamp, uv + float2( 0.0,  o.y), 0).rgb * 0.125;
    sum += gPostSceneTex.SampleLevel(gPostSamp, uv + float2(-o.x, -o.y), 0).rgb * 0.0625;
    sum += gPostSceneTex.SampleLevel(gPostSamp, uv + float2( o.x, -o.y), 0).rgb * 0.0625;
    sum += gPostSceneTex.SampleLevel(gPostSamp, uv + float2(-o.x,  o.y), 0).rgb * 0.0625;
    sum += gPostSceneTex.SampleLevel(gPostSamp, uv + float2( o.x,  o.y), 0).rgb * 0.0625;
    return float4(sum, 1.0);
}

// ---- eye adaptation ------------------------------------------------------------------------
// A 256-bin histogram of log2 luminance, reduced to one exposure value damped over time.
groupshared uint gHistLocal[256];

// Bins the scene's log2 luminance into the shared 256-bin histogram.
[numthreads(16, 16, 1)]
void CSHistogram(uint3 tid : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    gHistLocal[gi] = 0;
    GroupMemoryBarrierWithGroupSync();

    if (tid.x < (uint)gPostDst.x && tid.y < (uint)gPostDst.y) {
        float2 uv = (tid.xy + 0.5) * gPostDst.zw;
        float lum = averLuminance(gPostSceneTex.SampleLevel(gPostSamp, uv, 0).rgb);
        uint bin = 0;
        if (lum > 1e-4) {
            float t = saturate((log2(lum) - gPostAdapt.x) * gPostAdapt.y);
            bin = (uint)(t * 254.0 + 1.0);
        }
        InterlockedAdd(gHistLocal[bin], 1);
    }
    GroupMemoryBarrierWithGroupSync();

    uint local = gHistLocal[gi];
    if (local > 0) gPostHist.InterlockedAdd(gi * 4, local);
}

// Reduces the histogram to one exposure value and damps towards it in log space.
[numthreads(1, 1, 1)]
void CSExposure() {
    float target = gPostTone.x;

    if (gPostMisc.y > 0.5) {
        uint total = 0;
        uint counts[256];
        for (uint i = 0; i < 256; ++i) { counts[i] = gPostHist.Load(i * 4); total += counts[i]; }

        float lowCut  = total * gPostLimit.z;
        float highCut = total * gPostLimit.w;
        float seen = 0.0, weighted = 0.0, weight = 0.0;
        for (uint b = 1; b < 256; ++b) {
            float c = counts[b];
            float lo = max(lowCut - seen, 0.0);
            float hi = max(seen + c - highCut, 0.0);
            float take = max(c - lo - hi, 0.0);
            weighted += take * (float)b;
            weight += take;
            seen += c;
        }

        if (weight > 0.0) {
            float avgBin = weighted / weight;
            float avgLum = exp2((avgBin - 1.0) / 254.0 / gPostAdapt.y + gPostAdapt.x);
            target = gPostMisc.x / max(avgLum, 1e-4);
        }
        target = clamp(target, gPostLimit.x, gPostLimit.y);

        for (uint z = 0; z < 256; ++z) gPostHist.Store(z * 4, 0);
    }

    float prev = asfloat(gPostExp.Load(0));
    uint  seeded = gPostExp.Load(4);
    float next = (seeded == 0 || prev <= 0.0)
               ? target
               : exp2(lerp(log2(prev), log2(max(target, 1e-4)), saturate(gPostAdapt.z)));
    gPostExp.Store(0, asuint(next));
    gPostExp.Store(4, 1);
}

// ---- composite -----------------------------------------------------------------------------
// Exposure, bloom, tonemap and gamma in one pass; the frame becomes a display image here.
float4 PSComposite(AverPostVSOut i) : SV_TARGET {
    float3 c = gPostSceneTex.SampleLevel(gPostSamp, i.uv, 0).rgb;
#ifdef AVER_POST_AUTOEXPOSURE
    c *= asfloat(gPostExpRead.Load(0));
#else
    c *= gPostTone.x;
#endif
#ifdef AVER_POST_BLOOM
    c += gPostBloomTex.SampleLevel(gPostSamp, i.uv, 0).rgb * gPostTone.y;
#endif
    return float4(toGamma(acesTonemap(c)), 1.0);
}
)";
    return s.c_str();
}

} // namespace aver::rhi
