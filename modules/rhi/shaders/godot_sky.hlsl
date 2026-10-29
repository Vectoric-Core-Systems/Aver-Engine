// Fabric G6 SH3: a Godot sky shader as the engine's sky (docs/plans/fabric/G6_SHADERS.md, section SH3).
//
// SPLICED INTO THE SHARED PRELUDE at the marker line before skyColorFull (aver::rhi::spliceGodotSky), after godot_env_common.hlsl and the renamed Godot library,
// ONLY when a converted sky shader is registered; the prelude then also sees AVER_GD_SKY and routes skyColorFull / skyColor / averSkyIrradiance / averSkyRadianceCheap
// to the functions at the end of this file. The file is split at its source-marker line (the one with @GD_SOURCE@ in it, below): the head defines the hook macros, the
// converted source goes in the middle, the tail calls it. Everything here sees the prelude's declarations above the marker (gTime, gCamPos, gLightDir, gLightColor, gSkyParams, srgbToLin).

struct GdSkyCtx {
    float3 eyedir;       // Godot world axes
    float3 position;     // the camera, Godot world axes, metres
    float2 skyCoords;
    float2 screenUv;
    bool   cubemapPass;
    float3 color;
    float  alpha;
    float4 fog;
};
static GdSkyCtx gGdSky;

float3 gdeSunDirG()   { return gdeE2G(normalize(gLightDir.xyz)); }
float  gdeSunSize()   { return 2.0 * acos(clamp(gSkyParams.w, -1.0, 1.0)); }
float  gdeSunEnergy() { return gSkyParams.z * (1.0 / 104719.755); }

#define AVER_GD_TIME gTime.x
#define AVER_GD_EYEDIR gGdSky.eyedir
#define AVER_GD_POSITION gGdSky.position
#define AVER_GD_SKY_COORDS gGdSky.skyCoords
#define AVER_GD_SCREEN_UV gGdSky.screenUv
#define AVER_GD_AT_CUBEMAP_PASS gGdSky.cubemapPass
#define AVER_GD_AT_HALF_RES_PASS false
#define AVER_GD_AT_QUARTER_RES_PASS false
#define AVER_GD_HALF_RES_COLOR float4(0.0, 0.0, 0.0, 0.0)
#define AVER_GD_QUARTER_RES_COLOR float4(0.0, 0.0, 0.0, 0.0)
#define AVER_GD_COLOR gGdSky.color
#define AVER_GD_ALPHA gGdSky.alpha
#define AVER_GD_FOG gGdSky.fog
#define AVER_GD_LIGHT0_ENABLED true
#define AVER_GD_LIGHT0_DIRECTION gdeSunDirG()
#define AVER_GD_LIGHT0_COLOR srgbToLin(gLightColor.rgb)
#define AVER_GD_LIGHT0_ENERGY gdeSunEnergy()
#define AVER_GD_LIGHT0_SIZE gdeSunSize()
#define AVER_GD_LIGHT1_ENABLED false
#define AVER_GD_LIGHT1_DIRECTION float3(0.0, 1.0, 0.0)
#define AVER_GD_LIGHT1_COLOR float3(0.0, 0.0, 0.0)
#define AVER_GD_LIGHT1_ENERGY 0.0
#define AVER_GD_LIGHT1_SIZE 0.0
#define AVER_GD_LIGHT2_ENABLED false
#define AVER_GD_LIGHT2_DIRECTION float3(0.0, 1.0, 0.0)
#define AVER_GD_LIGHT2_COLOR float3(0.0, 0.0, 0.0)
#define AVER_GD_LIGHT2_ENERGY 0.0
#define AVER_GD_LIGHT2_SIZE 0.0
#define AVER_GD_LIGHT3_ENABLED false
#define AVER_GD_LIGHT3_DIRECTION float3(0.0, 1.0, 0.0)
#define AVER_GD_LIGHT3_COLOR float3(0.0, 0.0, 0.0)
#define AVER_GD_LIGHT3_ENERGY 0.0
#define AVER_GD_LIGHT3_SIZE 0.0
//@GD_SOURCE@
#undef AVER_GD_TIME
#undef AVER_GD_EYEDIR
#undef AVER_GD_POSITION
#undef AVER_GD_SKY_COORDS
#undef AVER_GD_SCREEN_UV
#undef AVER_GD_AT_CUBEMAP_PASS
#undef AVER_GD_AT_HALF_RES_PASS
#undef AVER_GD_AT_QUARTER_RES_PASS
#undef AVER_GD_HALF_RES_COLOR
#undef AVER_GD_QUARTER_RES_COLOR
#undef AVER_GD_COLOR
#undef AVER_GD_ALPHA
#undef AVER_GD_FOG
#undef AVER_GD_LIGHT0_ENABLED
#undef AVER_GD_LIGHT0_DIRECTION
#undef AVER_GD_LIGHT0_COLOR
#undef AVER_GD_LIGHT0_ENERGY
#undef AVER_GD_LIGHT0_SIZE
#undef AVER_GD_LIGHT1_ENABLED
#undef AVER_GD_LIGHT1_DIRECTION
#undef AVER_GD_LIGHT1_COLOR
#undef AVER_GD_LIGHT1_ENERGY
#undef AVER_GD_LIGHT1_SIZE
#undef AVER_GD_LIGHT2_ENABLED
#undef AVER_GD_LIGHT2_DIRECTION
#undef AVER_GD_LIGHT2_COLOR
#undef AVER_GD_LIGHT2_ENERGY
#undef AVER_GD_LIGHT2_SIZE
#undef AVER_GD_LIGHT3_ENABLED
#undef AVER_GD_LIGHT3_DIRECTION
#undef AVER_GD_LIGHT3_COLOR
#undef AVER_GD_LIGHT3_ENERGY
#undef AVER_GD_LIGHT3_SIZE
#undef AVER_GD_UBUF
#undef AVER_GD_GBUF
#undef ddx
#undef ddy
#undef ddx_coarse
#undef ddy_coarse
#undef ddx_fine
#undef ddy_fine
#undef fwidth

