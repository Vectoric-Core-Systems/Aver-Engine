// Fabric G6 SH3: Godot fog shaders on the volumetric fog's local volumes (docs/plans/fabric/G6_SHADERS.md, section SH3).
//
// Composed by aver::rhi::godotFogProgram into the prelude of the AVER_FOG_VOLUMES + AVER_FOG_SHADERS variants of voxi_froxel.hlsl, ONLY when a fog volume
// carries a converted FogMaterial shader. Split at two marker lines (the ones with @GD_SOURCE@ and @GD_CASES@ in them, below): the head defines the context and the hook
// macros, the converted sources go at the first, the dispatch cases at the second. voxi_froxel.hlsl calls averGdFogEval for a volume whose shape word names a shader.

struct GdFogCtx {
    float3 world;     // Godot world axes, metres
    float3 object;    // the volume's centre
    float3 uvw;       // 0..1 inside the volume, Godot axes
    float3 size;      // the volume's size property, metres
    float  sdf;
    float3 albedo;
    float3 emission;
    float  density;
};
static GdFogCtx gGdFog;
// The volume's shader table, filled by the caller from the constants before the volume loop: AVER_GD_FOG_REGS registers per shader.
static float4 gGdFogRegs[AVER_GD_FOG_REG_COUNT];

#define AVER_GD_TIME gTime.x
#define AVER_GD_WORLD_POSITION gGdFog.world
#define AVER_GD_OBJECT_POSITION gGdFog.object
#define AVER_GD_UVW gGdFog.uvw
#define AVER_GD_SIZE gGdFog.size
#define AVER_GD_SDF gGdFog.sdf
#define AVER_GD_ALBEDO gGdFog.albedo
#define AVER_GD_EMISSION gGdFog.emission
#define AVER_GD_DENSITY gGdFog.density
//@GD_SOURCE@
#undef AVER_GD_TIME
#undef AVER_GD_WORLD_POSITION
#undef AVER_GD_OBJECT_POSITION
#undef AVER_GD_UVW
#undef AVER_GD_SIZE
#undef AVER_GD_SDF
#undef AVER_GD_ALBEDO
#undef AVER_GD_EMISSION
#undef AVER_GD_DENSITY
#undef AVER_GD_UBUF
#undef AVER_GD_GBUF
#undef ddx
#undef ddy
#undef ddx_coarse
#undef ddy_coarse
#undef ddx_fine
#undef ddy_fine
#undef fwidth

// Runs shader `idx` (1-based) for the volume point and returns its density (per metre), albedo and emission. worldCm / centreCm / halfCm are engine centimetres and
// engine local half extents; u is the unit-box coordinate (-1..1, engine local axes).
void averGdFogEval(uint idx, float3 worldCm, float3 centreCm, float3 halfCm, float3 u, out float density, out float3 albedo, out float3 emission) {
    GdFogCtx c = (GdFogCtx)0;
    c.world = gdeE2G(worldCm) * 0.01;
    c.object = gdeE2G(centreCm) * 0.01;
    const float3 ue = u * 0.5 + 0.5;
    c.uvw = float3(ue.y, ue.z, 1.0 - ue.x);
    c.size = float3(halfCm.y, halfCm.z, halfCm.x) * 0.02;
    c.sdf = 1000.0;
    c.albedo = float3(1.0, 1.0, 1.0);
    c.emission = float3(0.0, 0.0, 0.0);
    c.density = 1.0;
    gGdFog = c;
//@GD_CASES@
    density = gGdFog.density;
    albedo = saturate(gGdFog.albedo);
    emission = max(gGdFog.emission, float3(0.0, 0.0, 0.0));
}
