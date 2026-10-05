// Resolves the multisampled G-buffer twins into the single-sample targets every reader uses
// (denoiser, AverSR, NeuRAA): per pixel, the nearest covered sample's velocity, view Z and normal.
// Averaging these would invent depths and normals no surface has. A view Z of 0 is the clear value
// ("nothing drawn") and never wins.

Texture2DMS<float2> gMsVelocity : register(t0);
Texture2DMS<float>  gMsViewZ    : register(t1);
Texture2DMS<float4> gMsNormal   : register(t2);

struct VSOut { float4 pos : SV_Position; };

VSOut GBufResolveVS(uint id : SV_VertexID) {
    const float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

struct PSOut {
    float2 velocity        : SV_Target0;
    float  viewZ           : SV_Target1;
    float4 normalRoughness : SV_Target2;
};

PSOut GBufResolvePS(VSOut i) {
    const int2 p = int2(i.pos.xy);
    uint w, h, n;
    gMsViewZ.GetDimensions(w, h, n);
    uint best = 0;
    float bestZ = 3.0e38;
    for (uint s = 0; s < n; ++s) {
        const float z = gMsViewZ.Load(p, s);
        if (z > 0.0 && z < bestZ) { bestZ = z; best = s; }
    }
    PSOut o;
    o.velocity        = gMsVelocity.Load(p, best);
    o.viewZ           = bestZ < 3.0e38 ? bestZ : 0.0;
    o.normalRoughness = gMsNormal.Load(p, best);
    return o;
}
