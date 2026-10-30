// Custom fog programs on the volumetric fog's local volumes.
//
// Composed by aver::rhi::customFogProgram into the prelude of the AVER_FOG_VOLUMES + AVER_FOG_SHADERS variants of voxi_froxel.hlsl, ONLY when a fog volume
// carries a registered program. Split at two marker lines (the ones with @CUSTOM_SOURCE@ and @CUSTOM_CASES@ in them, below): the head defines the context and the hook
// macros, the translated sources go at the first, the dispatch cases at the second. voxi_froxel.hlsl calls averCustomFogEval for a volume whose shape word names a program.

struct CustomFogCtx {
    float3 world;     // the program's world axes, metres
    float3 object;    // the volume's centre
    float3 uvw;       // 0..1 inside the volume, the program's axes
    float3 size;      // the volume's size property, metres
    float  sdf;
    float3 albedo;
    float3 emission;
    float  density;
};
static CustomFogCtx gCustomFog;
// The volume's shader table, filled by the caller from the constants before the volume loop: AVER_CUSTOM_FOG_REGS registers per shader.
static float4 gCustomFogRegs[AVER_CUSTOM_FOG_REG_COUNT];

#define AVER_CUSTOM_TIME gTime.x
#define AVER_CUSTOM_WORLD_POSITION gCustomFog.world
#define AVER_CUSTOM_OBJECT_POSITION gCustomFog.object
#define AVER_CUSTOM_UVW gCustomFog.uvw
#define AVER_CUSTOM_SIZE gCustomFog.size
#define AVER_CUSTOM_SDF gCustomFog.sdf
#define AVER_CUSTOM_ALBEDO gCustomFog.albedo
#define AVER_CUSTOM_EMISSION gCustomFog.emission
#define AVER_CUSTOM_DENSITY gCustomFog.density
//@CUSTOM_SOURCE@
#undef AVER_CUSTOM_TIME
#undef AVER_CUSTOM_WORLD_POSITION
#undef AVER_CUSTOM_OBJECT_POSITION
#undef AVER_CUSTOM_UVW
#undef AVER_CUSTOM_SIZE
#undef AVER_CUSTOM_SDF
#undef AVER_CUSTOM_ALBEDO
#undef AVER_CUSTOM_EMISSION
#undef AVER_CUSTOM_DENSITY
#undef AVER_CUSTOM_UBUF
#undef AVER_CUSTOM_GBUF
#undef ddx
#undef ddy
#undef ddx_coarse
#undef ddy_coarse
#undef ddx_fine
#undef ddy_fine
#undef fwidth

// Runs shader `idx` (1-based) for the volume point and returns its density (per metre), albedo and emission. worldCm / centreCm / halfCm are engine centimetres and
// engine local half extents; u is the unit-box coordinate (-1..1, engine local axes).
void averCustomFogEval(uint idx, float3 worldCm, float3 centreCm, float3 halfCm, float3 u, out float density, out float3 albedo, out float3 emission) {
    CustomFogCtx c = (CustomFogCtx)0;
    c.world = cenvE2S(worldCm) * 0.01;
    c.object = cenvE2S(centreCm) * 0.01;
    const float3 ue = u * 0.5 + 0.5;
    c.uvw = float3(ue.y, ue.z, 1.0 - ue.x);
    c.size = float3(halfCm.y, halfCm.z, halfCm.x) * 0.02;
    c.sdf = 1000.0;
    c.albedo = float3(1.0, 1.0, 1.0);
    c.emission = float3(0.0, 0.0, 0.0);
    c.density = 1.0;
    gCustomFog = c;
//@CUSTOM_CASES@
    density = gCustomFog.density;
    albedo = saturate(gCustomFog.albedo);
    emission = max(gCustomFog.emission, float3(0.0, 0.0, 0.0));
}