// The sky in a direction (engine world axes). cubemapPass: reflections and ambient light, false for the background. screenUv: the pixel in the background pass.
float3 averGdSky(float3 dirE, bool cubemapPass, float2 screenUv) {
    GdSkyCtx c = (GdSkyCtx)0;
    c.eyedir = gdeE2G(normalize(dirE));
    c.position = gdeE2G(gCamPos.xyz) * 0.01;
    c.cubemapPass = cubemapPass;
    c.screenUv = screenUv;
    c.skyCoords = screenUv;
    c.color = float3(0.0, 0.0, 0.0);
    c.alpha = 1.0;
    c.fog = float4(0.0, 0.0, 0.0, 0.0);
    gGdSky = c;
    AVER_GD_SKY_ENTRY();
    float3 col = lerp(gGdSky.color, gGdSky.fog.rgb, saturate(gGdSky.fog.a));
    return max(col, float3(0.0, 0.0, 0.0));
}
float3 averGdSky(float3 dirE, bool cubemapPass) { return averGdSky(dirE, cubemapPass, float2(0.5, 0.5)); }

// Ambient light from the sky: the cosine-weighted average of the sky about N, from the normal and four directions 60 degrees off it (AVER_GD_SKY_AMBIENT_TAPS
// = 1 evaluates the normal alone). The sky function can be expensive and this runs in every lit pixel: raise the tap count only for a cheap sky.
#ifndef AVER_GD_SKY_AMBIENT_TAPS
#define AVER_GD_SKY_AMBIENT_TAPS 5
#endif
float3 averGdSkyIrradiance(float3 N) {
    float3 sum = averGdSky(N, true);
#if AVER_GD_SKY_AMBIENT_TAPS > 1
    const float3 up = abs(N.z) < 0.99 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    const float3 T = normalize(cross(up, N));
    const float3 B = cross(N, T);
    const float s = 0.8660254, c = 0.5;   // sin / cos of 60 degrees
    sum = sum * 0.4;
    sum += 0.15 * averGdSky(N * c + T * s, true);
    sum += 0.15 * averGdSky(N * c - T * s, true);
    sum += 0.15 * averGdSky(N * c + B * s, true);
    sum += 0.15 * averGdSky(N * c - B * s, true);
#endif
    return sum;
}
