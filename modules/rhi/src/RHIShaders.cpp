// The HLSL every GPU consumer shares: the constant-buffer layouts the backend uploads, the vertex
// structures it feeds, the colour-space helpers, and the camera post. The BRDF is NOT here any
// more - pbr::materialShaderPrelude() owns it, because a material system is a module and not a
// property of the hardware interface.
//
// It lives here rather than in a backend or a feature module because it is a cross-module ABI with
// no compiler behind it: `cbuffer PerFrame` must match PerFrameCB field for field, and the mesh
// geometry registers must match what dispatchMeshFor() binds. Duplicating any of it would make
// reordering one copy corrupt the other with no diagnostic anywhere.
#include "aver/rhi/RHIResources.hpp"
#include "aver/rhi/RHI.hpp"

#include <cmath>
#include <string>

namespace aver::rhi {

void SkyAtmosphere::setSunAngles(f32 elevationDeg, f32 azimuthDeg) {
    constexpr f32 kDeg = 3.14159265358979f / 180.0f;
    const f32 el = elevationDeg * kDeg, az = azimuthDeg * kDeg;
    const f32 ce = std::cos(el);
    sunDirection[0] = ce * std::cos(az);
    sunDirection[1] = ce * std::sin(az);
    sunDirection[2] = std::sin(el);
}

void SkyAtmosphere::sunAngles(f32& elevationDeg, f32& azimuthDeg) const {
    constexpr f32 kRad = 180.0f / 3.14159265358979f;
    const f32 x = sunDirection[0], y = sunDirection[1], z = sunDirection[2];
    const f32 len = std::sqrt(x * x + y * y + z * z);
    if (len < 1e-6f) { elevationDeg = 0.0f; azimuthDeg = 0.0f; return; }
    elevationDeg = std::asin(z / len) * kRad;
    // atan2 of a zero pair is 0 rather than undefined, which is the right answer for a sun straight
    // overhead: every azimuth is equivalent there, so any of them is correct.
    azimuthDeg = std::atan2(y, x) * kRad;
}

// Planckian locus to linear sRGB, normalised so the brightest channel is 1 — the value is a COLOUR,
// and the brightness is sunIntensity's job. Krystek's rational fit for CIE 1960 uv over
// 1000..15000 K, then uv -> xy -> XYZ -> linear sRGB.
//
// A fit and not a spectral integral on purpose: the difference over the range a sun is authored in
// is far below what an 8-bit display can show, and a table would be one more thing to get subtly
// wrong with no way to notice.
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

// Colour space and tonemapping, factored out so BOTH the scene prelude and the post chain below are
// built from the same text. They are the two ends of one round trip -- the scene writes linear
// radiance, the post chain tonemaps and encodes it, and an editor line inverts the pair to name a
// display colour -- so a second copy of any of these would be a divergence that shows up as a grid
// whose grey no longer matches the one it was authored as, and nothing to attribute it to.
const char* kColorHlsl = R"(
float3 acesTonemap(float3 x){ return saturate((x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14)); }
float3 toGamma(float3 c){ return pow(max(c,0.0), 1.0/2.2); }
float3 srgbToLin(float3 c){ return pow(max(c,0.0), 2.2); }

// The EXACT inverse of acesTonemap, for the handful of things authored as a DISPLAY colour rather
// than as radiance: editor lines, gizmos, and the clear colour behind them all name the pixel they
// want on screen. The scene target is HDR and the tonemap runs at the end of the frame, so writing
// such a colour straight into it would put it through a curve it was never meant to see.
//
// The fit is a ratio of quadratics, so inverting it is a quadratic in x:
//   (2.43y - 2.51)x^2 + (0.59y - 0.03)x + 0.14y = 0
// and the root wanted is the positive one. Not an approximation and not a lookup: a display colour
// that survives the round trip only APPROXIMATELY is a grid whose grey drifts every time the post
// chain is touched.
float3 averInverseTonemap(float3 y) {
    // The fit asymptotes at 2.51/2.43; a value at or past it has no finite pre-image, so clamp just
    // below rather than divide by a vanishing discriminant.
    y = clamp(y, 0.0, 1.0329 - 1e-4);
    float3 a = 2.43 * y - 2.51;
    float3 b = 0.59 * y - 0.03;
    float3 c = 0.14 * y;
    float3 disc = sqrt(max(b * b - 4.0 * a * c, 0.0));
    // a is strictly negative over the clamped range, so (-b - disc) / (2a) is the positive root.
    return (-b - disc) / (2.0 * a);
}

// Rec. 709 relative luminance. Used by the bloom threshold, the Karis firefly weight and the
// exposure histogram, which must agree on what "bright" means or a highlight can be above the
// bloom threshold and below the histogram's.
float averLuminance(float3 c){ return dot(c, float3(0.2126, 0.7152, 0.0722)); }
)";

} // namespace

