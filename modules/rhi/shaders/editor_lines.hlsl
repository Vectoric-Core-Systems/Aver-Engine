// Editor lines drawn AFTER the camera post chain -- see aver/rhi/EditorLines.hpp for the whole design.
// Compiled as the tail of rhi::sharedShaderPrelude() (gViewProj, gInvViewProjRel, the PerObject
// block, srgbToLin). One vertex/pixel pair for every backend; AVER_EDITOR_LINE_MS picks the
// multisampled depth read.
//
// PerObject, as EditorLines::replay fills it:
//   gWorld         the line mesh's world matrix (row vectors, like every other draw)
//   gBaseColor     x line width (display px), y depth-tested (0/1), zw the 3D view's rect size (px)
//   gMaterial      x 1 = the target stores sRGB itself (write linear), zw the 3D view's rect origin
//   gEmissive      xy the whole target's size (px)

// Positions arrive split xy + z (EditorLineVertex's comment says why); member order is the Vulkan
// attribute order.
struct ELIn {
    float2 axy    : POSITION0;
    float  az     : POSITION1;
    float2 bxy    : POSITION2;
    float  bz     : POSITION3;
    float4 col    : COLOR0;      // RGBA8Unorm, display colour
    float2 corner : TEXCOORD0;   // x: 0 at a, 1 at b; y: -1 / +1 side
};
struct ELOut {
    float4 pos  : SV_POSITION;
    float3 col  : COLOR0;
    float  side : TEXCOORD0;     // signed distance from the centre line, in pixels
};

// The one-pixel anti-aliasing feather added outside the solid width.
static const float kEditorLineFeather = 1.0;

// Projects both endpoints, clips the segment to the near side of the camera, and moves this corner
// sideways (and past its end, for a square cap) by half the width in SCREEN pixels. Offsetting xy by
// px * 2/size * w keeps z/w -- the line's own depth -- untouched.
ELOut VSEditorLine(ELIn i) {
    float4 ca = mul(mul(float4(i.axy, i.az, 1.0), gWorld), gViewProj);
    float4 cb = mul(mul(float4(i.bxy, i.bz, 1.0), gWorld), gViewProj);
    ELOut o;
    o.col = i.col.rgb;
    o.side = 0.0;
    const float kNearW = 1e-3;
    if (ca.w < kNearW && cb.w < kNearW) {   // wholly behind the eye: a degenerate, clipped corner
        o.pos = float4(0.0, 0.0, -1.0, 1.0);
        return o;
    }
    if (ca.w < kNearW) ca = lerp(ca, cb, (kNearW - ca.w) / (cb.w - ca.w));
    if (cb.w < kNearW) cb = lerp(cb, ca, (kNearW - cb.w) / (ca.w - cb.w));

    const float2 rectSize = max(gBaseColor.zw, float2(1.0, 1.0));
    const float2 sa = ca.xy / ca.w * 0.5 * rectSize;   // pixels from the view's centre, y up
    const float2 sb = cb.xy / cb.w * 0.5 * rectSize;
    float2 dir = sb - sa;
    const float len = length(dir);
    dir = len > 1e-4 ? dir / len : float2(1.0, 0.0);
    const float2 nrm = float2(-dir.y, dir.x);

    const float halfSolid = 0.5 * max(gBaseColor.x, 1.0);
    const float halfQuad  = halfSolid + kEditorLineFeather;
    const bool atB = i.corner.x > 0.5;
    float4 c = atB ? cb : ca;
    const float2 offPx = nrm * (i.corner.y * halfQuad) + dir * ((atB ? 1.0 : -1.0) * halfSolid);
    c.xy += offPx * (2.0 / rectSize) * c.w;
    o.pos = c;
    o.side = i.corner.y * halfQuad;
    return o;
}

#if AVER_EDITOR_LINE_MS
Texture2DMS<float> gEditorSceneDepth : register(t0);
float editorSceneDepth(int2 px) { return gEditorSceneDepth.Load(px, 0); }
float2 editorSceneDepthSize() { uint w, h, n; gEditorSceneDepth.GetDimensions(w, h, n); return float2(w, h); }
#else
Texture2D<float> gEditorSceneDepth : register(t0);
float editorSceneDepth(int2 px) { return gEditorSceneDepth.Load(int3(px, 0)); }
float2 editorSceneDepthSize() { uint w, h; gEditorSceneDepth.GetDimensions(w, h); return float2(w, h); }
#endif

// A camera-relative point on this pixel's ray at NDC depth z.
float3 editorRelPoint(float2 ndc, float z) {
    const float4 p = mul(float4(ndc, z, 1.0), gInvViewProjRel);
    return p.xyz / p.w;
}

// ---- THE WIREFRAME VIEW (EditorLines::queueWire) ----
// A scene mesh's own vertices (VSIn, the engine's MeshVertex) through a FillMode::Wireframe pipeline:
// gWorld is its world matrix, gBaseColor.rgb the wire colour, already in the target's encoding.
float4 VSEditorWire(VSIn i) : SV_POSITION {
    return mul(mul(float4(i.pos, 1.0), gWorld), gViewProj);
}

float4 PSEditorWire(float4 pos : SV_POSITION) : SV_TARGET {
    return float4(gBaseColor.rgb, 1.0);
}

float4 PSEditorLine(ELOut i) : SV_TARGET {
    if (gBaseColor.y > 0.5) {
        // The scene depth covers the whole (render-scaled) target, so a target pixel maps to it by
        // one uniform scale; the NDC of the pixel comes from the 3D view's rect inside the target.
        const float2 depthSize = editorSceneDepthSize();
        const float2 scale = depthSize / max(gEmissive.xy, float2(1.0, 1.0));
        const int2 px = int2(min(i.pos.xy * scale, depthSize - 1.0));
        // The FARTHEST of a 2x2 footprint, so a line along a silhouette -- an outline on its own mesh,
        // a collider edge -- is not hidden because the coarser depth happened to land on the near side.
        const int2 lim = int2(depthSize) - 1;
        float zs = editorSceneDepth(px);
        zs = max(zs, editorSceneDepth(min(px + int2(1, 0), lim)));
        zs = max(zs, editorSceneDepth(min(px + int2(0, 1), lim)));
        zs = max(zs, editorSceneDepth(min(px + int2(1, 1), lim)));
        if (zs < 1.0) {   // 1.0 is the clear value: nothing there, the line shows against the sky
            const float2 uv = (i.pos.xy - gMaterial.zw) / max(gBaseColor.zw, float2(1.0, 1.0));
            const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
            const float dScene = length(editorRelPoint(ndc, zs));
            const float dLine  = length(editorRelPoint(ndc, i.pos.z));
            // 0.2% + 1 cm behind the surface still counts as ON it (the grid on a floor).
            if (dLine > dScene * 1.002 + 1.0) discard;
        }
    }
    const float halfSolid = 0.5 * max(gBaseColor.x, 1.0);
    const float coverage = saturate(halfSolid + 0.5 - abs(i.side));
    if (coverage <= 0.0) discard;
    const float3 col = gMaterial.x > 0.5 ? srgbToLin(i.col) : i.col;
    return float4(col * coverage, coverage);   // premultiplied
}
