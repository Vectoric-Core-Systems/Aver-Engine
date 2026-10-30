// A custom sky program as the engine's sky.
//
// SPLICED INTO THE SHARED PRELUDE at the marker line before skyColorFull (aver::rhi::spliceCustomSky), after custom_env_common.hlsl and the renamed helper library,
// ONLY when a custom sky program is registered; the prelude then also sees AVER_CUSTOM_SKY and routes skyColorFull / skyColor / averSkyIrradiance / averSkyRadianceCheap
// to the functions at the end of this file. The file is split at its source-marker line (the one with @CUSTOM_SOURCE@ in it, below): the head defines the hook macros, the
// translated source goes in the middle, the tail calls it. Everything here sees the prelude's declarations above the marker (gTime, gCamPos, gLightDir, gLightColor, gSkyParams, srgbToLin).

struct CustomSkyCtx {
    float3 eyedir;       // the program's world axes
    float3 position;     // the camera, the program's world axes, metres
    float2 skyCoords;
    float2 screenUv;
    bool   cubemapPass;
    float3 color;
    float  alpha;
    float4 fog;
};
static CustomSkyCtx gCustomSky;

float3 customSunDir()    { return cenvE2S(normalize(gLightDir.xyz)); }
float  customSunSize()   { return 2.0 * acos(clamp(gSkyParams.w, -1.0, 1.0)); }
float  customSunEnergy() { return gSkyParams.z * (1.0 / 104719.755); }

#define AVER_CUSTOM_TIME gTime.x
#define AVER_CUSTOM_EYEDIR gCustomSky.eyedir
#define AVER_CUSTOM_POSITION gCustomSky.position
#define AVER_CUSTOM_SKY_COORDS gCustomSky.skyCoords
#define AVER_CUSTOM_SCREEN_UV gCustomSky.screenUv
#define AVER_CUSTOM_AT_CUBEMAP_PASS gCustomSky.cubemapPass
#define AVER_CUSTOM_AT_HALF_RES_PASS false
#define AVER_CUSTOM_AT_QUARTER_RES_PASS false
#define AVER_CUSTOM_HALF_RES_COLOR float4(0.0, 0.0, 0.0, 0.0)
#define AVER_CUSTOM_QUARTER_RES_COLOR float4(0.0, 0.0, 0.0, 0.0)
#define AVER_CUSTOM_COLOR gCustomSky.color
#define AVER_CUSTOM_ALPHA gCustomSky.alpha
#define AVER_CUSTOM_FOG gCustomSky.fog
#define AVER_CUSTOM_LIGHT0_ENABLED true
#define AVER_CUSTOM_LIGHT0_DIRECTION customSunDir()
#define AVER_CUSTOM_LIGHT0_COLOR srgbToLin(gLightColor.rgb)
#define AVER_CUSTOM_LIGHT0_ENERGY customSunEnergy()
#define AVER_CUSTOM_LIGHT0_SIZE customSunSize()
#define AVER_CUSTOM_LIGHT1_ENABLED false
#define AVER_CUSTOM_LIGHT1_DIRECTION float3(0.0, 1.0, 0.0)
#define AVER_CUSTOM_LIGHT1_COLOR float3(0.0, 0.0, 0.0)
#define AVER_CUSTOM_LIGHT1_ENERGY 0.0
#define AVER_CUSTOM_LIGHT1_SIZE 0.0
#define AVER_CUSTOM_LIGHT2_ENABLED false
#define AVER_CUSTOM_LIGHT2_DIRECTION float3(0.0, 1.0, 0.0)
#define AVER_CUSTOM_LIGHT2_COLOR float3(0.0, 0.0, 0.0)
#define AVER_CUSTOM_LIGHT2_ENERGY 0.0
#define AVER_CUSTOM_LIGHT2_SIZE 0.0
#define AVER_CUSTOM_LIGHT3_ENABLED false
#define AVER_CUSTOM_LIGHT3_DIRECTION float3(0.0, 1.0, 0.0)
#define AVER_CUSTOM_LIGHT3_COLOR float3(0.0, 0.0, 0.0)
#define AVER_CUSTOM_LIGHT3_ENERGY 0.0
#define AVER_CUSTOM_LIGHT3_SIZE 0.0
//@CUSTOM_SOURCE@
#undef AVER_CUSTOM_TIME
#undef AVER_CUSTOM_EYEDIR
#undef AVER_CUSTOM_POSITION
#undef AVER_CUSTOM_SKY_COORDS
#undef AVER_CUSTOM_SCREEN_UV
#undef AVER_CUSTOM_AT_CUBEMAP_PASS
#undef AVER_CUSTOM_AT_HALF_RES_PASS
#undef AVER_CUSTOM_AT_QUARTER_RES_PASS
#undef AVER_CUSTOM_HALF_RES_COLOR
#undef AVER_CUSTOM_QUARTER_RES_COLOR
#undef AVER_CUSTOM_COLOR
#undef AVER_CUSTOM_ALPHA
#undef AVER_CUSTOM_FOG
#undef AVER_CUSTOM_LIGHT0_ENABLED
#undef AVER_CUSTOM_LIGHT0_DIRECTION
#undef AVER_CUSTOM_LIGHT0_COLOR
#undef AVER_CUSTOM_LIGHT0_ENERGY
#undef AVER_CUSTOM_LIGHT0_SIZE
#undef AVER_CUSTOM_LIGHT1_ENABLED
#undef AVER_CUSTOM_LIGHT1_DIRECTION
#undef AVER_CUSTOM_LIGHT1_COLOR
#undef AVER_CUSTOM_LIGHT1_ENERGY
#undef AVER_CUSTOM_LIGHT1_SIZE
#undef AVER_CUSTOM_LIGHT2_ENABLED
#undef AVER_CUSTOM_LIGHT2_DIRECTION
#undef AVER_CUSTOM_LIGHT2_COLOR
#undef AVER_CUSTOM_LIGHT2_ENERGY
#undef AVER_CUSTOM_LIGHT2_SIZE
#undef AVER_CUSTOM_LIGHT3_ENABLED
#undef AVER_CUSTOM_LIGHT3_DIRECTION
#undef AVER_CUSTOM_LIGHT3_COLOR
#undef AVER_CUSTOM_LIGHT3_ENERGY
#undef AVER_CUSTOM_LIGHT3_SIZE
#undef AVER_CUSTOM_UBUF
#undef AVER_CUSTOM_GBUF
#undef ddx
#undef ddy
#undef ddx_coarse
#undef ddy_coarse
#undef ddx_fine
#undef ddy_fine
#undef fwidth

