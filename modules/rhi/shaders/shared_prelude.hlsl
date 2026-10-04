
// The engine's per-frame block. MIRRORS PerFrameCB field for field.
cbuffer PerFrame : register(AVER_CB_JOIN(b, AVER_FRAME_CB)) {
    float4x4 gViewProj;
    // Inverse of gViewProj with translation removed (rotation only, eye at origin): maps clip
    // space to a world-space OFFSET FROM gCamPos, not an absolute position. Use averViewRayDir,
    // not by unprojecting through this and subtracting gCamPos back off.
    float4x4 gInvViewProjRel;
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
    // Fed every frame, but the raymarch that reads them lives in D3D12Device.cpp's kShaderHLSL
    // (PSky only), not here -- unlike fog/sky ambient (averApplyFog, averSkyIrradiance), nothing
    // else reads cloud data. Kept in this cbuffer (not split out) only to avoid a PerFrameCB
    // layout change across every consumer; a separable follow-up.
    float4   gCloudParams; // x coverage, y density, z layer bottom, w layer top
    float4   gCloudMotion; // xy wind offset, z 1/feature size, w enabled
    // ---- the PHYSICAL atmosphere (rhi::AtmosphereProfile) ----
    float4   gAtmoRayleigh; // rgb scattering per km, w scale height km
    float4   gAtmoMie;      // x scatter, y extinction, z scale height km, w phase g
    float4   gAtmoOzone;    // rgb absorption per km, w tent half-width km
    float4   gAtmoPlanet;   // x planet radius km, y atmosphere top radius km, z world->km, w on/off
    float4   gAtmoTune;     // x ozone centre km, y multi-scatter gain, z view steps, w aerial steps
    float4   gAtmoSunE0;    // rgb sun irradiance ABOVE the air, w ground albedo
    // averFogInscatterRef()'s answer, baked once per frame on the CPU (Atmosphere.cpp mirrors this
    // function) rather than marched per pixel -- see averFogInscatterRef below for why that's
    // exact.
    float4   gFogInscatterRef; // rgb; w unused
    // x > 0.5: WHITE FURNACE mode, every direction (and the ground) carries radiance y. z > 0.5:
    // SUN-ON variant -- environment drops to zero, only the sun is lit (the only config where the
    // direct term is tested alone). A measuring instrument, not a look -- see averFurnaceOn.
    float4   gFurnace;
    // Nine L2 SH coefficients of the sky, rgb (w unused). Baked once per frame on the CPU by
    // atmoSkyRadianceSH, same reasoning as gFogInscatterRef. Written only while the PHYSICAL
    // atmosphere is on; averSkyIrradiance reads them only under averAtmoOn() for that reason.
    float4   gSkySh[9];
    // x seconds wrapped to 3600, y seconds raw, z delta seconds. See PerFrameCB::time for why the
    // clock lives in THIS block and not in the material or feature ones, and why x is wrapped.
    float4   gTime;
    // The water wave set: xy = unit direction, z = k (rad/cm), w = angular speed (rad/s).
    // gWaveParams: x amplitude, y live count. See PerFrameCB::wave for why they live in THIS block.
    float4   gWave[3];
    float4   gWaveParams;
    // gViewProj without temporal AA's sub-pixel jitter, for what draws after the resolve
    // (PerFrameCB::viewProjNoJitter). gTaaJitter.xy: the jitter in scene pixels (+y down).
    float4x4 gViewProjNoJitter;
    float4   gTaaJitter;
};

// The unit world-space view ray through one viewport-relative NDC point (y up). gInvViewProjRel
// maps clip space to an offset from the camera, so the product's xyz IS the ray -- no eye to
// subtract, nothing large to cancel. w is positive in front of the camera and just rescales, so
// it is not divided out.
float3 averViewRayDir(float2 ndc) {
    return normalize(mul(float4(ndc, 1.0, 1.0), gInvViewProjRel).xyz);
}

// ---- water surface, evaluated in one place ------------------------------------------------
// Single source of truth for the waves: the ripple (material-graph pin defaults) and the caustics
// (HLSL) used to duplicate the same wavelengths, so retuning one alone slid the caustics off the
// ripples they light. Both now call these instead of owning their own copies.

// One wave's phase at a world XY position, in radians.
float averWavePhase(float4 w, float2 p) { return dot(w.xy, p) * w.z + gTime.x * w.w; }

// Surface height in centimetres, relative to the flat surface.
float averWaveHeight(float2 p) {
    const uint n = (uint)(gWaveParams.y + 0.5);
    float h = 0.0;
    [unroll] for (uint i = 0; i < 3; ++i)
        if (i < n) h += sin(averWavePhase(gWave[i], p));
    return h * gWaveParams.x;
}

// Tangent-space normal of the height field, ANALYTIC not finite-difference (derivative of a sum
// of sines is a sum of cosines -- exact, same cost as the height). z = 1 keeps it a tilt.
float3 averWaveNormal(float2 p) {
    const uint n = (uint)(gWaveParams.y + 0.5);
    float2 d = float2(0.0, 0.0);
    [unroll] for (uint i = 0; i < 3; ++i)
        if (i < n) d += gWave[i].xy * (gWave[i].z * cos(averWavePhase(gWave[i], p)));
    return normalize(float3(-d * gWaveParams.x, 1.0));
}

