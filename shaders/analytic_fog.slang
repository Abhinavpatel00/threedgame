#include "common_bindless.slang"

struct AnalyticFogPush
{
  uint src_texture_id;
  uint depth_texture_id;
  uint output_image_id;
  uint sampler_id;

  uint width;
  uint height;
  uint fog_enabled;
  uint pad0;

  float density_scale;
  float fog_radius;
  float noise_amount;
  float pad1;

  float3 fog_color;
  float pad2;

  float3 fog_center;
  float pad3;
};

[[vk::push_constant]]
AnalyticFogPush push;

float saturate(float x)
{
  return clamp(x, 0.0f, 1.0f);
}

float hash12(float2 p)
{
  float h = dot(p, float2(127.1f, 311.7f));
  return frac(sin(h) * 43758.5453123f);
}

float antiderivative_quartic(float a, float b, float c, float t)
{
  float a2 = a * a;
  float b2 = b * b;

  float term5 = (a2 * t * t * t * t * t) / 5.0f;
  float term4 = (a * b) * t * t * t * t;
  float term3 = ((4.0f * b2 + 2.0f * a * c - 2.0f * a) * t * t * t) / 3.0f;
  float term2 = 2.0f * (b * c - b) * t * t;
  float term1 = (c * c - 2.0f * c + 1.0f) * t;

  return term5 + term4 + term3 + term2 + term1;
}

float primitive_tau(float3 ray_origin, float3 ray_dir, float scene_t, float3 center, float radius)
{
  if(radius <= 1e-4f)
    return 0.0f;

  float3 oc = ray_origin - center;
  float qa = dot(ray_dir, ray_dir);
  float qb = dot(oc, ray_dir);
  float qc = dot(oc, oc) - radius * radius;

  float disc = qb * qb - qa * qc;
  if(disc <= 0.0f)
    return 0.0f;

  float root_disc = sqrt(disc);
  float t0 = (-qb - root_disc) / qa;
  float t1 = (-qb + root_disc) / qa;

  float entry_t = max(0.0f, t0);
  float exit_t = min(scene_t, t1);

  if(exit_t <= entry_t)
    return 0.0f;

  float inv_radius = 1.0f / radius;
  float3 ro_local = (ray_origin - center) * inv_radius;
  float3 rd_local = ray_dir * inv_radius;

  float a = dot(rd_local, rd_local);
  float b = dot(rd_local, ro_local);
  float c = dot(ro_local, ro_local);

  float tau = antiderivative_quartic(a, b, c, exit_t) - antiderivative_quartic(a, b, c, entry_t);
  return max(tau, 0.0f);
}

float3 reconstruct_world_pos(float2 uv, float depth)
{
  float2 ndc_xy = uv * 2.0f - 1.0f;
  float4 clip = float4(ndc_xy, depth, 1.0f);
  float4 world_h = mul(global_ubo.inv_viewproj, clip);
  return world_h.xyz / max(world_h.w, 1e-6f);
}

[numthreads(16, 16, 1)]
void cs_main(uint3 id: SV_DispatchThreadID)
{
  if(id.x >= push.width || id.y >= push.height)
    return;

  int2 pix = int2(id.xy);

  Texture2D src_tex = textures[push.src_texture_id];
  Texture2D depth_tex = textures[push.depth_texture_id];
  RWTexture2D<float4> dst = storage_images[push.output_image_id];

  float3 source = src_tex.Load(int3(pix, 0)).rgb;

  if(push.fog_enabled == 0)
  {
    dst[pix] = float4(source, 1.0f);
    return;
  }

  float depth = depth_tex.Load(int3(pix, 0)).r;
  if(depth <= 1e-6f)
  {
    dst[pix] = float4(source, 1.0f);
    return;
  }

  float2 uv = (float2(pix) + 0.5f) / float2(push.width, push.height);
  float3 camera_pos = global_ubo.camera_pos.xyz;
  float3 world_pos = reconstruct_world_pos(uv, depth);

  float3 ray_to_point = world_pos - camera_pos;
  float scene_t = length(ray_to_point);
  if(scene_t <= 1e-6f)
  {
    dst[pix] = float4(source, 1.0f);
    return;
  }

  float3 ray_dir = ray_to_point / scene_t;

  float radius0 = push.fog_radius;
  float radius1 = push.fog_radius * 0.65f;
  float radius2 = push.fog_radius * 0.50f;

  float wobble1 = sin(global_ubo.time * 0.37f) * (0.35f * push.fog_radius);
  float wobble2 = cos(global_ubo.time * 0.21f) * (0.25f * push.fog_radius);

  float3 center0 = push.fog_center;
  float3 center1 = push.fog_center + float3(push.fog_radius * 0.55f, wobble1, -push.fog_radius * 0.45f);
  float3 center2 = push.fog_center + float3(-push.fog_radius * 0.75f, wobble2, push.fog_radius * 0.35f);

  float tau = 0.0f;
  tau += primitive_tau(camera_pos, ray_dir, scene_t, center0, radius0) * 1.00f;
  tau += primitive_tau(camera_pos, ray_dir, scene_t, center1, radius1) * 0.80f;
  tau += primitive_tau(camera_pos, ray_dir, scene_t, center2, radius2) * 0.65f;

  tau *= max(push.density_scale, 0.0f);

  if(push.noise_amount > 0.0f)
  {
    float noise = hash12(float2(pix) + float(global_ubo.frame_count) * 0.123f);
    float noise_scale = 1.0f + (noise * 2.0f - 1.0f) * push.noise_amount;
    tau *= max(noise_scale, 0.0f);
  }

  float transmittance = exp(-max(tau, 0.0f));
  float3 fogged = source * transmittance + push.fog_color * (1.0f - transmittance);

  dst[pix] = float4(fogged, 1.0f);
}
