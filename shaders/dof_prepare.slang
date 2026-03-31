#include "common_bindless.slang"

struct DofPreparePush
{
  uint src_texture_id;
  uint depth_texture_id;
  uint output_image_id;
  uint sampler_id;

  uint width;
  uint height;
  uint out_width;
  uint out_height;

  uint  dof_enabled;
  float dof_focus_point;
  float dof_focus_scale;
  float dof_far_plane;
};

[[vk::push_constant]]
DofPreparePush push;

static const float MAX_BLUR_SIZE = 20.0f;

float saturate(float x)
{
  return clamp(x, 0.0f, 1.0f);
}

int2 clamp_fullres(int2 p)
{
  int2 max_p = int2((int)push.width - 1, (int)push.height - 1);
  return clamp(p, int2(0, 0), max_p);
}

float compute_blur_size(float depth_linear)
{
  float d = max(depth_linear, 1e-4f);
  float coc = (1.0f / max(push.dof_focus_point, 1e-4f) - 1.0f / d) * push.dof_focus_scale;
  coc = clamp(coc, -1.0f, 1.0f);
  return abs(coc) * MAX_BLUR_SIZE;
}

[numthreads(16, 16, 1)]
void cs_main(uint3 id: SV_DispatchThreadID)
{
  if(id.x >= push.out_width || id.y >= push.out_height)
    return;

  RWTexture2D<float4> dst = storage_images[push.output_image_id];
  if(push.dof_enabled == 0)
  {
    float2 uv = (float2(id.xy) + 0.5f) / float2(push.out_width, push.out_height);
    float3 col = textures[push.src_texture_id].SampleLevel(samplers[push.sampler_id], uv, 0.0f).rgb;
    dst[int2(id.xy)] = float4(col, 0.0f);
    return;
  }

  Texture2D src = textures[push.src_texture_id];
  Texture2D depth_tex = textures[push.depth_texture_id];

  int2 base = int2(id.xy) * 2;
  int2 p00 = clamp_fullres(base + int2(0, 0));
  int2 p10 = clamp_fullres(base + int2(1, 0));
  int2 p01 = clamp_fullres(base + int2(0, 1));
  int2 p11 = clamp_fullres(base + int2(1, 1));

  float3 c00 = src.Load(int3(p00, 0)).rgb;
  float3 c10 = src.Load(int3(p10, 0)).rgb;
  float3 c01 = src.Load(int3(p01, 0)).rgb;
  float3 c11 = src.Load(int3(p11, 0)).rgb;
  float3 down_col = (c00 + c10 + c01 + c11) * 0.25f;

  float d00 = depth_tex.Load(int3(p00, 0)).r * push.dof_far_plane;
  float d10 = depth_tex.Load(int3(p10, 0)).r * push.dof_far_plane;
  float d01 = depth_tex.Load(int3(p01, 0)).r * push.dof_far_plane;
  float d11 = depth_tex.Load(int3(p11, 0)).r * push.dof_far_plane;

  float b00 = compute_blur_size(d00);
  float b10 = compute_blur_size(d10);
  float b01 = compute_blur_size(d01);
  float b11 = compute_blur_size(d11);
  float blur_size = (b00 + b10 + b01 + b11) * 0.25f;
  float coc_norm = saturate(blur_size / MAX_BLUR_SIZE);

  dst[int2(id.xy)] = float4(down_col, coc_norm);
}