// The HLSL below hardcodes these; the C++ side reads the constants. Neither can move alone.
static_assert(kMeshShaderTrisPerGroup == 64, "AVER_MS_TRIS in the prelude is written out as 64");
static_assert(kMeshGeometryConstantRegister == 5, "MeshCB in the prelude is written out as b5");
static_assert(kObjectConstantRegister == 1, "PerObject in the prelude is written out as b1");
static_assert(kObjectConstantDwords == 32,
              "PerObject below is 32 dwords: world 16, base colour 4, material 4, model 4, emissive 4");

const char* sharedShaderPrelude() {
    // Composed once and cached. The pointer is held for the life of the process by every
    // ShaderDesc::prelude that borrows it, so the storage has to outlive every pipeline build.
    static const std::string s = std::string(kColorHlsl) + R"(
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
    // ---- the authored atmosphere. MIRRORS PerFrameCB's tail field for field. ----
    float4   gSkyParams;   // x atmosphere height, y sky-light intensity, z sun intensity, w cos(sun radius)
    float4   gGroundColor; // rgb below the horizon, a = how much of it replaces the sky
    float4   gFogParams;   // x height falloff, y fog height, z start distance, w max opacity
    float4   gCloudParams; // x coverage, y density, z layer bottom, w layer top
    float4   gCloudMotion; // xy wind offset, z 1/feature size, w enabled
};
// Per draw. The shading model is an ID rather than a shader permutation: a uniform branch costs one
// scalar compare per wave, where a permutation would multiply the pipeline count of every renderer
// that draws scene geometry -- Voxi already builds eleven and rebuilds four of them on every MSAA
// change.
cbuffer PerObject : register(b1) {
    float4x4 gWorld;
    float4   gBaseColor;
    // z is the unlit flag the FROZEN no-material path below still reads. The material system reads
    // gShadingModel instead; the two are not kept in step, because the frozen path exists precisely
    // so that it never has to be.
    float4   gMaterial;      // x=metallic, y=roughness, z=unlit(0/1) for plainShadeSurface only
    uint     gShadingModel;  // which shading model evaluates this draw
    // DEAD as of the material system owning reflectance: the shading model reads gMatReflectance /
    // gMatF90 out of b2, and the frozen path below hardcodes 0.04 with F(90) = 1. They are left
    // declared, and still written, only because removing them moves kObjectConstantDwords and
    // therefore every root signature that declares b1. Reclaim them together, not one at a time.
    float    gReflectance;   // unread
    float    gF90;           // unread
    float    _objPad;
    float4   gEmissive;      // rgb, radiance this surface emits on its own
};

static const float PI = 3.14159265;

// The sky dome. Above the horizon it is a horizon-to-zenith blend whose EXPONENT is the authored
// atmosphere height: a small exponent pushes the pale band high and reads as thick hazy air, a large
// one pulls it to a thin bright line and reads as thin high-altitude air. Below the horizon it fades
// to the ground albedo instead of continuing the sky underneath the camera, which is visible the
// moment anything reflective looks down.
// The sun's radiance as every lighting path must see it: the authored colour, decoded, times the
// authored intensity. ONE definition, because the two paths that used to compute this separately
// disagreed by a factor of three -- the lit pass multiplied by 3 and the GI injection did not, so
// every bounce in the engine was a third as bright as the light that produced it. Colour bleeding
// was not missing; it was there and two thirds too dark to see.
float3 averSunRadiance() { return srgbToLin(gLightColor.rgb) * gSkyParams.z; }

// The dome ABOVE the horizon. Split out from skyColorFull so the ground below it can be LIT by the
// sky without the two calling each other.
//
// The blend parameter is dir.z remapped from [-1,1] to [0,1], which is the curve this engine has
// always used; gSkyParams.x is the exponent, so the horizon band's height is authorable either way.
// Interpolated in LINEAR light: blending two sRGB triples and decoding the result is not the same
// curve as decoding both and blending, and the encode being concave made every midpoint of the sky
// sag darker than the radiances it lies between.
float3 averSkyAbove(float3 dir) {
    return lerp(srgbToLin(gSkyHorizon.rgb), srgbToLin(gSkyZenith.rgb),
                pow(saturate(dir.z * 0.5 + 0.5), gSkyParams.x));
}

// What the ground below the horizon actually RADIATES.
//
// gGroundColor is an ALBEDO, and it was being used directly as a radiance -- so the lower half of
// the dome was a flat unlit swatch that read as a painted wall standing behind the scene rather than
// as a surface receding into the distance. The ground is a surface: what a camera sees is its albedo
// times the light falling on it, over pi. It faces up, so it collects the sun at cos(elevation) and
// the whole upper hemisphere of sky.
float3 averGroundRadiance() {
    float3 albedo = srgbToLin(gGroundColor.rgb);
    float  ndl    = saturate(normalize(gLightDir.xyz).z);
    // The sky's contribution to a horizontal surface: a hemisphere of radiance L delivers PI*L.
    float3 E = averSunRadiance() * ndl + PI * averSkyAbove(float3(0, 0, 0.5)) * gAmbient.r;
    return albedo * E / PI;
}

