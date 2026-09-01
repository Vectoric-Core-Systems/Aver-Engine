// The engine's own scene, sky and line shading -- ONE copy, compiled by both backends.
//
// THIS LIVED TWICE, as kShaderHLSL in D3D12Device.cpp and kVulkanSceneHLSL in VulkanDevice.cpp. The
// Vulkan copy's comment set out the deal honestly: "kept identical so the two backends render the
// same picture. If PSky's cloud march or PSLine's tonemap ever changes on the D3D12 side, this copy
// needs the same edit; there is no way to share the string across the two modules without a third
// one neither currently depends on."
//
// BOTH BACKENDS ALREADY DEPEND ON Aver.RHI, so that third module was always there -- and now that
// shaders are deployed files rather than C++ literals, it has somewhere to put this. The discipline
// HAD held, for what it is worth: stripped of comments the two copies were 128 lines each and
// identical to the character. That is a good record and not a reason to keep asking for it.
//
// Compiled as the tail of rhi::sharedShaderPrelude(), which is where VSOut, plainShadeSurface,
// skyColor, averInverseTonemap and the PerFrame/PerObject blocks come from.

// Shades a scene surface: unshadowed, no bounce, through the prelude's FROZEN plainShadeSurface.
float4 PSMainPlain(VSOut i) : SV_TARGET { return plainShadeSurface(i, 1.0, float3(0,0,0), 1.0); }

// ================= volumetric clouds =================
// One raymarched layer, evaluated on sky pixels only, with analytic noise so it needs no SRV.
//
// LIVES HERE, NOT IN THE SHARED PRELUDE, ON PURPOSE: PSky below is the only caller anywhere in the
// engine. Fog and sky-ambient math stay in rhi::sharedShaderPrelude() because ordinary surface
// shading genuinely needs them (averApplyFog, averSkyIrradiance -- called from Voxi, PBR, every
// lit pixel shader there is); nothing outside the sky pass itself ever touches a cloud. Putting
// cloud code in the shared prelude would paste a 24-step raymarch and its noise functions into
// every module's shader source -- Voxi, PBR, UI, ActorPreview, the path tracer -- none of which
// call it, for no reason but that the fog/atmosphere code next to it in the file genuinely is
// shared. gCloudParams/gCloudMotion themselves still live in the shared PerFrame cbuffer (see
// their own comment there for why splitting the DATA out is separable follow-up work from moving
// the CODE that reads it).

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
    float cover = 1.0 - gCloudParams.x;

    // The cheapest octave, evaluated FIRST rather than last, as a conservative reject: `n` is at
    // most nLow + 0.6 (the base octave's full weight) + 0.3-or-0.15 (the detail octave's, if this
    // call even asked for one) -- every value-noise call returns in [0,1], so that is the best
    // case no matter what the other two octaves turn out to be. If even that best case cannot
    // clear `cover`, the remaining one or two noise samples this step would have bought are
    // guaranteed to leave d at 0 -- skip them. This never skips a point that would have been
    // nonzero; it only ever skips a point already proven to be empty.
    float nLow = averValueNoise(p * 0.41) * 0.25;
    const float bestCase = nLow + (detail ? 0.9 : 0.75);
    if (bestCase <= cover) return 0.0;

    float n = averValueNoise(p) * 0.6 + nLow;
    if (detail) n += averValueNoise(p * 3.17) * 0.3;
    else        n += 0.15;

    float d = saturate((n - cover) / max(1.0 - cover, 1e-3));
    return d * shape;
}

// Henyey-Greenstein phase function.
float averHG(float ct, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * ct, 1e-4), 1.5));
}

