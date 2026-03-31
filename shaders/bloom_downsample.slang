#include "common_bindless.slang"

struct BloomDownPush
{
    uint src_texture_id;
    uint output_image_id;
    uint sampler_id;
    uint first_pass;

    uint width;
    uint height;
    float threshold;
    float threshold_knee;
    float src_texel_x;
    float src_texel_y;
};

[[vk::push_constant]]
BloomDownPush push;

float3 soft_threshold(float3 color, float threshold, float knee)
{
    float brightness = max(color.r, max(color.g, color.b));
    float soft       = clamp(brightness - (threshold - knee), 0.0f, 2.0f * knee);
    soft             = (soft * soft) / max(4.0f * knee + 1e-5f, 1e-5f);
    float contrib    = max(brightness - threshold, soft) / max(brightness, 1e-5f);
    return color * contrib;
}

float3 sample_input_13_tap(Texture2D src, SamplerState s, float2 uv, float2 texel)
{
    float3 a = src.SampleLevel(s, uv + texel * float2(-2.0f,  2.0f), 0.0f).rgb;
    float3 b = src.SampleLevel(s, uv + texel * float2( 0.0f,  2.0f), 0.0f).rgb;
    float3 c = src.SampleLevel(s, uv + texel * float2( 2.0f,  2.0f), 0.0f).rgb;
    float3 d = src.SampleLevel(s, uv + texel * float2(-2.0f,  0.0f), 0.0f).rgb;
    float3 e = src.SampleLevel(s, uv, 0.0f).rgb;
    float3 f = src.SampleLevel(s, uv + texel * float2( 2.0f,  0.0f), 0.0f).rgb;
    float3 g = src.SampleLevel(s, uv + texel * float2(-2.0f, -2.0f), 0.0f).rgb;
    float3 h = src.SampleLevel(s, uv + texel * float2( 0.0f, -2.0f), 0.0f).rgb;
    float3 i = src.SampleLevel(s, uv + texel * float2( 2.0f, -2.0f), 0.0f).rgb;
    float3 j = src.SampleLevel(s, uv + texel * float2(-1.0f,  1.0f), 0.0f).rgb;
    float3 k = src.SampleLevel(s, uv + texel * float2( 1.0f,  1.0f), 0.0f).rgb;
    float3 l = src.SampleLevel(s, uv + texel * float2(-1.0f, -1.0f), 0.0f).rgb;
    float3 m = src.SampleLevel(s, uv + texel * float2( 1.0f, -1.0f), 0.0f).rgb;

    float3 sample = (a + c + g + i) * 0.03125f;
    sample += (b + d + f + h) * 0.0625f;
    sample += (e + j + k + l + m) * 0.125f;
    return sample;
}

float3 sample_input_4_tap(Texture2D src, SamplerState s, float2 uv, float2 texel)
{
    float2 half_t = texel * 0.5f;
    float3 a = src.SampleLevel(s, uv + float2(-half_t.x, -half_t.y), 0.0f).rgb;
    float3 b = src.SampleLevel(s, uv + float2( half_t.x, -half_t.y), 0.0f).rgb;
    float3 c = src.SampleLevel(s, uv + float2(-half_t.x,  half_t.y), 0.0f).rgb;
    float3 d = src.SampleLevel(s, uv + float2( half_t.x,  half_t.y), 0.0f).rgb;
    return (a + b + c + d) * 0.25f;
}

[numthreads(16, 16, 1)]
void cs_main(uint3 id : SV_DispatchThreadID)
{
    if(id.x >= push.width || id.y >= push.height)
        return;

    int2 pix = int2(id.xy);

    Texture2D           src = textures[push.src_texture_id];
    RWTexture2D<float4> dst = storage_images[push.output_image_id];
    float2              texel = float2(max(push.src_texel_x, 1e-6f), max(push.src_texel_y, 1e-6f));

    float2 uv = (float2(pix) + 0.5f) / float2(push.width, push.height);

    float3 color = sample_input_4_tap(src, samplers[push.sampler_id], uv, texel);

    if(push.first_pass != 0)
    {
        color = soft_threshold(color, push.threshold, push.threshold * push.threshold_knee);
    }

    dst[pix] = float4(max(color, 0.0f.xxx), 1.0f);
}