// The sky in a direction (engine world axes). cubemapPass: reflections and ambient light, false for the background. screenUv: the pixel in the background pass.
float3 averCustomSky(float3 dirE, bool cubemapPass, float2 screenUv) {
    CustomSkyCtx c = (CustomSkyCtx)0;
    c.eyedir = cenvE2S(normalize(dirE));
    c.position = cenvE2S(gCamPos.xyz) * 0.01;
    c.cubemapPass = cubemapPass;
    c.screenUv = screenUv;
    c.skyCoords = screenUv;
    c.color = float3(0.0, 0.0, 0.0);
    c.alpha = 1.0;
    c.fog = float4(0.0, 0.0, 0.0, 0.0);
    gCustomSky = c;
    AVER_CUSTOM_SKY_ENTRY();
    float3 col = lerp(gCustomSky.color, gCustomSky.fog.rgb, saturate(gCustomSky.fog.a));
    return max(col, float3(0.0, 0.0, 0.0));
}
float3 averCustomSky(float3 dirE, bool cubemapPass) { return averCustomSky(dirE, cubemapPass, float2(0.5, 0.5)); }

// Ambient light from the sky: the cosine-weighted average of the sky about N, from the normal and four directions 60 degrees off it (AVER_CUSTOM_SKY_AMBIENT_TAPS
// = 1 evaluates the normal alone). The sky function can be expensive and this runs in every lit pixel: raise the tap count only for a cheap sky.
#ifndef AVER_CUSTOM_SKY_AMBIENT_TAPS
#define AVER_CUSTOM_SKY_AMBIENT_TAPS 5
#endif
float3 averCustomSkyIrradiance(float3 N) {
    float3 sum = averCustomSky(N, true);
#if AVER_CUSTOM_SKY_AMBIENT_TAPS > 1
    const float3 up = abs(N.z) < 0.99 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    const float3 T = normalize(cross(up, N));
    const float3 B = cross(N, T);
    const float s = 0.8660254, c = 0.5;   // sin / cos of 60 degrees
    sum = sum * 0.4;
    sum += 0.15 * averCustomSky(N * c + T * s, true);
    sum += 0.15 * averCustomSky(N * c - T * s, true);
    sum += 0.15 * averCustomSky(N * c + B * s, true);
    sum += 0.15 * averCustomSky(N * c - B * s, true);
#endif
    return sum;
}
