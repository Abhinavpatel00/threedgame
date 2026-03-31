import tonemapping;
#include "common_bindless.slang"

struct PostPush
{
  uint src_texture_id;
  uint dof_texture_id;
  uint output_image_id;
  uint sampler_id;

  uint  bloom_texture_id;
  float bloom_intensity;
  uint  width;
  uint  height;

  uint half_width;
  uint half_height;
  uint dof_enabled;
  float exposure;

  uint  frame;
  float dof_max_blur_size;
  float dof_rad_scale;
  float pad0;
};

__intrinsic_op(cmpGT) public vector<bool, N> greaterThan<T, let N : int>(vector<T, N> x, vector<T, N> y);

inline float3 toSrgb(float3 rgb)
{
  float3 low  = rgb * 12.92f;
  float3 high = fma(pow(rgb, float3(1.0F / 2.4F)), float3(1.055F), float3(-0.055F));
  return lerp(low, high, float3(greaterThan(rgb, float3(0.0031308F))));
}

inline float3 toLinear(float3 srgb)
{
  float3 low  = srgb / 12.92F;
  float3 high = pow((srgb + float3(0.055F)) / 1.055F, float3(2.4F));
  return lerp(low, high, float3(greaterThan(srgb, float3(0.04045F))));
}

inline float3 tonemapACES(float3 color)
{
  const float3x3 ACESInputMat = float3x3(0.59719F, 0.07600F, 0.02840F,
                                         0.35458F, 0.90834F, 0.13383F,
                                         0.04823F, 0.01566F, 0.83777F);
  color = mul(color, ACESInputMat);

  float3 a = color * (color + float3(0.0245786F)) - float3(0.000090537F);
  float3 b = color * (float3(0.983729F) * color + float3(0.4329510F)) + float3(0.238081F);
  color = a / b;

  const float3x3 ACESOutputMat = float3x3(1.60475F, -0.10208F, -0.00327F,
                                          -0.53108F, 1.10813F, -0.07276F,
                                          -0.07367F, -0.00605F, 1.07602F);
  color = mul(color, ACESOutputMat);
  return toLinear(color);
}

[[vk::push_constant]]
PostPush push;

static const int DOF_TAP_COUNT = 12;
static const float2 DOF_POISSON[DOF_TAP_COUNT] = {
  float2(-0.326212f, -0.405810f),
  float2(-0.840144f, -0.073580f),
  float2(-0.695914f,  0.457137f),
  float2(-0.203345f,  0.620716f),
  float2( 0.962340f, -0.194983f),
  float2( 0.473434f, -0.480026f),
  float2( 0.519456f,  0.767022f),
  float2( 0.185461f, -0.893124f),
  float2( 0.507431f,  0.064425f),
  float2( 0.896420f,  0.412458f),
  float2(-0.321940f, -0.932615f),
  float2(-0.791559f, -0.597705f)
};

float saturate(float x)
{
  return clamp(x, 0.0f, 1.0f);
}

[numthreads(16, 16, 1)]
void cs_main(uint3 id: SV_DispatchThreadID)
{
  if(id.x >= push.width || id.y >= push.height)
    return;

  int2 pix = int2(id.xy);
  float2 uv = (float2(pix) + 0.5f) / float2(push.width, push.height);

  Texture2D src       = textures[push.src_texture_id];
  Texture2D dof_tex   = textures[push.dof_texture_id];
  Texture2D bloom_tex = textures[push.bloom_texture_id];
  RWTexture2D<float4> dst = storage_images[push.output_image_id];

  float3 source = src.Load(int3(pix, 0)).rgb;
  float3 color  = source;
  float  avg_sample_size = 0.0f;

  if(push.dof_enabled != 0)
  {
    float2 half_uv = (float2(pix) + 0.5f) / float2(push.width, push.height);
    float4 center  = dof_tex.SampleLevel(samplers[push.sampler_id], half_uv, 0.0f);

    float max_blur = max(push.dof_max_blur_size, 1e-4f);
    float center_size = center.a * max_blur;

    if(center_size > 0.05f)
    {
      float2 half_pixel = 1.0f / float2(max(push.half_width, 1u), max(push.half_height, 1u));
      float radius_scale = max(push.dof_rad_scale, 1e-4f);
      float2 blur_scale = half_pixel * min(center_size, max_blur) * radius_scale;

      float3 accum = center.rgb;
      float total  = 1.0f;
      float sum_size = center_size;
      float sample_count = 1.0f;

      [unroll]
      for(int i = 0; i < DOF_TAP_COUNT; ++i)
      {
        float2 tc = half_uv + DOF_POISSON[i] * blur_scale;
        float4 sample = dof_tex.SampleLevel(samplers[push.sampler_id], tc, 0.0f);
        float sample_size = sample.a * max_blur;

        float clamped_size = min(sample_size, center_size * 2.0f);
        float m = saturate(clamped_size / max(center_size, 1e-4f));
        accum += sample.rgb * m;
        total += m;

        sum_size += sample_size;
        sample_count += 1.0f;
      }

      float3 blurred = accum / max(total, 1.0f);
      avg_sample_size = sum_size / max(sample_count, 1.0f);

      float blur_mix = smoothstep(max_blur * 0.15f, max_blur * 0.6f, avg_sample_size);
      color = lerp(source, blurred, blur_mix);
    }
  }

  float3 bloom = bloom_tex.SampleLevel(samplers[push.sampler_id], uv, 0.0f).rgb;
  color += bloom * push.bloom_intensity;
  color *= exp2(push.exposure);
  color = tonemapACES(color);

  float alpha_out = saturate(avg_sample_size / max(push.dof_max_blur_size, 1e-4f));
  dst[pix] = float4(color, alpha_out);
}
