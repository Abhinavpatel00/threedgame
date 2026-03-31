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
};

[[vk::push_constant]]
BloomDownPush push;

[[vk::binding(0, 0)]]
Texture2D textures[];

[[vk::binding(1, 0)]]
SamplerState samplers[];

[[vk::binding(2, 0)]]
RWTexture2D<float4> storage_images[];

float3 soft_threshold(float3 color, float threshold, float knee)
{
    float brightness = max(color.r, max(color.g, color.b));
    float soft       = clamp(brightness - (threshold - knee), 0.0f, 2.0f * knee);
    soft             = (soft * soft) / max(4.0f * knee + 1e-5f, 1e-5f);
    float contrib    = max(brightness - threshold, soft) / max(brightness, 1e-5f);
    return color * contrib;
}

float3 sample_input_13_tap(Texture2D src, SamplerState s, float2 uv)
{
    uint src_w = 1;
    uint src_h = 1;
    src.GetDimensions(src_w, src_h);
    float2 texel = 1.0f / float2(src_w, src_h);

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

[numthreads(16, 16, 1)]
void cs_main(uint3 id : SV_DispatchThreadID)
{
    if(id.x >= push.width || id.y >= push.height)
        return;

    int2 pix = int2(id.xy);

    Texture2D           src = textures[push.src_texture_id];
    RWTexture2D<float4> dst = storage_images[push.output_image_id];

    float2 uv = (float2(pix) + 0.5f) / float2(push.width, push.height);

    float3 color = sample_input_13_tap(src, samplers[push.sampler_id], uv);

    if(push.first_pass != 0)
    {
        color = soft_threshold(color, push.threshold, push.threshold * push.threshold_knee);
    }

    dst[pix] = float4(max(color, 0.0f.xxx), 1.0f);
}