// How strongly the surface focuses light here: 0 spreading, 1 at tightest convergence. This is
// the negated, normalised Laplacian of the height field -- for a sum of sines the 2nd derivative
// is analytic (-k^2 sin(phase)), which is why caustics need no ray tracing, photon map or texture.
float averWaveFocus(float2 p) {
    const uint n = (uint)(gWaveParams.y + 0.5);
    float lap = 0.0, norm = 0.0;
    [unroll] for (uint i = 0; i < 3; ++i) {
        if (i >= n) continue;
        const float k2 = gWave[i].z * gWave[i].z;
        lap  -= k2 * sin(averWavePhase(gWave[i], p));
        norm += k2;
    }
    return saturate(lap / max(norm, 1e-6));
}
// The per-draw block: transform plus shading constants. 32 dwords, matching kObjectConstantDwords.
cbuffer PerObject : register(AVER_CB_JOIN(b, AVER_OBJECT_CB)) {
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

// Whether the shading model is in furnace mode, and the radiance every direction then carries.
//
// A WHITE FURNACE is the standard energy oracle: a surface of albedo 1 in a uniform environment of
// radiance L must read back exactly L, from every direction and orientation, REGARDLESS OF
// GEOMETRY (an enclosure of other surfaces still reads L, since it is emitting L too).
// Missing/double-counted factors show as a ratio: drop 1/PI -> PI*L, drop the cosine -> 2L, an
// occlusion term that forgets the occluder emits too -> darker.
bool  averFurnaceOn() { return gFurnace.x > 0.5; }
// Declared here, not beside averSunRadiance, because averFurnaceL calls it and HLSL has no forward
// declarations -- use-before-declare fails at DXC runtime, not compile time, even on a build that
// reported success.
bool  averFurnaceSun() { return gFurnace.z > 0.5; }
// Zero in the sun-on variant: an environment carrying radiance would add an ambient term and
// the direct term would no longer be alone in the answer.
float averFurnaceL()  { return averFurnaceSun() ? 0.0 : gFurnace.y; }

// The sun's radiance: authored colour (decoded) times authored intensity. OFF in the plain
// furnace -- a directional source isn't part of a uniform environment and would swamp what's
// being measured; in the SUN-ON variant it's the only thing lit.
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

        // Two attempts to rebalance this term's chromaticity here were reverted: averLuminance(sunT)
        // broke AtmosphereTest's fog-reference sweep, sunT.r broke the dome-fit sunrise sweep. Both
        // were directionally correct (gAtmoTune.y is an isotropic multi-scatter stand-in; reusing
        // sunT here double-counts airmass reddening) but this integral is shared by sky/fog/aerial/
        // every atmoOn surface, each with its own smoothness contract. See averSkyPhysical, which
        // applies the idea locally to the sky alone instead.
        float3 source = ((scatR * pR + scatM * pM) +
                         (scatR + scatM) * gAtmoTune.y / (4.0 * PI)) * sunT * gAtmoSunE0.rgb;
        float3 stepT = exp(-ext * dt);
        total += transmittance * source * (1.0 - stepT) / max(ext, 1e-12);
        transmittance *= stepT;
    }
    return total;
}

// The camera's altitude in kilometres, floored at a metre (not zero): at z=0 exactly, the
// ray-sphere near root is t=0 for every downward direction, so the ground-hit test never fires
// and the march runs the full atmosphere-shell length straight down through the planet at
// sea-level density the whole way -- the bright speckled field seen once a character could fall
// past z=0 (only reachable once the implicit ground came out). A metre of standoff is free above
// ground and keeps the intersection well-posed below it.
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

// The reference sky colour that fog and the dome's lower half both resolve to.
//
// Two terms, computed ONCE PER FRAME on the CPU: straight up (ambient half) blended with straight
// out along the sun's azimuth, levelled to the horizon (what colour light hits the fog right now).
// Real implementation is Atmosphere.cpp's atmoFogInscatterRef, mirrored from this reasoning;
// gFogInscatterRef is its answer for this frame.
//
// The zenith term alone isn't enough: aerial-perspective transmittance (bac1d87's original gate)
// needs tens of km to move, while authored FOG density is tuned to read within a few hundred
// metres to a couple of km -- wherever fog shows, that gate sits at zero, and a zenith march is
// nearly blind to the sun's azimuth besides. Fixed with a horizontal ray toward the sun's own
// azimuth instead: the path through air is longest there, so it reddens as the sun drops,
// independent of the fog's distance from the camera -- cosVS reduces to length(L.xy) (sin of the
// sun's elevation) for free. The blend weight is a tuned constant standing in for multiple
// scattering, which single-scatter can't simulate.
//
// SAFE TO HOIST, NOT AN APPROXIMATION: the whole input is sun direction + atmosphere profile +
// camera altitude, fixed for the frame before any pixel shades. Was two full 32-step marches PER
// CALL, up to 3x per pixel (averFogInscatter once, averSkyPhysical's ground veil twice more) --
// baking it once on the CPU is the same march, run once instead of thrice-per-pixel, matching
// gSkyZenith/gSkyHorizon's own CPU-baked pattern for the authored dome.
float3 averFogInscatterRef() {
    return gFogInscatterRef.rgb;
}