// Marches the layer and returns scattered radiance in rgb, TRANSMITTANCE in a. `outDist` comes back
// as the distance the scattering actually happened at, weighted by how much each step contributed,
// so the caller can put the right amount of air in front of it.
float4 averCloudLayer(float3 ro, float3 rd, float3 sunDir, float3 sunColour, out float outDist) {
    outDist = 0.0;
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
    // Was 24: cut a third, relying on the jitter below (already there, already hiding banding as
    // noise instead of visible steps) to absorb the coarser sampling rather than adding anything
    // new to hide it.
    const int kSteps = 16;
    float featureSize = 1.0 / max(gCloudMotion.z, 1e-9);
    t1 = min(t1, t0 + kSteps * featureSize * 0.35);
    if (t1 <= t0) return float4(0, 0, 0, 1);

    float dt = (t1 - t0) / kSteps;
    float jitter = averHash13(rd * 811.7);
    float t = t0 + dt * jitter;

    float sigma = averCloudSigma();
    float3 scattered = 0.0;
    float transmittance = 1.0;
    // WAS 0.62. That still left the sky broadly white across a whole quadrant, not just around the
    // sun disk: with the sun at 59.5 degrees elevation, a near-zenith view keeps a wide swath of
    // view rays within a moderate angle of sunDir, and HG at g=0.62 stays meaningfully elevated out
    // past 45 degrees off-forward (phase(60 deg) is still ~0.07 against an isotropic average of
    // ~0.08 -- barely fallen at all). Rebalancing the sun/ambient weights below moved the pixel I
    // measured but could not fix that, because the problem was never how MUCH direct light there
    // was at any one point, it was how WIDE an area was getting a meaningful dose of it. 0.85 is a
    // realistic Mie asymmetry for actual water droplets (0.62 was already below the usual 0.75-0.85
    // range) and its narrower lobe is what a narrower dose looks like: phase(60 deg) drops to ~0.03,
    // under half of 0.62's, while the peak at dead-forward rises -- the glow right around the sun
    // gets brighter and tighter instead of smearing across the whole sky.
    float phase = averHG(dot(rd, sunDir), 0.85);
    float distWeight = 0.0;
    // THE SAME BRANCH PSky ITSELF TAKES A FEW LINES DOWN -- averSkyPhysical UNDER A PHYSICAL SKY,
    // skyColorFull ONLY as the authored fallback -- not skyColorFull unconditionally the way this
    // read before. skyColorFull is a two-colour gradient an artist set by hand; under `SKY model
    // physical` the real answer for "what colour is the sky overhead" is the Rayleigh/Mie/ozone
    // march every other physical surface in this file already asks for, and clouds lighting
    // themselves off the authored gradient instead was exactly the brute-forced colour this cloud
    // pass was supposed to have stopped doing. It reads as a flat, pale grey-white deck instead of
    // blue precisely because the one input that WAS carrying blue -- real Rayleigh scattering, which
    // is blue for a physical reason (short wavelengths scatter harder) -- was never being asked for.
    float3 ambientTop = averAtmoOn() ? averSkyPhysical(float3(0, 0, 1)) : skyColorFull(float3(0, 0, 1));

    [loop] for (int i = 0; i < kSteps; ++i) {
        if (transmittance < 0.02) break;
        float3 p = ro + rd * t;
        float d = averCloudDensity(p, true);
        if (d > 0.001) {
            float lt = 0.0;
            // Was 3 steps at 0.25*(top-bottom): samples at 0.5/1.5/2.5 step-widths ahead reached
            // (2.5+0.5)*0.25 = 0.75 of the layer's thickness toward the sun. 2 steps at a wider
            // 0.375*(top-bottom) sample 0.5/1.5 step-widths ahead, reaching (1.5+0.5)*0.375 = 0.75
            // -- the same total reach, one fewer (and this loop's most expensive) sample.
            float lstep = (top - bottom) * 0.375;
            [unroll] for (int j = 0; j < 2; ++j) {
                float3 lp = p + sunDir * (lstep * (j + 0.5));
                lt += averCloudDensity(lp, false) * lstep;
            }
            float sunT = exp(-lt * sigma);

            // NO POWDER TERM ON THE SUN ANY MORE, AND THAT IS WHY THIS LAYER HAS FORM AGAIN. It used
            // to be 1-exp(-2*d*dt*sigma) -- built from the density of THIS step -- and the result was
            // then multiplied by (1-stepT), which is 1-exp(-d*dt*sigma) from the same quantity. Two
            // factors, both going to zero with density, so the direct sun arrived scaled by roughly
            // density SQUARED while the ambient beside it scaled linearly. Wherever the layer was
            // thin -- which is most of what a coverage dial around a half actually draws -- the sun
            // term was squared away to nothing and the pixel was very nearly pure flat ambient. That
            // is the washed-out grey smear: not a missing effect, an extra factor. Single scatter
            // wants the one (1-stepT) below and nothing else.
            //
            // The form comes from the ambient instead, where it physically belongs. A sample near the
            // top of the layer sees most of the sky dome; one near the base sees it through every
            // metre of cloud above it, which is exactly why real cloud bottoms are grey and their
            // tops are white. Squaring the height ratio keeps the darkening in the lower half of the
            // layer rather than spreading it evenly. The old flat 0.9 of the zenith at every sample,
            // base and top alike, is what made this read as one uniform slab of fog.
            float hN = saturate((p.z - bottom) / max(top - bottom, 1.0));

            // THE 0.6 AND THE 3.0 ARE A MEASURED CORRECTION, NOT A GUESS. Isolating each term at a
            // pixel deep in the deck showed ambient alone reads a genuine sky blue (44,66,92) while
            // the full sum reads within a few codes of sun ALONE (145,144,140 vs 142,140,134) -- the
            // direct term was simply several times the ambient term's magnitude, so summing them
            // left ambient invisible whatever hue it carried, and that ratio does not move with
            // exposure: scaling both terms together by a common exposure factor cannot change their
            // RELATIVE weight, which is confirmed by testing exposures from 0.45 to 1.0 with the
            // original weights and getting the same near-neutral hue every time. What single-scatter
            // sun and single-scatter sky can never reproduce on their own is what makes a real cloud's
            // shadowed interior read blue-grey: MULTIPLE scattering, light bouncing many times through
            // the droplets before it escapes, which this pass does not simulate and which is most of
            // a real cloud's brightness. ambientTop is standing in for all of that missing bounce
            // light, not only the one sky-dome term its name suggests, so it was underweighted twice
            // over. Direct scaled down and ambient scaled up together land the shadowed underside of a
            // thick cloud as the blue-grey UE's own clouds show there, without the deck going dim and
            // moody the way scaling direct down alone did (tried at 0.3 with ambient untouched: the
            // hue was right, the whole sky read like dusk). This alone was not the whole fix -- see
            // where `phase` above is computed for the other half of it, the WIDTH of the area getting
            // any direct light at all, which this ratio cannot touch.
            float3 lit = sunColour * sunT * phase * 0.6 + ambientTop * lerp(0.12, 0.55, hN * hN) * 3.0;

            float stepT = exp(-d * dt * sigma);
            float w = transmittance * (1.0 - stepT);
            scattered += w * lit;
            outDist += w * t;
            distWeight += w;
            transmittance *= stepT;
        }
        t += dt;
    }
    outDist = distWeight > 1e-6 ? outDist / distWeight : t0;
    return float4(scattered, transmittance);
}