float3 skyColorFull(float3 dir)
{
    float3 above = averSkyAbove(dir);
    // The ground fades in across the whole lower hemisphere, not in a seven-degree band at the
    // horizon. The old saturate(-dir.z * 8.0) reached full ground within about seven degrees, which
    // put a hard edge under the horizon and a flat colour everywhere below it -- exactly the two
    // things that make a dome read as a wall. A smoothstep over the first twenty degrees leaves the
    // near-horizon band sky-coloured, which is also what aerial perspective does: that is where the
    // air between you and the ground is deepest.
    float g = smoothstep(0.0, 0.35, saturate(-dir.z)) * gGroundColor.a;
    return lerp(above, averGroundRadiance(), g);
}
// The one every shading path already calls. It forwards to skyColorFull so the authored atmosphere
// reaches the AMBIENT term as well as the visible dome -- the sky is the fill light, and a version
// of it that only the camera could see would light the scene from a sky nobody was looking at.
//
// The 0.5+0.5 remap the old body used is kept inside skyColorFull's `saturate(up)` for directions
// above the horizon; below it, the ground now answers instead of the sky continuing underneath.
float3 skyColor(float3 dir){ return skyColorFull(dir); }

// The sky as LIGHT: the cosine-weighted average radiance over the hemisphere a surface with normal
// N can see. This is what a diffuse ambient term needs, and it is NOT skyColor(N).
//
// skyColor(N) is the radiance arriving from ONE direction. Using it as the ambient stands a single
// sample in for an integral over every direction the surface sees, and for a two-colour dome the
// error is up to the whole horizon-to-zenith difference: a floor was lit by pure zenith blue when
// half of what it actually sees is the pale horizon, and a wall by the horizon alone when half of
// what it sees is sky above it and half is ground below.
//
// Solved by sampling the dome's own gradient at the cosine-weighted mean direction of the visible
// hemisphere rather than by a runtime integral. For a hemisphere about N the cosine-weighted mean
// of dir.z is (1 + N.z) / 3 remapped onto the dome's parameterisation, which is exact for a dome
// linear in dir.z and a close fit for the authored exponent -- and it costs one lerp, where a real
// integral costs a sample loop per pixel.
float3 averSkyIrradiance(float3 N) {
    // The mean height a hemisphere about N sees, in the same [-1,1] space the dome is authored in.
    float meanZ = N.z * 0.5;
    float3 dome = skyColorFull(float3(0.0, 0.0, meanZ));
    // A surface also sees the ground when it tilts down, and the dome function already blends it in
    // below the horizon -- but the MEAN direction never goes below -0.5, so the ground would be
    // under-represented for a downward-facing surface. Fold in the fraction of the hemisphere that
    // is below the horizon explicitly.
    float belowFraction = saturate(0.5 - N.z * 0.5) * gGroundColor.a;
    // The LIT ground, for the same reason the visible dome uses it: bouncing a raw albedo into the
    // scene as though it were radiance makes the ground a light source of its own brightness rather
    // than a surface reflecting the sky.
    return lerp(dome, averGroundRadiance(), belowFraction * 0.5);
}

// ---- PBR mesh with sky ambient + distance fog ----
struct VSIn  { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_POSITION; float3 nrmWS : NORMAL; float3 wpos : TEXCOORD0; float2 uv : TEXCOORD1; };

VSOut VSMain(VSIn i) {
    VSOut o;
    float4 wp = mul(float4(i.pos, 1.0), gWorld);
    o.wpos = wp.xyz;
    o.pos = mul(wp, gViewProj);
    o.nrmWS = mul(float4(i.nrm, 0.0), gWorld).xyz;
    o.uv = i.uv;
    return o;
}

// ---- the image you get with NO material module: FROZEN, do not evolve ----
// Aver.Render.PBR.Materials owns the BRDF now, and this backend cannot link it: the material system
// depends on Aver.RHI, so a backend that depended on the material system would invert the layering.
// What stays here is therefore the shading exactly as it stood before the contract was split out,
// under private names so this string and pbr::materialShaderPrelude() can be concatenated.
//
// It is a verbatim copy on purpose. A hand-written lambert or unlit substitute would be a silent
// divergence with no oracle behind it and nothing to catch it: PSMainPlain is unreachable in any
// Voxi-enabled build, so no gate would ever look at the result.
float3 plainFresnelSchlick(float ct, float3 F0){ return F0 + (1.0-F0)*pow(saturate(1.0-ct),5.0); }
float plainDistGGX(float ndh, float a){ float a2=a*a; float d=ndh*ndh*(a2-1.0)+1.0; return a2/(PI*d*d+1e-6); }
float plainGeomSchlick(float nd, float k){ return nd/(nd*(1.0-k)+k); }