// What the ground below the horizon radiates: albedo times the sun and sky falling on it.
float3 averGroundRadiance() {
    // Part of the environment, so in a furnace it carries L like everything else -- leaving it
    // physical would darken the lower hemisphere and fail every downward-facing probe, for
    // reasons unrelated to the shading model.
    if (averFurnaceOn()) return averFurnaceL();
    float3 albedo = srgbToLin(gGroundColor.rgb);
    float  ndl    = saturate(normalize(gLightDir.xyz).z);
    float3 E = averSunRadiance() * ndl + PI * averSkyAbove(float3(0, 0, 0.5)) * gAmbient.r;
    return albedo * E / PI;
}

// The ground as the PHYSICAL model should see it: same albedo, lit by a sun that actually
// travelled through the atmosphere. The only difference from averGroundRadiance is sunT, and it
// is the whole point -- that function uses averSunRadiance() at full strength, so a neutral 0.24
// albedo under a 1.0/0.98/0.92 sun at 100k lux reads warm cream at every sun elevation, including
// sunset (where the real ground should be red-brown and much dimmer; an earlier attempt to blend
// toward it was reported back as "brown" for exactly this reason -- the blend wasn't the mistake,
// the sun feeding it was). Passing the sky march's own transmittance here makes the ground track
// the sun it's actually lit by, dim and red at sunset, without dragging cream across the bottom of
// the frame.
float3 averGroundRadianceLit(float3 sunT) {
    if (averFurnaceOn()) return averFurnaceL();
    float3 albedo = srgbToLin(gGroundColor.rgb);
    float  ndl    = saturate(normalize(gLightDir.xyz).z);
    float3 E = averSunRadiance() * sunT * ndl + PI * averSkyAbove(float3(0, 0, 0.5)) * gAmbient.r;
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
// The ray is NOT clamped to the upper hemisphere (no cosV = max(dir.z,0)): that treated every
// downward direction as pointing exactly at the horizon -- the longest, haziest path -- painting
// the whole lower hemisphere one flat slab, crossfaded to averGroundRadiance() (no air in front
// of it) over a fixed ~20-degree band. Together: a bright horizon band and warm brown below.
//
// (This comment once claimed the ray is marched to the planet with the ground composited in
// front of it; it has not been since the horizon-seam fix below replaced that -- a stale comment
// describing the opposite of what the code does, a defect this file treats as a bug, not
// untidiness.)
//
// Pinning cosV to 0 alone, with no ground term, makes the whole lower hemisphere read the
// horizon-grazing colour -- the BRIGHTEST direction the model can return, being the longest path
// through air toward the sun -- which was reported as the void far too bright: a sunset straight
// down read 220,157,118.
//
// So cosV is pinned to 0 below the horizon (both sides then evaluate the identical integral at
// dir.z=0, so the seam closes exactly at every sun elevation) AND a GROUND TERM is composited over
// the pinned march, weighted by depth below the horizon (zero at the horizon; see
// averGroundRadianceLit for why this doesn't bring back the cream that sank the earlier attempt).
// The phase term still uses the TRUE direction (dot(dir,L)), so azimuth and the sun's glow
// survive -- only the distance profile is pinned. Above the horizon this is bit-identical to
// before: the planet's near root is behind the camera, so tMax is still the shell's far root and
// cosV still equals dir.z (the old ground crossfade was zero there too) -- only dir.z < 0 changes,
// so the fog's zenith sample and every surface probe are untouched.
float3 averSkyPhysical(float3 dir) {
    // FURNACE FIRST, same as skyColor. Without this guard, enabling the physical atmosphere left
    // every surface correctly reading L against a REAL sky background instead of L -- the shading
    // was right and the picture a lie, which makes the furnace oracle unreadable by eye and the
    // background unusable as the reference plates are compared against.
    if (averFurnaceOn()) return averFurnaceL();
    float3 L   = normalize(gLightDir.xyz);
    float  alt = averAtmoCamAlt();
    float  r0  = gAtmoPlanet.x + alt;
    float  b   = r0 * dir.z;

    // Where the ray stops: the planet if it meets it, else the top of the air.
    //
    // Radius terms are FACTORED, never squared and subtracted: r0 and planet radius are both ~6360
    // km, so r0*r0 ~4.05e7 and one float32 ulp there is ~4 km^2 -- bigger than the ~25 km^2 (2 m up)
    // or ~2.5 km^2 (20 cm up) actually being asked for. (r0*r0 - R*R) is mostly rounding error
    // there, amplified by the near-root subtraction below -- the cause of speckle/concentric arcs
    // in the lower hemisphere at eye height. (r0-R)(r0+R) can't cancel: the first factor IS the
    // altitude, already known exactly.
    float cP    = alt * (r0 + gAtmoPlanet.x);
    float discP = b * b - cP;
    // Vieta's form for the near root: t_near = c / (-b + sqrt(disc)). The textbook -b - sqrt(disc)
    // subtracts two nearly-equal positives, discarding precision exactly when the ground is close.
    float tGround = (dir.z < 0.0 && discP >= 0.0) ? cP / max(-b + sqrt(discP), 1e-9) : -1.0;
    bool  hitsGround = tGround > 0.0;

    // Concretely: marched to the planet, the below-horizon integral is a genuinely SHORTER path
    // than just above it (from 200 m up, grazing down hits ground ~50 km out; grazing up runs
    // hundreds of km through air) -- that gap is the bright rim, a discontinuity in what's
    // integrated on either side of dir.z=0, not a band drawn at the horizon.
    //
    // Not the old cosV = max(dir.z,0) clamp coming back: what made THAT a flat slab was that it was
    // the ONLY value used for the entire lower hemisphere. Here the phase term still gets the true
    // direction, so only the march's distance profile is flat below the horizon, not the scene.
    float cosV  = hitsGround ? 0.0 : dir.z;
    float bV    = r0 * cosV;
    float cA    = (r0 - gAtmoPlanet.y) * (r0 + gAtmoPlanet.y);
    float discA = bV * bV - cA;
    if (discA < 0.0) return 0.0;
    float tMax  = -bV + sqrt(discA);

    float3 T;
    float3 sky = averAtmoScatter(r0, cosV, L.z, dot(dir, L), tMax, (int)gAtmoTune.z, T);

    // Restores some of the blue a fuller multi-scatter treatment would keep -- applied HERE,
    // locally, not inside averAtmoScatter (two attempts there were tried and reverted; see that
    // function's comment), so it can only ever affect the sky, never the shared integral. Real air
    // scatters blue harder than red, but by the time light has gone far enough through air to
    // matter for a grazing view, it's ALSO already lost more blue than red getting to the ground
    // (airmass reddening -- a 60-degree sun is airmass ~1.16, enough to knock blue transmittance to
    // ~0.7 against red's ~0.95). Single-scatter can't recover that the way real multi-bounce light
    // does, which is what read as a flat cream horizon, not blue-grey.
    //
    // groundSunT.r - groundSunT: how far each channel trails red's own survival here (zero near
    // noon, largest toward sunrise/sunset). haze is SQUARED, not linear: saturate(1-luminance(T))
    // hits its 1.0 ceiling well before the view is horizontal, so a linear haze applied near-full
    // strength across a wide band, reading brighter than the sky just above it with a hard edge
    // where it capped (reported back as "too bright, make it consistent with the sky"). Squaring
    // keeps it small until genuinely grazing, so the band gains depth (a gradient toward blue)
    // instead of a flat wash with a seam.
    float3 groundSunT = averAtmoSunTransmittance(max(alt, 0.0), L.z);
    float3 shortfall  = max(groundSunT.r - groundSunT, 0.0);
    float  haze       = saturate(1.0 - averLuminance(T));
    sky += averLuminance(sky) * shortfall * (haze * haze) * gAtmoTune.y * 1.5;

    // Below the horizon you see the SKY, veiled by whatever air is in the way -- not a lit ground
    // disc, and not averGroundRadiance (same cream problem as averGroundRadianceLit's comment: a
    // full-strength sun on 0.24 albedo reads warm cream regardless of blend weight -- an earlier
    // version of this fix blended toward it anyway, to avoid a flat lower hemisphere, and brought
    // the cream straight back -- and the ground you actually see in a game is the level's own
    // geometry anyway). The GROUND is instead composited OVER the pinned march, via
    // averGroundRadianceLit (a sun that's actually been through the air), because the haze in
    // front of distant ground under the horizon is real.
    //
    // Weighted by depth below the horizon and ZERO at it (smoothstep(0, 0.45, -dir.z)), so this
    // can't reopen the seam the cosV pinning closes -- above the horizon bit-identical to before.
    // Full weight by ~27 degrees down, where there's little air left to look through anyway.
    // gGroundColor.a is the authored "how much of the ground replaces the sky" and is respected:
    // a project that wants the old flat haze sets it to 0.
    const float below = smoothstep(0.0, 0.45, saturate(-dir.z)) * gGroundColor.a;
    if (below > 0.0) sky = lerp(sky, averGroundRadianceLit(groundSunT), below);
    return sky;
}

// Below averSkyPhysical (not beside skyColorFull) for the same HLSL forward-declaration reason as
// averFurnaceSun above: it calls averSkyPhysical, and a use-before-declare fails at DXC runtime.
float3 skyColor(float3 dir){
    if (averFurnaceOn()) return averFurnaceL();
    // Until this fix, every reflection fallback (glass, water, metal, RT miss) used averSkyAbove --
    // elevation only, no azimuth -- while SkyModel::Physical (RHI.hpp's default) draws an
    // azimuth-aware, sun-graded dome on screen. A near-mirror reflected a smooth vertical gradient
    // while the sky behind it had structure, which is why authored glass read as flat tinted
    // plastic regardless of Fresnel: a real, undimmed reflection -- of nothing. Diagnosed from a
    // PTTest capture, not the code (the panes were uniform sheets under a visibly graded sky).
    // Gated on averAtmoOn(), same switch averFogInscatter branches on below, so an authored sky's
    // reflections are unaffected.
    //
    // NOT FREE: averSkyPhysical measured 85% of the scene pass / 41% of the frame before the fog
    // path learned to defer it (see averFogInscatter below) -- no equivalent threshold here, since
    // a reflection either needs the sky or doesn't. Measure before assuming this is small; if not,
    // the fix is a cheaper directional approximation, not a mismatched gradient.
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
// RAY once enough real atmosphere lies between camera and wpos to be the honest answer.
//
// Under a PHYSICAL sky this blends averFogInscatterRef() (zenith) toward averSkyPhysical(dir) (the
// true view ray) by `t`, the aerial-perspective transmittance averApplyFog already computed
// getting here (passed in, not recomputed -- same march). `t` already IS "how much atmosphere is
// in the way" (exp(-opticalDepth)), continuous by construction: near the camera t~1 and this
// returns the old always-zenith answer; only once real atmosphere separates camera and point does
// the true ray's own colour fade in -- "slowly visible at large scale" for free, off a quantity
// already being computed. Also already shaped by the planet's actual radius (t comes from
// averAtmoScatter's altitude-dependent density, r0 = planet radius + camera altitude) -- a smaller
// planet reveals the grazing sky sooner, an Earth-scale one gently, no curvature term added.
//
// Under an AUTHORED sky the level's FOG colour tints skyColorFull's gradient along the view ray; a
// PHYSICAL sky now does the same job, which it didn't used to (RHI.hpp calls fogColor "a TINT on
// the in-scattered sky, not a replacement" -- the gap behind "make the fog properly blue, like
// UE's"). gFogColor defaults to white, so this multiply is a no-op for scenes that don't ask for
// it; SkyForge's own `FOG color 0.7 0.78 0.88` now finally reaches the physical fog it was always
// written to tint.
float3 averFogInscatter(float3 wpos, float3 t) {
    float3 dir = normalize(wpos - gCamPos.xyz);
    if (averAtmoOn()) {
        float  w   = saturate(1.0 - averLuminance(t));
        // Branched for the same reason averApplyFog branches around this whole function, one level
        // further down and far more expensive: HLSL evaluates both sides of a lerp, so every fogged
        // pixel was paying for a full averSkyPhysical march and then multiplying by w -- and w is
        // ZERO wherever fog is actually visible (aerial transmittance needs tens of km to move; fog
        // reads within a few hundred metres to a couple of km -- see averFogInscatterRef above).
        //
        // MEASURED, ElectricDreams 2750x1639, 4x MSAA, rt/gi off: scene draw 8.9ms -> 1.3ms (85% of
        // the scene pass, 41% of the frame; cascade PCF/environment/BRDF each ~0.0ms beside it).
        //
        // Deferred, not removed: once real air stands between camera and surface, w lifts off zero
        // and the ray's sky genuinely stops matching the reference. The skipped value differs from
        // the true one by at most 0.2% of the gap -- under a bit at 8-bit output, no visible seam.
        float3 physical = averFogInscatterRef();
        // 0.01 measured, not picked: sweeping the threshold, 0.01-0.9 all give the same 1.9ms,
        // 0.002 gives 8.9ms -- w for effectively every pixel here lands between those two. At 0.01
        // the skipped term is at most 1% of the gap between reference and marched sky, a fraction
        // of one 8-bit step, so no pixel can band crossing the threshold.
        if (w > 0.01) physical = lerp(physical, averSkyPhysical(dir), w);
        return physical * srgbToLin(gFogColor.rgb);
    }
    return skyColorFull(dir) * srgbToLin(gFogColor.rgb);
}

// Applies the air between the camera and a surface: physical atmosphere, then height fog.
//
// `aerial`: lets the aerial march's own cost be measured/gated independently of height fog's
// (below), which was already branched -- no ablation could isolate the march alone before this
// split. averApplyFog keeps its signature and behaviour; no caller changes.
//
// `airVis` [0,1]: how much of the upper sky-hemisphere the air at wpos can see (voxi.hlsl's air
// sky-visibility volume, gAirVis/voxiAirVisibility). Every caller but voxi.hlsl's own
// occlusion-aware sites passes 1.0 (`x * 1.0` folds away -> byte-identical to before this param
// existed). Only the IN-SCATTER terms are scaled; EXTINCTION (`color * T`, and the lerp's implicit
// `1-fogF` weight) is not -- light lost to air en route is true regardless of occlusion; only the
// light air adds BACK by seeing sky is blockable.
float3 averApplyFogAirVis(float3 color, float3 wpos, bool aerial, float airVis) {
    float3 T = 1.0;
    // No magnitude gate here, unlike the two below -- measured, not an oversight. Looks like the
    // same near-zero-result bug fixed twice elsewhere, but ablation (AVER_RD_ABL_AERIAL, voxi.hlsl)
    // says leave it alone. Sponza 2750x1639, ray-driven primary:
    //     baseline                       16.83 ms
    //     aerial march ablated (mode 12) 16.64 ms   -> this term costs 0.19 ms, 1.1% of the pass
    //     all of averApplyFog  (mode 11) 16.61 ms   -> height fog adds 0.03 ms on top
    // Unlike the 41%-of-frame case elsewhere in this file, the work here is USED (not multiplied by
    // a weight that's zero where fog is visible): ablating it changes 28.6% of the frame by up to
    // 37 codes, so a threshold would trade visible haze for at most 0.2ms. Cheap despite the loop
    // because the pass is ray-traversal bound, not ALU bound -- a 4-step march with analytic inner
    // transmittance hides behind memory latency the rays already pay for.
    if (aerial && averAtmoOn()) {
        float3 inscatter = averAtmoAerial(wpos, T);
        color = color * T + inscatter * airVis;
    }

    // Branched, not lerped unconditionally: HLSL evaluates both sides of a lerp eagerly, so every
    // pixel nearer than fog's own start distance (averFogFactor's early `len <= start` return,
    // RHIShaders.cpp) used to pay for averFogInscatter anyway -- its dir normalize, and under a
    // physical sky a full averSkyPhysical march -- for a contribution multiplied by zero.
    // averFogFactor is smooth and spatially coherent, so GPU-wave divergence should stay confined
    // to fog's own boundary, not scatter across the image.
    float fogF = averFogFactor(wpos);
    if (fogF > 0.001) color = lerp(color, averFogInscatter(wpos, T) * airVis, fogF);
    return color;
}

// THE SAME AIR AS TWO TERMS, for a caller that must treat a colour's parts differently (voxi.hlsl's blended
// panes: the pane's own light is extinguished, the in-scatter weighted by coverage, the backdrop's share
// left alone). averApplyFogAirVis is affine in its colour -- color*T + inscatter, then a lerp toward the fog
// in-scatter -- so it is exactly averApplyFogAirVis(c) == c * extinction + inscatter, from ONE evaluation
// instead of the two that fog(c) and fog(0) would cost.
void averFogTermsAirVis(float3 wpos, bool aerial, float airVis, out float3 extinction, out float3 inscatter) {
    float3 T = 1.0;
    extinction = 1.0;
    inscatter  = 0.0;
    if (aerial && averAtmoOn()) {
        const float3 aerialIn = averAtmoAerial(wpos, T);
        extinction = T;
        inscatter  = aerialIn * airVis;
    }
    const float fogF = averFogFactor(wpos);
    if (fogF > 0.001) {
        extinction *= 1.0 - fogF;
        inscatter = lerp(inscatter, averFogInscatter(wpos, T) * airVis, fogF);
    }
}

// Byte-identical wrappers: airVis == 1.0 is today's unoccluded air, so every caller but voxi.hlsl's
// own occlusion-aware call sites (PSMainVoxi/PSRayDriven, near their own gAirVis/voxiAirVisibility
// use) compiles to exactly the code it did before averApplyFogAirVis existed.
float3 averApplyFogEx(float3 color, float3 wpos, bool aerial) { return averApplyFogAirVis(color, wpos, aerial, 1.0); }
float3 averApplyFog(float3 color, float3 wpos) { return averApplyFogAirVis(color, wpos, true, 1.0); }


// Transforms a NORMAL by a world matrix. Not the same operation as transforming a direction.
//
// A normal needs the inverse transpose of the upper 3x3, not the matrix itself -- `mul(float4(n,0),
// w)` only agrees with it under rotation + UNIFORM scale (and for an axis-aligned box under any
// scale, since box normals lie along the principal axes and a diagonal scale only lengthens them).
// Wrong for any normal with components on more than one axis once scale is non-uniform: reads as a
// lighting bug, not a transform one.
//
// Uses the COFACTOR matrix, not a real inverse: cofactor(M) == det(M)*inverse(M)^T, and the caller
// normalises so the det factor divides back out -- no determinant, no division, no singular case.
// A mirrored transform (det<0) flips the normal, correctly.
//
// Row-vector convention: t' = t*M, so n' = n*(M^-1)^T, and (M^-1)^T is cofactor(M) up to scale.
float3 averTransformNormal(float3 n, float4x4 w) {
    float3 c0 = float3(w[0][0], w[0][1], w[0][2]);
    float3 c1 = float3(w[1][0], w[1][1], w[1][2]);
    float3 c2 = float3(w[2][0], w[2][1], w[2][2]);
    // Rows of the cofactor matrix are the cross products of the other two rows.
    float3x3 cof = float3x3(cross(c1, c2), cross(c2, c0), cross(c0, c1));
    return mul(n, cof);
}

// The sky as irradiance from the nine SH coefficients, DIVIDED BY PI so the result is a mean
// radiance (the unit averSkyIrradiance's callers expect: E = sunIrradiance*ndl + PI*skyRadiance*
// ambient -- the sky term there is a radiance, so the PI cancels). The three constants are the PI,
// 2PI/3, PI/4 cosine-convolution factors with the basis constants folded in and PI divided back out.
//
// SELF-CHECK: a UNIFORM sky of radiance L projects to c[0] = L*0.282095*4PI and nothing else, and
// this returns exactly L -- what keeps the white furnace reading 1.000 through this path.
float3 averShIrradiance(float3 n) {
    float3 e = gSkySh[0].rgb * 0.282095;
    e += (gSkySh[1].rgb * n.y + gSkySh[2].rgb * n.z + gSkySh[3].rgb * n.x) * 0.325735;
    e += (gSkySh[4].rgb * (n.x * n.y) + gSkySh[5].rgb * (n.y * n.z) +
          gSkySh[7].rgb * (n.x * n.z)) * 0.273137;
    e += gSkySh[6].rgb * ((3.0 * n.z * n.z - 1.0) * 0.078848);
    e += gSkySh[8].rgb * ((n.x * n.x - n.y * n.y) * 0.136569);
    // L2 can undershoot on a sky with a strong, small bright region; ambient light is never
    // negative, so clamp rather than subtract light from whatever this is added to.
    return max(e, 0.0);
}

// The sky as RADIANCE ALONG ONE DIRECTION, from the same nine coefficients averShIrradiance reads.
// The difference is the cosine convolution: averShIrradiance answers "light arriving at a
// surface", folding in cosine-lobe factors A0=PI, A1=2PI/3, A2=PI/4 (then dividing PI back out);
// this answers "how bright looking along d", the raw basis -- each constant here is that
// function's constant with its A-factor divided out:
//     0.282095 = 0.282095 / 1        (A0/PI = 1)
//     0.488603 = 0.325735 / (2/3)    (A1/PI = 2/3)
//     1.092548 = 0.273137 / (1/4)    (A2/PI = 1/4)
//     0.315392 = 0.078848 / (1/4)
//     0.546274 = 0.136569 / (1/4)
// Using the wrong one isn't a small error: the convolved version is a nearly-flat hemispherical
// average, so a per-direction caller gets a blurred constant and never sees it's wrong. Nine
// coefficients also can't represent the sun disc or a cloud edge -- fine for an AMBIENT gather,
// wrong for a mirror reflection (which still marches the atmosphere); the sun is its own direct
// term elsewhere, not missing here, only absent.
//
// SELF-CHECK, matching averShIrradiance's: a uniform sky of radiance L projects to c[0] =
// L*0.282095*4PI and nothing else, and this returns exactly L in every direction.
float3 averShRadiance(float3 d) {
    float3 e = gSkySh[0].rgb * 0.282095;
    e += (gSkySh[1].rgb * d.y + gSkySh[2].rgb * d.z + gSkySh[3].rgb * d.x) * 0.488603;
    e += (gSkySh[4].rgb * (d.x * d.y) + gSkySh[5].rgb * (d.y * d.z) +
          gSkySh[7].rgb * (d.x * d.z)) * 1.092548;
    e += gSkySh[6].rgb * ((3.0 * d.z * d.z - 1.0) * 0.315392);
    e += gSkySh[8].rgb * ((d.x * d.x - d.y * d.y) * 0.546274);
    // Same clamp and reason as averShIrradiance: L2 undershoots near a strong small bright region,
    // and negative radiance isn't a dim colour -- acesTonemap floors negative/NaN at zero
    // (color.hlsli, since ded8784a), so an unclamped negative here renders as confident BLACK, not
    // the BRIGHT this comment used to warn of under acesTonemap's pre-ded8784a behaviour -- that
    // broken-value-reads-as-plausible failure mode is already recorded in this tree, just dark now.
    return max(e, 0.0);
}

// The per-direction sky a ray should use on miss: averShRadiance under the physical atmosphere,
// the authored dome's own evaluation otherwise -- mirrors averSkyIrradiance's split so an
// authored-sky project isn't handed a reconstruction of a sky it doesn't use. NOT skyColor(): that's
// a 32-step march, far too expensive per hemisphere sample; this trades away high-frequency detail
// an ambient term can't see anyway.
float3 averSkyRadianceCheap(float3 d) {
    if (averFurnaceOn()) return averFurnaceL();
    if (averAtmoOn()) return averShRadiance(d);
    return skyColorFull(d);
}

// The sky as light: the cosine-weighted average radiance over the hemisphere about N.
float3 averSkyIrradiance(float3 N) {
    if (averFurnaceOn()) return averFurnaceL();
    // The physical branch varies with azimuth (nine SH coefficients, ~20 ALU) where the authored
    // dome below does not: everything below this line reads only N.z, so a wall facing the sun and
    // one facing away used to get identical ambient light. The authored path is kept deliberately
    // for non-physical projects -- their dome IS azimuthally symmetric, so SH would only cost more
    // to say the same.
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
    // TWO-SIDED SHADING: a leaf/frond/grass blade is one sheet drawn with culling off, so half of
    // what you see is the BACK of a surface whose normal points away -- unflipped, N.L<0 clamps to
    // black and every plant rendered as a dark silhouette. Nothing anywhere else handled it either:
    // SV_IsFrontFace does not appear in a single shader in this tree. Flips on dot(N,V) rather than
    // SV_IsFrontFace deliberately: the face flag needs a new PS input threaded through every entry
    // point here and the mesh-shader variants beside it, while this is one line and answers the
    // same question. On a correctly-wound closed mesh, back faces are culled before reaching here.
    if (dot(N, V) < 0.0) N = -N;
    float3 L = normalize(gLightDir.xyz);
    float3 H = normalize(V + L);
    float metallic = saturate(gMaterial.x);
    float rough = clamp(gMaterial.y, 0.045, 1.0);

    if (gMaterial.z > 0.5) {
        // UNLIT: hand back the authored colour, unshaded -- used by editor chrome (the selection
        // outline) where colour is a signal, not a surface, so sun, sky ambient, specular and fog
        // all have nothing to say about it.
        //
        // srgbToLin IS REQUIRED: every other exit converts with srgbToLin (see `albedo` below)
        // because this shader writes LINEAR radiance that the tonemap/sRGB-encode downstream
        // expects. Skipping it once measurably broke this: selection orange (1.0, 0.62, 0.12) came
        // out YELLOW, because ACES compresses the saturated red channel harder than green and the
        // ratio between them collapsed.
        return float4(srgbToLin(gBaseColor.rgb), gBaseColor.a);
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
cbuffer MeshCB : register(AVER_CB_JOIN(b, AVER_MESH_GEOM_CB)) { uint gTriCount; uint3 _msPad; };


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
// One AS thread per cluster (kClusterAmplificationGroupSize/group), running the CPU reference's
// local cut test UNCHANGED -- aver::trifactor::inLocalCut/coneCull/Frustum::intersectsSphere
// (modules/trifactor/src/ClusterAdapt.cpp, ClusterSelect.cpp): same comparisons, sentinels, cone
// sign convention -- then DispatchMesh's the survivors. Reuses AVER_MS_VTX_REG/AVER_MS_IDX_REG from
// rhi::meshGeometryDefines (never compiled alongside AVER_MS's own block, so no collision) rather
// than a second -D convention for the same "vertices/indices sit past this layout's own SRV table"
// idea.
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
// AVER_MS_IDX_REG is reserved by the backend's mesh-geometry convention (declaredSrvCount+1) and
// dispatchMeshClusters binds a real address there (this mesh's own index buffer) so the root
// signature's SRV param is never left unset -- but nothing in THIS shader declares a resource
// there: the cluster path only uses gClusterVerts/gClusterTris above. A root signature may
// over-provision what a shader reads; it may not under-provide it, and this shader reads nothing
// there at all.

// How many clusters this dispatch covers -- dispatchMeshClusters' clusterCount argument.
cbuffer ClusterCountCB : register(AVER_CB_JOIN(b, AVER_MESH_GEOM_CB)) { uint gClusterCount; uint3 _mscPad; };

// Per-instance data the amplification shader needs beyond gWorld/gViewProj/gCamPos (already
// reachable via PerObject/PerFrame, b1/b0, since every root param is ALL-stage visible): pixel
// budget (already clamped above zero by the CPU caller, kMinClusterBudgetPx), projection scale
// (aver::trifactor::projScale), a uniform-scale approximation for cluster bounds (same one
// SandboxApp's CPU path uses transforming a cluster sphere by an instance's world matrix), and the
// 6 world-space frustum planes -- PRECOMPUTED ON THE CPU by the same aver::trifactor::
// Frustum::fromViewProj this design reuses, so the GPU test runs against the exact same six numbers
// the CPU reference tested against, not a formula that merely agrees.
cbuffer ClusterFrameCB : register(AVER_CB_JOIN(b, AVER_FEATURE_FRAME_CB)) {
    float  gBudgetPx;
    float  gProjScale;
    float  gWorldScale;
    uint   _cfcPad;
    float4 gFrustumPlane[6];
};

#define AVER_MSC_MAX_VERTS 64
#define AVER_MSC_MAX_TRIS 124

// One AS group's survivors, handed to the mesh-shader groups it spawns.
struct ClusterPayload { uint clusterId[AVER_MSC_GROUP]; };
groupshared ClusterPayload gsPayload;
groupshared uint gsSurvivorCount;

// THE LOD cut, PORTED UNCHANGED from aver::trifactor::inLocalCut (ClusterAdapt.cpp): same nearest-
// point-of-sphere distance (floored at 0), same 1e-3 near-camera sentinel (1e30, finite, matching
// the CPU's own comment on why not IEEE inf), same strict-less on own error, >= on parent error.
// `budgetPx` arrives already clamped by the CPU caller -- this does not re-clamp, mirroring
// inLocalCut's body.
bool clusterInCut(ClusterBounds c, float3 centerWS, float radiusWS, float3 eye, float budgetPx, float projScale) {
    float d = max(length(centerWS - eye) - radiusWS, 0.0);
    float ownPx = (d <= 1e-3) ? 1e30 : c.ownErrorCm * projScale / d;
    if (ownPx >= budgetPx) return false;
    float parentPx = (d <= 1e-3) ? 1e30 : c.parentErrorCm * projScale / d;
    return parentPx >= budgetPx;
}

// Cone backface test, PORTED UNCHANGED from aver::trifactor::coneCull (ClusterSelect.cpp): cull iff
// dot(normalize(apex-eye), axis) >= cutoff -- ">= cull", not "<=", per that file's measured-not-
// assumed sign convention. `axisWS` already folds in the CPU path's getSafeNormal() (zero-length
// axis -> never cull).
//
// cutoff <= -1.0 is the OTHER "never cull" sentinel, byte-for-byte with coneCull's own check (see
// that function's comment for the full account) -- already-cooked assets on disk still carry
// ClusterBuilder.cpp's pre-fix -127/-1.0f encoding; this repairs them at read time with no re-cook,
// without touching the formula for any real cutoff value.
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
            // Plain direction transform (not averTransformNormal's cofactor normal transform):
            // coneAxis is a direction along the surface, matching the CPU path's xformVec.
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

// One mesh-shader group per surviving cluster: expands that cluster's local vertex/triangle block,
// unindexed, within the 64-vertex/124-triangle caps the format guarantees (ClusterBuilder.hpp's
// kMaxClusterVertices/kMaxClusterTriangles) -- wrong SetMeshOutputCounts here drops triangles
// silently, so it's called with the desc's own counts, verbatim, before any write.
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

// Shades a cluster the SAME way the IA/whole-mesh path does (D3D12Device.cpp's PSMainPlain calls
// the identical plainShadeSurface(i, 1.0, float3(0,0,0), 1.0)) -- indistinguishable from drawMesh()
// by shading, only by which triangles exist. Separate definition, not a rename: PSMainPlain lives
// in D3D12Device.cpp's backend-internal kShaderHLSL, unreachable from a feature module's shader
// source (sceneShaderSource() is never exposed outside that file).
//
// PSClusterMain HAS MOVED, to sandbox/src/ClusterMaterialShader.hpp -- the fix. It used to be one
// line here (the same plainShadeSurface call), which never samples a material texture, so every
// textured surface on this path drew as a flat dark silhouette. It could not do otherwise from
// inside this prelude: rhi::sharedShaderPrelude() compiles BEFORE pbr::materialShaderPrelude(), so
// averEvalMaterial doesn't exist yet here. ASMain/MSClusterMain stay -- they touch no material
// state, and MSClusterMain already fills VSOut::uv, exactly what the lit shader needed.
#endif // AVER_MS_CLUSTER
