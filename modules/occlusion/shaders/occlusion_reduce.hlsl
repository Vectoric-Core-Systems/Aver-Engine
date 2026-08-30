// Aver.Occlusion: the HZB mip reduce.
//
// Moved out of a C++ raw-string literal; composed and -D'd exactly as before -- see the call site.

Texture2D<float> gSrc : register(t0);
RWTexture2D<float> gDst : register(u0);

cbuffer ReduceCB : register(b1) {
    uint2 gSrcSize;
    uint2 gDstSize;
};

[numthreads(8, 8, 1)]
void CSReduce(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= gDstSize.x || tid.y >= gDstSize.y) return;
    int2 base = int2(tid.xy) * 2;
    int2 maxSrc = int2(gSrcSize) - 1;
    int2 p0 = min(base,                 maxSrc);
    int2 p1 = min(base + int2(1, 0),    maxSrc);
    int2 p2 = min(base + int2(0, 1),    maxSrc);
    int2 p3 = min(base + int2(1, 1),    maxSrc);
    float z = gSrc.Load(int3(p0, 0));
    z = max(z, gSrc.Load(int3(p1, 0)));
    z = max(z, gSrc.Load(int3(p2, 0)));
    z = max(z, gSrc.Load(int3(p3, 0)));
    gDst[tid.xy] = z;
}
