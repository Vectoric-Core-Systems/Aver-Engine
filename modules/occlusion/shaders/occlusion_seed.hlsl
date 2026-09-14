// Aver.Occlusion: the HZB seed kernel.
//
// Moved out of a C++ raw-string literal; composed and -D'd exactly as before -- see the call site.

#if AVER_HZB_MS
Texture2DMS<float, AVER_HZB_SAMPLES> gDepth : register(t0);
#else
Texture2D<float> gDepth : register(t0);
#endif
RWTexture2D<float> gMip0 : register(u0);

cbuffer SeedCB : register(b1) {
    uint2 gSrcSize;
    uint2 gSeedPad;
};

[numthreads(8, 8, 1)]
void CSSeed(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= gSrcSize.x || tid.y >= gSrcSize.y) return;
    int2 p = int2(tid.xy);
#if AVER_HZB_MS
    float z = gDepth.Load(p, 0);
#if AVER_HZB_SAMPLES > 1
    z = max(z, gDepth.Load(p, 1));
#endif
#if AVER_HZB_SAMPLES > 2
    z = max(z, gDepth.Load(p, 2));
    z = max(z, gDepth.Load(p, 3));
#endif
#if AVER_HZB_SAMPLES > 4
    // D3D12's own MSAA range tops out at 8x (DeviceCaps::msaaMask's own comment: "bit N set => N
    // samples supported (bits 1,2,4,8)"), so 4 explicit taps past the first four cover every sample
    // count this engine can ever report.
    z = max(z, gDepth.Load(p, 4));
    z = max(z, gDepth.Load(p, 5));
    z = max(z, gDepth.Load(p, 6));
    z = max(z, gDepth.Load(p, 7));
#endif
#else
    float z = gDepth.Load(int3(p, 0));
#endif
    gMip0[p] = z;
}
