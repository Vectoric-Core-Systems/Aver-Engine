// precompiled_space1.hlsl -- fixture for PrecompiledShaderTest.
//
// A tiny compute shader whose binding layout is the awkward one the test exists to prove: the
// constant buffer and the sampler sit in register SPACE 1, the texture SRV and the UAV in space 0.
// Every resource is used, so the compiler cannot optimise a declaration (and with it the register
// the root signature must cover) away.
//
// The test embeds this file's DXIL as tests/rhi/src/PrecompiledShaderFixture.inc. Recompile and
// regenerate that file whenever this one changes -- the exact command is in its header.
cbuffer Params : register(b0, space1) {
    float2 gUvScale;
    float  gGain;
    float  gPad;
};
SamplerState gSampler : register(s0, space1);
Texture2D<float4>   gSrc : register(t0, space0);
RWTexture2D<float4> gDst : register(u0, space0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const float2 uv = (float2(id.xy) + 0.5) * gUvScale;
    gDst[id.xy] = gSrc.SampleLevel(gSampler, uv, 0) * gGain;
}