float4 plainShadeSurface(VSOut i, float sunVis, float3 indirectRadiance, float ao) {
    float3 N = normalize(i.nrmWS);
    float3 V = normalize(gCamPos.xyz - i.wpos);
    float3 L = normalize(gLightDir.xyz);
    float3 H = normalize(V + L);
    float metallic = saturate(gMaterial.x);
    float rough = clamp(gMaterial.y, 0.045, 1.0);

    if (gMaterial.z > 0.5) { // unlit (gizmo/grid): authored display colour, no lighting
        return float4(gBaseColor.rgb, gBaseColor.a);
    }

    float3 albedo = srgbToLin(gBaseColor.rgb);
    float3 lightC = srgbToLin(gLightColor.rgb) * 3.0; // sun radiance
    float ndv = saturate(dot(N, V));
    float ndl = saturate(dot(N, L));
    float3 F0 = lerp(0.04.xxx, albedo, metallic);

    // direct (Cook-Torrance GGX)
    float a = rough * rough;
    float k = (rough + 1.0); k = k * k / 8.0;
    float D = plainDistGGX(saturate(dot(N, H)), a);
    float G = plainGeomSchlick(ndv, k) * plainGeomSchlick(ndl, k);
    float3 F = plainFresnelSchlick(saturate(dot(H, V)), F0);
    float3 spec = (D * G * F) / (4.0 * ndv * ndl + 1e-4);
    float3 kd = (1.0 - F) * (1.0 - metallic);
    float3 direct = (kd * albedo / PI + spec) * lightC * ndl * sunVis;

    // ambient: sky hemisphere irradiance (linear) + crude spec reflection of the sky
    float3 ambient = kd * albedo * skyColor(N) * gAmbient.r;
    float3 R = reflect(-V, N);
    float3 envSpec = skyColor(R) * plainFresnelSchlick(ndv, F0) * (1.0 - rough);

    float3 indirect = kd * albedo * indirectRadiance;
    ambient *= ao;   // whatever produced the indirect term already knows what is occluded
    float3 color = direct + ambient + indirect + envSpec * 0.35;

    // distance fog (linear space)
    float dist = length(i.wpos - gCamPos.xyz);
    float fog = 1.0 - exp(-dist * gFogColor.a);
    color = lerp(color, srgbToLin(gFogColor.rgb), saturate(fog));

    // LINEAR RADIANCE, not a display colour. The scene target is HDR and the tonemap is the last
    // thing that happens to the frame, in the post chain — a shader that tonemapped here would be
    // tonemapping twice, and would also destroy the range bloom and eye adaptation exist to read.
    return float4(color, gBaseColor.a);
}

// ---- camera / post. Deliberately NOT the material's: fog, tonemap and gamma are properties of
// the camera looking at the scene, and a material system that owned them would make every shading
// model reimplement them identically. ----
// EXPONENTIAL HEIGHT FOG, solved analytically along the view ray rather than marched.
//
// For a density that falls off with altitude, d(z) = d0 * exp(-(z - h0) * k), the optical depth
// along a segment has a closed form: the integral of exp(-k z) over a straight line is
// (1 - exp(-k dz)) / (k dz) times the density at the near end, times the segment length. So the
// whole thing is one exp and one divide, and it is EXACT rather than a sum of slabs.
//
// k == 0 collapses it to d0 * length, which is the uniform distance fog this engine had -- a grey
// veil that thickens with distance alone, so a mountain top is as hazy as the valley floor. Any
// positive falloff gives what people mean by fog: haze that pools low and clears with altitude.
// The default is 0, so nothing moves until somebody authors a falloff.
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
        // The dz -> 0 limit of (1 - exp(-x))/x is 1, and the guard is on the PRODUCT because that is
        // the quantity that vanishes -- a horizontal ray through thick fog has a large seg and a
        // zero dz, and testing dz alone would divide by nothing at exactly the common case.
        float f = abs(kdz) > 1e-4 ? (1.0 - exp(-kdz)) / kdz : 1.0;
        tau = d0 * exp(-(a.z - gFogParams.y) * k) * seg * f;
    }
    return saturate(1.0 - exp(-tau)) * gFogParams.w;
}

// The in-scatter target is THE SKY ALONG THE VIEW RAY, tinted by the authored fog colour.
//
// It used to be a constant. A ray that hits nothing has infinite optical depth, so its in-scattered
// radiance is the limit distant geometry converges to -- and with a constant target the two limits
// were independently authored numbers that did not match. Distant ground came out brighter than the
// sky above it (the opposite of aerial perspective) with a hard step at the horizon wherever they
// met. Taking the sky as the target makes them agree BY CONSTRUCTION: there is no pair of values
// left that can disagree.
//
// gFogColor is a TINT on that, not a replacement, so a level can still say "the air here is warmer
// than the sky" without reintroducing a second horizon.
float3 averFogInscatter(float3 wpos) {
    float3 dir = normalize(wpos - gCamPos.xyz);
    return skyColorFull(dir) * srgbToLin(gFogColor.rgb);
}

float3 averApplyFog(float3 color, float3 wpos) {
    return lerp(color, averFogInscatter(wpos), averFogFactor(wpos));
}

