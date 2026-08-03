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
    float4   gCloudParams; // x coverage, y density, z layer bottom, w layer top
    float4   gCloudMotion; // xy wind offset, z 1/feature size, w enabled
    // ---- the PHYSICAL atmosphere (rhi::AtmosphereProfile) ----
    float4   gAtmoRayleigh; // rgb scattering per km, w scale height km
    float4   gAtmoMie;      // x scatter, y extinction, z scale height km, w phase g
    float4   gAtmoOzone;    // rgb absorption per km, w tent half-width km
    float4   gAtmoPlanet;   // x planet radius km, y atmosphere top radius km, z world->km, w on/off
    float4   gAtmoTune;     // x ozone centre km, y multi-scatter gain, z view steps, w aerial steps
    float4   gAtmoSunE0;    // rgb sun irradiance ABOVE the air, w ground albedo
    // x > 0.5 puts the shading model in a WHITE FURNACE: every direction carries radiance y and the
    // ground carries it too. z > 0.5 is the SUN-ON variant: the environment drops to zero and the
    // sun stays lit, which is the only configuration in which the direct term is under test at all.
    // A measuring instrument, not a look -- see averFurnaceOn.
    float4   gFurnace;
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

        float3 source = ((scatR * pR + scatM * pM) +
                         (scatR + scatM) * gAtmoTune.y / (4.0 * PI)) * sunT * gAtmoSunE0.rgb;
        float3 stepT = exp(-ext * dt);
        total += transmittance * source * (1.0 - stepT) / max(ext, 1e-12);
        transmittance *= stepT;
    }
    return total;
}

// The camera's altitude in kilometres.
float averAtmoCamAlt()    { return max(gCamPos.z * gAtmoPlanet.z, 0.0); }
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
float3 skyColor(float3 dir){
    if (averFurnaceOn()) return averFurnaceL();
    return skyColorFull(dir);
}