// Procedural sky for a fullscreen triangle. The only place the full scattering integral runs.
float4 PSky(SkyOut i) : SV_TARGET {
    float4 far = mul(float4(i.ndc, 1.0, 1.0), gInvViewProj);
    float3 ray = normalize(far.xyz / far.w - gCamPos.xyz);
    float3 L = normalize(gLightDir.xyz);
    float3 sky = averAtmoOn() ? averSkyPhysical(ray) : skyColor(ray);
    float sd = saturate(dot(ray, L));
    float3 sunC = srgbToLin(gLightColor.rgb) * gSkyParams.z;
    float cosR = gSkyParams.w;
    float disk = smoothstep(cosR - 0.0004, cosR + 0.0002, sd);
    sky += sunC * disk * 14.0;
    if (!averAtmoOn()) sky += sunC * pow(sd, 12.0) * 0.30;

    // THE DECK GETS THE AIR IN FRONT OF IT, like every other surface in the renderer. Without this
    // the cloud radiance was composited straight onto the sky at full strength however far away it
    // was, and near the horizon "far away" is tens of kilometres -- the deck stayed as crisp and as
    // bright at the horizon as it was overhead, then simply stopped where the march's distance cap
    // fell. A hard edge of full-contrast cloud sitting on a band of haze is most of what read as a
    // rim there. Veiled properly the deck loses contrast into exactly the haze the sky behind it is
    // already made of, so it recedes instead of ending, and the horizon needs no separate fade.
    float cloudDist;
    float4 cloud = averCloudLayer(gCamPos.xyz, ray, L, sunC, cloudDist);
    if (averAtmoOn() && cloud.a < 0.999) {
        float3 aerialT;
        float3 aerialIn = averAtmoAerial(gCamPos.xyz + ray * cloudDist, aerialT);
        cloud.rgb = cloud.rgb * aerialT + aerialIn * (1.0 - cloud.a);
    }
    sky = sky * cloud.a + cloud.rgb;

    return float4(sky, 1.0);
}

// Line vertex as authored: position and display colour.
struct LVSIn  { float3 pos : POSITION; float3 col : COLOR; };
// Line vertex after transform.
struct LVSOut { float4 pos : SV_POSITION; float3 col : COLOR; };
// Transforms a line vertex into clip space.
LVSOut VSLine(LVSIn i) {
    LVSOut o;
    float4 wp = mul(float4(i.pos, 1.0), gWorld);
    o.pos = mul(wp, gViewProj);
    o.col = i.col;
    return o;
}
// Writes a display-authored line colour as the scene radiance that tonemaps back to it, times the
// glow multiplier in gBaseColor.x (1.0 = exactly the colour authored; see IDevice::setLineGlow).
//
// THE MULTIPLY IS AFTER THE INVERSE TONEMAP ON PURPOSE. averInverseTonemap is near-vertical at the
// top -- it clamps at 1.0329, where its 2.43y - 2.51 denominator hits zero -- so brightening the
// INPUT colour explodes rather than glows: 1.0 maps to about 7.24 and 1.4 to about 1931, and any
// channel already at 1.0 washes the hue toward white on the way. Scaling the radiance it returns
// has no ceiling and scales every channel equally, so a red axis handle stays red.
float4 PSLine(LVSOut i) : SV_TARGET {
    return float4(averInverseTonemap(srgbToLin(i.col)) * gBaseColor.x, 1.0);
}