// ================= volumetric clouds =================
// A single raymarched layer, evaluated ONLY on sky pixels. That is the whole of what makes it cheap:
// the sky is a fullscreen pass drawn before the scene with no depth write, so every pixel geometry
// covers costs nothing at all, and the march is bounded to the slab the ray actually crosses.
//
// The noise is ANALYTIC rather than a 3D texture. A texture would be faster per sample, but it would
// also need an SRV in the scene root signature -- which today declares no descriptor table at all --
// and that means changing a root signature every scene pipeline in the engine is built against. A
// hash is a few ALU and needs nothing.

float averHash13(float3 p) {
    p = frac(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return frac((p.x + p.y) * p.z);
}

// Value noise with a smoothstep-interpolated lattice. Eight hashes a call, which is why the octave
// counts below are as low as they are.
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

// Cloud density at a world point. `detail` buys a second octave; the light march passes 0 for it,
// because a shadow ray through a cloud is integrating a bulk quantity and the fine octave costs
// eight hashes to change it by almost nothing.
// Extinction per world unit for a fully dense cloud. Derived from the layer's THICKNESS rather
// than authored directly, so `cloudDensity` is a unitless "how thick do these look" dial that means
// the same thing in a centimetre world and a metre one. Authored per-unit it would not: a density of
// 1 through a 1.3 km layer is an optical depth of 130000, which is opaque by a factor of ten
// thousand, and that is exactly what the first version of this rendered.
float averCloudSigma() {
    const float kOpticalDepthAtFull = 9.0;   // a dense cumulus, edge to edge
    return gCloudParams.y * kOpticalDepthAtFull / max(gCloudParams.w - gCloudParams.z, 1.0);
}

float averCloudDensity(float3 wpos, bool detail) {
    float bottom = gCloudParams.z, top = gCloudParams.w;
    // Height gradient: clouds have flat-ish bases and billowing tops, so the profile rises fast and
    // tapers slowly. Zero at both faces means the march never produces a hard-edged slab.
    float h = saturate((wpos.z - bottom) / max(top - bottom, 1.0));
    float shape = saturate(h * 4.0) * saturate((1.0 - h) * 1.6);
    if (shape <= 0.001) return 0.0;

    float3 p = (wpos + float3(gCloudMotion.xy, 0.0)) * gCloudMotion.z;
    float n = averValueNoise(p) * 0.6;
    if (detail) n += averValueNoise(p * 3.17) * 0.3;
    else        n += 0.15;   // the mean of the octave being skipped, so the two agree on average
    n += averValueNoise(p * 0.41) * 0.25;

    // Coverage REMAPS rather than scales: subtracting a threshold and rescaling is what turns a
    // noise field into distinct clouds with clear sky between them. Scaling would just make one
    // continuous overcast lighter or darker.
    float cover = 1.0 - gCloudParams.x;
    float d = saturate((n - cover) / max(1.0 - cover, 1e-3));
    return d * shape;
}

// Henyey-Greenstein. Clouds are strongly forward-scattering, which is why the sky around the sun
// glows and the same cloud looks dark from the other side.
float averHG(float ct, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * ct, 1e-4), 1.5));
}

// Marches the layer and returns scattered radiance in rgb, TRANSMITTANCE in a.
float4 averCloudLayer(float3 ro, float3 rd, float3 sunDir, float3 sunColour) {
    if (gCloudMotion.w < 0.5) return float4(0, 0, 0, 1);
    float bottom = gCloudParams.z, top = gCloudParams.w;

    // Slab entry and exit. A ray heading down, or up from above the layer, never enters it -- and
    // returning early there is most of why this is affordable at all.
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
        t0 = 0.0; t1 = (top - bottom) * 64.0;   // grazing: bounded rather than infinite
    }
    const int kSteps = 24;
    // THE SPAN IS CAPPED BY THE NOISE'S FEATURE SIZE, not by a multiple of the layer thickness.
    //
    // A near-horizontal ray crosses an unbounded amount of the slab, and a fixed step count over an
    // unbounded span means a step that eventually exceeds one whole noise feature -- at which point
    // consecutive samples are uncorrelated and the layer renders as speckle rather than as cloud.
    // Capping the span so dt stays a fraction of a feature makes that impossible by construction,
    // whatever units the world is in, and the cost is that a grazing ray stops marching early and
    // fades out instead of aliasing.
    float featureSize = 1.0 / max(gCloudMotion.z, 1e-9);
    t1 = min(t1, t0 + kSteps * featureSize * 0.35);
    if (t1 <= t0) return float4(0, 0, 0, 1);

    float dt = (t1 - t0) / kSteps;
    // Dithered start, from the direction alone. Without it the fixed step size shows as concentric
    // rings; with it the same error becomes noise, which the eye forgives and a blur would remove.
    float jitter = averHash13(rd * 811.7);
    float t = t0 + dt * jitter;

    float sigma = averCloudSigma();
    float3 scattered = 0.0;
    float transmittance = 1.0;
    float phase = averHG(dot(rd, sunDir), 0.62);

    [loop] for (int i = 0; i < kSteps; ++i) {
        if (transmittance < 0.02) break;   // nothing behind this can still be seen
        float3 p = ro + rd * t;
        float d = averCloudDensity(p, true);
        if (d > 0.001) {
            // Light march: a few long steps toward the sun. Long on purpose -- this is integrating
            // bulk occlusion, and the error from a coarse step is a slightly softer cloud, which is
            // indistinguishable from a slightly different cloud.
            float lt = 0.0;
            float lstep = (top - bottom) * 0.25;
            [unroll] for (int j = 0; j < 3; ++j) {
                float3 lp = p + sunDir * (lstep * (j + 0.5));
                lt += averCloudDensity(lp, false) * lstep;
            }
            float sunT = exp(-lt * sigma);
            // Powder: the darkening on the sun-facing side of a cloud that pure Beer's law misses,
            // because light scattered INTO the eye has to have entered the cloud first.
            float powder = 1.0 - exp(-d * dt * sigma * 2.0);
            float3 lit = sunColour * sunT * phase * powder;
            // Ambient from the sky above the sample, so an overcast base is not simply black.
            lit += skyColorFull(float3(0, 0, 1)) * 0.9;

            float stepT = exp(-d * dt * sigma);
            // Energy-conserving accumulation: the light scattered by this step is what it emits
            // minus what it absorbs, integrated exactly rather than as t * d * dt.
            scattered += transmittance * lit * (1.0 - stepT);
            transmittance *= stepT;
        }
        t += dt;
    }
    return float4(scattered, transmittance);
}