// The sky along one view ray, marched through the physical model.
float3 averSkyPhysical(float3 dir) {
    float3 L = normalize(gLightDir.xyz);
    float cosV = max(dir.z, 0.0);
    float r0 = averAtmoCamRadius();
    float b = r0 * cosV;
    float disc = b * b - (r0 * r0 - gAtmoPlanet.y * gAtmoPlanet.y);
    if (disc < 0.0) return 0.0;

    float3 T;
    float3 sky = averAtmoScatter(r0, cosV, L.z, dot(dir, L), -b + sqrt(disc), (int)gAtmoTune.z, T);
    float g = smoothstep(0.0, 0.35, saturate(-dir.z)) * gGroundColor.a;
    return lerp(sky, averGroundRadiance(), g);
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

// The fog's in-scatter target: the sky along the view ray, tinted by the authored fog colour.
float3 averFogInscatter(float3 wpos) {
    float3 dir = normalize(wpos - gCamPos.xyz);
    return skyColorFull(dir) * srgbToLin(gFogColor.rgb);
}

// Applies the air between the camera and a surface: physical atmosphere, then height fog.
float3 averApplyFog(float3 color, float3 wpos) {
    if (averAtmoOn()) {
        float3 T;
        float3 inscatter = averAtmoAerial(wpos, T);
        color = color * T + inscatter;
    }
    return lerp(color, averFogInscatter(wpos), averFogFactor(wpos));
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

float3 averSkyIrradiance(float3 N) {
    if (averFurnaceOn()) return averFurnaceL();
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


// ================= volumetric clouds =================
// One raymarched layer, evaluated on sky pixels only, with analytic noise so it needs no SRV.

// Hashes a 3D point to a scalar in [0,1).
float averHash13(float3 p) {
    p = frac(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return frac((p.x + p.y) * p.z);
}

// Value noise with a smoothstep-interpolated lattice. Eight hashes a call.
float averValueNoise(float3 x) {
    float3 i = floor(x);
    float3 f = frac(x);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = averHash13(i + float3(0,0,0)), n100 = averHash13(i + float3(1,0,0));
    float n010 = averHash13(i + float3(0,1,0)), n110 = averHash13(i + float3(1,1,0));
    float n001 = averHash13(i + float3(0,0,1)), n101 = averHash13(i + float3(1,0,1));
    float n011 = averHash13(i + float3(0,1,1)), n111 = averHash13(i + float3(1,1,1));
    return lerp(lerp(lerp(n000, n100, f.x), lerp(n010, n110, f.x), f.y),
                lerp(lerp(n001, n101, f.x), lerp(n011, n111, f.x), f.y), f.z);
}

// Extinction per world unit for a fully dense cloud, derived from the layer's thickness.
float averCloudSigma() {
    const float kOpticalDepthAtFull = 9.0;   // a dense cumulus, edge to edge
    return gCloudParams.y * kOpticalDepthAtFull / max(gCloudParams.w - gCloudParams.z, 1.0);
}

// Cloud density at a world point. `detail` buys a second noise octave; the light march skips it.
float averCloudDensity(float3 wpos, bool detail) {
    float bottom = gCloudParams.z, top = gCloudParams.w;
    float h = saturate((wpos.z - bottom) / max(top - bottom, 1.0));
    float shape = saturate(h * 4.0) * saturate((1.0 - h) * 1.6);
    if (shape <= 0.001) return 0.0;

    float3 p = (wpos + float3(gCloudMotion.xy, 0.0)) * gCloudMotion.z;
    float n = averValueNoise(p) * 0.6;
    if (detail) n += averValueNoise(p * 3.17) * 0.3;
    else        n += 0.15;
    n += averValueNoise(p * 0.41) * 0.25;

    float cover = 1.0 - gCloudParams.x;
    float d = saturate((n - cover) / max(1.0 - cover, 1e-3));
    return d * shape;
}

// Henyey-Greenstein phase function.
float averHG(float ct, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * ct, 1e-4), 1.5));
}

// Marches the layer and returns scattered radiance in rgb, TRANSMITTANCE in a.
float4 averCloudLayer(float3 ro, float3 rd, float3 sunDir, float3 sunColour) {
    if (gCloudMotion.w < 0.5) return float4(0, 0, 0, 1);
    float bottom = gCloudParams.z, top = gCloudParams.w;

    float t0, t1;
    if (rd.z > 1e-4) {
        if (ro.z > top) return float4(0, 0, 0, 1);
        t0 = max((bottom - ro.z) / rd.z, 0.0);
        t1 = (top - ro.z) / rd.z;
    } else if (rd.z < -1e-4) {
        if (ro.z < bottom) return float4(0, 0, 0, 1);
        t0 = max((top - ro.z) / rd.z, 0.0);
        t1 = (bottom - ro.z) / rd.z;
    } else {
        if (ro.z < bottom || ro.z > top) return float4(0, 0, 0, 1);
        t0 = 0.0; t1 = (top - bottom) * 64.0;
    }
    const int kSteps = 24;
    float featureSize = 1.0 / max(gCloudMotion.z, 1e-9);
    t1 = min(t1, t0 + kSteps * featureSize * 0.35);
    if (t1 <= t0) return float4(0, 0, 0, 1);

    float dt = (t1 - t0) / kSteps;
    float jitter = averHash13(rd * 811.7);
    float t = t0 + dt * jitter;

    float sigma = averCloudSigma();
    float3 scattered = 0.0;
    float transmittance = 1.0;
    float phase = averHG(dot(rd, sunDir), 0.62);

    [loop] for (int i = 0; i < kSteps; ++i) {
        if (transmittance < 0.02) break;
        float3 p = ro + rd * t;
        float d = averCloudDensity(p, true);
        if (d > 0.001) {
            float lt = 0.0;
            float lstep = (top - bottom) * 0.25;
            [unroll] for (int j = 0; j < 3; ++j) {
                float3 lp = p + sunDir * (lstep * (j + 0.5));
                lt += averCloudDensity(lp, false) * lstep;
            }
            float sunT = exp(-lt * sigma);
            float powder = 1.0 - exp(-d * dt * sigma * 2.0);
            float3 lit = sunColour * sunT * phase * powder;
            lit += skyColorFull(float3(0, 0, 1)) * 0.9;

            float stepT = exp(-d * dt * sigma);
            scattered += transmittance * lit * (1.0 - stepT);
            transmittance *= stepT;
        }
        t += dt;
    }
    return float4(scattered, transmittance);
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
