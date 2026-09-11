//
// RT64
//

Texture2D<float4> gInput : register(t0);
SamplerState gSampler : register(s0);

float4 PSMain(in float4 pos : SV_Position, in float2 uv : TEXCOORD0) : SV_TARGET {
    return gInput.SampleLevel(gSampler, uv, 0);
}