// ---- procedural sky (fullscreen triangle via SV_VertexID) ----
struct SkyOut { float4 pos : SV_POSITION; float2 ndc : TEXCOORD0; };
SkyOut VSky(uint id : SV_VertexID) {
    SkyOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.ndc = uv * 2.0 - 1.0;
    o.pos = float4(o.ndc, 1.0, 1.0);
    return o;
}

#if AVER_MS
// ================= Mesh shader geometry path =================
// A mesh shader has no input assembler, so it reads the vertex and index buffers itself. Both are
// bound as ROOT SRVs (raw/structured buffers are allowed there), which keeps the per-mesh cost at
// two GPU virtual addresses and needs no descriptor heap slots at all.
//
// One thread group per 64 triangles, expanded unindexed: 192 vertices / 64 primitives, inside the
// 256/256 per-group limit. Skipping vertex reuse costs a little duplicate transform work and buys
// a much simpler shader; meshlet building belongs with the asset pipeline, not here.
//
// These three registers are the ones RHIResources.hpp reserves for geometry the backend binds
// itself. A feature module declaring anything at them collides silently.
//
// The two SRV registers sit just PAST whatever SRV table the pipeline declared, so they depend on
// layout.srvCount and cannot be written here as literals: a literal agrees with the root signature
// only while that count happens to match, and the day a layout grows an SRV the shader keeps asking
// for a register the root signature has moved something else into. That failure is invisible — at
// best CreateRootSignature rejects the overlap, at worst the mesh shader reads a texture as a
// vertex buffer, with no compile error and nothing for the debug layer to say. Hence -D macros,
// computed by the backend from the same field, and a hard error rather than a default if a caller
// forgets them.
#if !defined(AVER_MS_VTX_REG) || !defined(AVER_MS_IDX_REG)
#error "AVER_MS needs AVER_MS_VTX_REG / AVER_MS_IDX_REG from rhi::meshGeometryDefines"
#endif
// Two-step so the argument is expanded before it is pasted; one step pastes the macro NAME.
#define AVER_REG_JOIN2(a, b) a##b
#define AVER_REG_JOIN(a, b) AVER_REG_JOIN2(a, b)

// The element size of this struct IS the vertex stride on the mesh-shader path: gVerts is bound as
// a root SRV by raw GPU address, so nothing but this declaration tells the shader how far apart the
// vertices are. It must match rhi::MeshVertex byte for byte.
struct MeshVtx { float3 pos; float3 nrm; float2 uv; };
StructuredBuffer<MeshVtx> gVerts   : register(AVER_REG_JOIN(t, AVER_MS_VTX_REG));
ByteAddressBuffer         gIndices : register(AVER_REG_JOIN(t, AVER_MS_IDX_REG));
cbuffer MeshCB : register(b5) { uint gTriCount; uint3 _msPad; };

#define AVER_MS_TRIS 64

// Triangles this group owns, and where its slice of the output arrays starts.
uint msTriCount(uint gid) { return min(AVER_MS_TRIS, gTriCount - gid * AVER_MS_TRIS); }

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
        ov.nrmWS = mul(float4(v.nrm, 0.0), gWorld).xyz;
        ov.uv    = v.uv;
        verts[o + k] = ov;
    }
    tris[gtid] = uint3(o, o + 1, o + 2);
}
#endif // AVER_MS
)";
    return s.c_str();
}

