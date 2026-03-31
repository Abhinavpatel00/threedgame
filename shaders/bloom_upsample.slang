#include "common_bindless.slang"

struct BloomUpPush
{
    uint src_texture_id;
    uint output_image_id;
    uint sampler_id;
    float blend_factor;

    uint width;
    uint height;
    float radius;
    float pad0;
    float src_texel_x;
    float src_texel_y;
};

[[vk::push_constant]]
BloomUpPush push;

float3 sample_tent(Texture2D src, SamplerState s, float2 uv, float radius, float2 texel_size)
{
    float2 texel = radius * texel_size;

    float3 a = src.SampleLevel(s, uv + texel * float2(-1.0f,  1.0f), 0.0f).rgb;
    float3 b = src.SampleLevel(s, uv + texel * float2( 0.0f,  1.0f), 0.0f).rgb;
    float3 c = src.SampleLevel(s, uv + texel * float2( 1.0f,  1.0f), 0.0f).rgb;

    float3 d = src.SampleLevel(s, uv + texel * float2(-1.0f,  0.0f), 0.0f).rgb;
    float3 e = src.SampleLevel(s, uv, 0.0f).rgb;
    float3 f = src.SampleLevel(s, uv + texel * float2( 1.0f,  0.0f), 0.0f).rgb;

    float3 g = src.SampleLevel(s, uv + texel * float2(-1.0f, -1.0f), 0.0f).rgb;
    float3 h = src.SampleLevel(s, uv + texel * float2( 0.0f, -1.0f), 0.0f).rgb;
    float3 i = src.SampleLevel(s, uv + texel * float2( 1.0f, -1.0f), 0.0f).rgb;

    float3 sample = e * 0.25f;
    sample += (b + d + f + h) * 0.125f;
    sample += (a + c + g + i) * 0.0625f;

    return sample;
}

[numthreads(16, 16, 1)]
void cs_main(uint3 id : SV_DispatchThreadID)
{
    if(id.x >= push.width || id.y >= push.height)
        return;

    int2 pix = int2(id.xy);

    Texture2D           src = textures[push.src_texture_id];
    RWTexture2D<float4> dst = storage_images[push.output_image_id];
    float2              texel_size = float2(max(push.src_texel_x, 1e-6f), max(push.src_texel_y, 1e-6f));

    float2 uv = (float2(pix) + 0.5f) / float2(push.width, push.height);
    float3 color = sample_tent(src, samplers[push.sampler_id], uv, max(push.radius, 0.5f), texel_size);

    float3 base = dst[pix].rgb;

    dst[pix] = float4(base + color * push.blend_factor, 1.0f);
}
