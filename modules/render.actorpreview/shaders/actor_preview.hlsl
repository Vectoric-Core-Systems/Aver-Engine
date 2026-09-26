// The asset preview's own camera, vertex shader and pixel shaders: the meshes (PreviewPS), the studio
// backdrop, the floor grid and the bounds box.
//
// Compiled as the tail of rhi::sharedShaderPrelude(). Moved out of a C++ raw-string literal.

// The preview's own camera. Must be b4, the feature register: the backend rebinds b0 per setPipeline.
cbuffer PreviewFrame : register(b4) {
    float4x4 gPreviewViewProj;
    float4   gPreviewEye;      // xyz = eye, w = unused
    float4   gPreviewKey;      // xyz = direction TO the key light, w = its intensity
    float4   gPreviewAmbient;  // rgb = sky fill, w = selection highlight strength
    float4   gPreviewMode;     // x = PreviewViewMode: 0 lit, 1 unlit, 2 wireframe, 3 normals
    float4   gPreviewGrid;     // xy = fade centre, z = minor line step (cm), w = fade radius (cm)
};

static const uint kPreviewModeUnlit   = 1;
static const uint kPreviewModeWire    = 2;
static const uint kPreviewModeNormals = 3;

// What the preview vertex shader hands the pixel shader.
struct PreviewOut {
    float4 pos   : SV_POSITION;
    float3 nrmWS : NORMAL;
    float3 wpos  : TEXCOORD0;
    // ADDED FOR THE MATERIAL PATH: AverVertex (PbrShaders.cpp) carries a uv, and a graph that
    // samples a map needs one to sample it with. PreviewPS below still ignores it -- the simple
    // shader has no texture to sample -- so this costs it one unread interpolant, not a behaviour
    // change.
    float2 uv    : TEXCOORD1;
};

// Transforms a vertex to clip space and its normal to world space.
PreviewOut PreviewVS(VSIn i) {
    PreviewOut o;
    float4 wp = mul(float4(i.pos, 1.0), gWorld);
    o.wpos  = wp.xyz;
    o.pos   = mul(wp, gPreviewViewProj);
    o.nrmWS = normalize(averTransformNormal(i.nrm, gWorld));
    // Taken straight from VSIn, exactly like VSMain does in RHIShaders.cpp -- VSIn already carries
    // it (every mesh in this engine does), so nothing upstream of this shader has to change.
    o.uv    = i.uv;
    return o;
}

// The wireframe line colour: the draw's own tint lifted toward white, orange when selected.
float3 previewWireColor(float3 base) {
    float3 c = lerp(base, float3(0.92, 0.94, 1.0), 0.55);
    return lerp(c, float3(1.0, 0.62, 0.2), saturate(gPreviewAmbient.w));
}

// Shades a pixel with one key light, a hemisphere fill and a selection rim, or the view mode's stand-in.
float4 PreviewPS(PreviewOut i) : SV_TARGET {
    float3 n = normalize(i.nrmWS);
    uint mode = (uint)gPreviewMode.x;
    // Display values straight into the UNORM target, the way a normal map reads in an image viewer.
    if (mode == kPreviewModeNormals) return float4(n * 0.5 + 0.5, 1.0);
    if (mode == kPreviewModeWire) return float4(previewWireColor(gBaseColor.rgb), 1.0);

    float3 base = gBaseColor.rgb;
    float3 lit = base;
    if (mode != kPreviewModeUnlit) {
        float3 l = normalize(gPreviewKey.xyz);
        float ndl = saturate(dot(n, l));
        float up = n.z * 0.5 + 0.5;
        float3 fill = lerp(gPreviewAmbient.rgb * 0.35, gPreviewAmbient.rgb, up);
        lit = base * (fill + ndl * gPreviewKey.w);
    }

    float rim = pow(1.0 - saturate(dot(n, normalize(gPreviewEye.xyz - i.wpos))), 3.0);
    lit += gPreviewAmbient.w * rim * float3(1.0, 0.62, 0.2);

    return float4(toGamma(acesTonemap(lit)), 1.0);
}

// ---- the studio backdrop: a fullscreen triangle, drawn first with depth off ----

struct PreviewBackdropOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;   // y = 0 at the top edge, 1 at the bottom
};

PreviewBackdropOut PreviewBackdropVS(uint id : SV_VertexID) {
    PreviewBackdropOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv  = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// Display values, like the rest of the target: blue-grey overhead down to near-black at the floor.
float4 PreviewBackdropPS(PreviewBackdropOut i) : SV_TARGET {
    const float3 top    = float3(0.29, 0.31, 0.35);
    const float3 bottom = float3(0.10, 0.10, 0.11);
    float t = smoothstep(0.0, 1.0, saturate(i.uv.y));
    // One 8-bit step of noise so the long, shallow ramp does not band.
    float dither = frac(52.9829189 * frac(dot(i.pos.xy, float2(0.06711056, 0.00583715)))) - 0.5;
    return float4(lerp(top, bottom, t) + dither / 255.0, 1.0);
}

// ---- the floor grid: a large quad through PreviewVS, blended, depth-tested against the meshes ----

// Anti-aliased coverage of a grid's lines, widthPx wide, faded out as its cells shrink toward a
// pixel so distant lines do not shimmer.
float previewGridCoverage(float2 p, float cellSize, float widthPx) {
    float2 c = p / cellSize;
    float2 w = max(fwidth(c), 1e-6);
    float2 d = abs(frac(c - 0.5) - 0.5) / w;   // pixels to the nearest line on each axis
    float cover = saturate(widthPx * 0.5 + 0.5 - min(d.x, d.y));
    float density = saturate(1.5 - 4.0 * max(w.x, w.y));
    return cover * density;
}

float4 PreviewGridPS(PreviewOut i) : SV_TARGET {
    float2 p = i.wpos.xy;
    float cellSize = gPreviewGrid.z;
    float minor = previewGridCoverage(p, cellSize, 1.0);
    float major = previewGridCoverage(p, cellSize * 10.0, 1.5);
    float2 aw = max(fwidth(p), 1e-6);
    float axisX = saturate(1.25 - abs(p.y) / aw.y);   // the world X axis is the line y = 0
    float axisY = saturate(1.25 - abs(p.x) / aw.x);

    float3 col = float3(0.46, 0.47, 0.50);
    float a = minor * 0.28;
    col = lerp(col, float3(0.60, 0.61, 0.64), major);
    a = max(a, major * 0.55);
    col = lerp(col, float3(0.78, 0.26, 0.24), axisX);
    a = max(a, axisX * 0.75);
    col = lerp(col, float3(0.36, 0.70, 0.28), axisY);
    a = max(a, axisY * 0.75);

    float fade = 1.0 - smoothstep(gPreviewGrid.w * 0.4, gPreviewGrid.w, length(p - gPreviewGrid.xy));
    float3 v = normalize(gPreviewEye.xyz - i.wpos);
    fade *= saturate(abs(v.z) * 8.0);   // thins out toward the horizon, where the lines alias
    a *= fade;
    if (a < 0.004) discard;
    return float4(col, a);
}

// ---- the bounds box: the unit box scaled to the AABB, only the rim of each face kept ----

float4 PreviewBoundsPS(PreviewOut i) : SV_TARGET {
    float2 e = min(i.uv, 1.0 - i.uv) / max(fwidth(i.uv), 1e-6);   // pixels to the face's edge
    float a = saturate(1.25 - min(e.x, e.y));
    if (a < 0.01) discard;
    return float4(1.0, 0.80, 0.28, a);
}