// One owner for both halves of the reserved-register contract: the prelude above consumes these
// macros, the root-signature builder places the matching root SRVs at the same two indices, and
// both read declaredSrvCount(layout) rather than a number written down twice. It is the count
// across BOTH tables: table 1 is based at t(srvCount), so anything anchored to srvCount alone would
// now land on top of it.
std::string meshGeometryDefines(const PipelineLayout& layout) {
    const u32 base = declaredSrvCount(layout);
    return "AVER_MS_VTX_REG=" + std::to_string(base) +
           ";AVER_MS_IDX_REG=" + std::to_string(base + 1);
}

const char* postShaderSource() {
    static const std::string s = std::string(kColorHlsl) + R"(
// ================= the camera post chain =================
// Deliberately NOT built on the scene prelude. Every pass here runs with a root signature of its
// own, and declaring `cbuffer PerFrame : register(b0)` alongside this one would be two different
// layouts at the same register in one translation unit.
//
// One constant buffer serves every pass. A pass reads the fields it needs and ignores the rest,
// which is what lets the whole chain share one root signature and one upload -- seven small
// constant buffers would each be a separate suballocation and a separate root binding for what is
// never more than a dozen floats.
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

// Only the two adaptation passes write, and both write BUFFERS -- see the note on the bloom chain
// for why nothing here writes a texture through a UAV.
RWByteAddressBuffer   gPostHist     : register(u0);
RWByteAddressBuffer   gPostExp      : register(u1);

// ---- bloom ---------------------------------------------------------------------------------
// The pyramid is a HALF-resolution RGBA16F chain: a bloom halo is low-frequency by definition, so
// building it at full resolution buys nothing visible and costs four times the bandwidth. Every
// step below reads through a bilinear sampler, which is what makes a 13-tap downsample cost 13
// samples instead of 52.
//
// GRAPHICS passes, not compute, and that is a portability decision rather than a stylistic one.
// The upsample is additive, and a compute version would have to READ its own RWTexture2D — but a
// typed UAV load of RGBA16F is an optional D3D12 feature (this engine has a whole `no-typed-uav`
// gate configuration for hardware without it), so that shader would simply not run on some of the
// devices the oracle covers. The fixed-function blender adds for free and is universal.
//
// Each pass reads ONE mip through its own single-mip SRV and writes the next through an RTV, which
// is what makes reading and writing the same resource in one draw legal: the two subresources are
// in different states.

// Karis' firefly weight. A single pixel far brighter than its neighbours survives every box filter
// in the chain and re-emerges as a large flickering blob; weighting each tap by 1/(1+luma) averages
// in a space where that pixel cannot dominate. Applied ONLY on the first downsample, because after
// it the energy is already spread and further weighting would just darken the halo.
float3 averBloomKaris(float3 c) { return c / (1.0 + averLuminance(c)); }

// Soft-knee threshold, the standard quadratic ramp. A hard cut makes bloom appear and disappear as
// a highlight crosses it, which reads as flicker in motion and is far more objectionable than the
// halo the threshold exists to control.
// The exposure this frame settled on, as the bloom threshold must see it. gPostMisc.y is the
// auto-exposure flag; with it off the authored value applies.
float averPostExposure() {
    return gPostMisc.y > 0.5 ? asfloat(gPostExpRead.Load(0)) : gPostTone.x;
}

float3 averBloomPrefilter(float3 c) {
    float br = max(c.r, max(c.g, c.b));
    float knee = max(gPostTone.w, 1e-4);
    float soft = clamp(br - gPostTone.z + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    float weight = max(soft, br - gPostTone.z) / max(br, 1e-4);
    return c * weight;
}

struct AverPostVSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

AverPostVSOut PostVS(uint id : SV_VertexID) {
    AverPostVSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 PSBloomPrefilter(AverPostVSOut i) : SV_TARGET {
    // Sampling at the destination texel centre with a bilinear tap over a source twice as large is
    // exactly a 2x2 box, so four taps here cover a 4x4 source neighbourhood.
    float2 o = gPostSrc.zw;
    float3 a = gPostSceneTex.SampleLevel(gPostSamp, i.uv + float2(-o.x, -o.y), 0).rgb;
    float3 b = gPostSceneTex.SampleLevel(gPostSamp, i.uv + float2( o.x, -o.y), 0).rgb;
    float3 c = gPostSceneTex.SampleLevel(gPostSamp, i.uv + float2(-o.x,  o.y), 0).rgb;
    float3 d = gPostSceneTex.SampleLevel(gPostSamp, i.uv + float2( o.x,  o.y), 0).rgb;
    float3 sum = averBloomKaris(a) + averBloomKaris(b) + averBloomKaris(c) + averBloomKaris(d);
    // EXPOSED before it is thresholded. The threshold asks "is this brighter than white?", and white
    // is a property of the exposed image, not of the raw radiance. Thresholding unexposed radiance
    // meant that any scene needing an exposure above 1 -- which is any scene darker than a lit
    // studio, SkyForge included at about 3x -- had nothing at all cross the threshold, so bloom was
    // switched on, costing a full pyramid every frame, and producing exactly nothing.
    return float4(averBloomPrefilter(sum * 0.25 * averPostExposure()), 1.0);
}

// Jimenez' 13-tap downsample (Call of Duty: Advanced Warfare, SIGGRAPH 2014). The extra centre box
// over a plain 4-tap is what stops the chain aliasing into a shimmering mess as the camera moves.
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

// 9-tap tent upsample, ADDED into the level above by the blender rather than replacing it. Additive
// is what makes the pyramid a sum of scales -- a halo with a tight core and a wide skirt -- instead
// of only the coarsest level smeared back over the frame.
float4 PSBloomUp(AverPostVSOut i) : SV_TARGET {
    // The radius is in DESTINATION uv, not source texels, so the skirt keeps a constant angular
    // width as the chain gets coarser instead of doubling with every level.
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
// A 256-bin histogram of log2 luminance, reduced to one exposure value that is damped towards over
// time. A histogram rather than an average because an average is dominated by whatever occupies the
// most pixels: a bright sky closes the aperture on an interior, and one specular highlight closes
// it on everything.
groupshared uint gHistLocal[256];

[numthreads(16, 16, 1)]
void CSHistogram(uint3 tid : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    gHistLocal[gi] = 0;
    GroupMemoryBarrierWithGroupSync();

    // Sampled over a REDUCED grid rather than one thread per pixel. A histogram is a statistic, and
    // bilinear taps at quarter resolution give the same answer for a sixteenth of the threads --
    // which at a 2750x1711 viewport is the difference between 4.7 million threads a frame and
    // 300 thousand. gPostDst is the grid, gPostDst.zw its reciprocal.
    if (tid.x < (uint)gPostDst.x && tid.y < (uint)gPostDst.y) {
        float2 uv = (tid.xy + 0.5) * gPostDst.zw;
        float lum = averLuminance(gPostSceneTex.SampleLevel(gPostSamp, uv, 0).rgb);
        // Bin 0 is reserved for "black". Without it, log2 of a near-zero luminance floors into the
        // first real bin and a dark scene reads as if it were full of very dim detail, which drives
        // the adaptation to its maximum and blows out the first thing that lights up.
        uint bin = 0;
        if (lum > 1e-4) {
            float t = saturate((log2(lum) - gPostAdapt.x) * gPostAdapt.y);
            bin = (uint)(t * 254.0 + 1.0);
        }
        InterlockedAdd(gHistLocal[bin], 1);
    }
    GroupMemoryBarrierWithGroupSync();

    // One global atomic per BIN per group rather than one per pixel. That is the whole reason for
    // the groupshared pass above: 256 atomics per 256 pixels instead of 65536.
    uint local = gHistLocal[gi];
    if (local > 0) gPostHist.InterlockedAdd(gi * 4, local);
}

// Single-threaded on purpose. It is 256 iterations once per frame against a parallel reduction's
// several dispatches and barriers -- the reduction would be the slower of the two here, and it is
// the one with somewhere to hide an off-by-one.
[numthreads(1, 1, 1)]
void CSExposure() {
    float target = gPostTone.x;

    if (gPostMisc.y > 0.5) {
        uint total = 0;
        uint counts[256];
        for (uint i = 0; i < 256; ++i) { counts[i] = gPostHist.Load(i * 4); total += counts[i]; }

        // Trim both tails before averaging. Bin 0 (black) is excluded outright: sky-free interiors
        // are mostly it, and letting it into the average drags the exposure to the maximum.
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

        // Zeroed here rather than in a clear pass of its own: this is the one thread that has
        // finished reading it, so there is no barrier to get wrong.
        for (uint z = 0; z < 256; ++z) gPostHist.Store(z * 4, 0);
    }

    // Damped towards, in LOG space, so the same alpha takes the same time per stop whether the
    // scene got brighter or darker. Linear damping adapts to darkness far slower than to light and
    // reads as the eye sticking.
    float prev = asfloat(gPostExp.Load(0));
    uint  seeded = gPostExp.Load(4);
    float next = (seeded == 0 || prev <= 0.0)
               ? target
               : exp2(lerp(log2(prev), log2(max(target, 1e-4)), saturate(gPostAdapt.z)));
    gPostExp.Store(0, asuint(next));
    gPostExp.Store(4, 1);
}

// ---- composite -----------------------------------------------------------------------------
// The one place the frame becomes a display image. Exposure, bloom, tonemap and gamma in a single
// pass: each is a handful of instructions, and splitting them would cost a full-resolution
// round trip through memory per stage for no benefit at all.
float4 PSComposite(AverPostVSOut i) : SV_TARGET {
    float3 c = gPostSceneTex.SampleLevel(gPostSamp, i.uv, 0).rgb;
    // Exposure FIRST. The bloom pyramid was built from an already-exposed scene, so adding it before
    // this would expose it a second time.
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
