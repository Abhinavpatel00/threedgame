import tonemapping;

struct PostPush
{
  uint src_texture_id;
  uint output_image_id;
  uint depth_texture_id;
  uint sampler_id;

  uint  bloom_texture_id;
  float bloom_intensity;

  uint width;
  uint height;

  uint  frame;
  float exposure;
  uint  dof_enabled;
  float dof_focus_depth;
  float dof_focus_range;
  float dof_max_blur_radius;
};

#define SLANG_DEFAULT(x) = (x)
__intrinsic_op(cmpGT) public vector<bool, N> greaterThan<T, let N : int>(vector<T, N> x, vector<T, N> y);


inline float3 toSrgb(float3 rgb)
{
  float3 low  = rgb * 12.92f;
  float3 high = fma(pow(rgb, float3(1.0F / 2.4F)), float3(1.055F), float3(-0.055F));
  return lerp(low, high, float3(greaterThan(rgb, float3(0.0031308F))));
}

// Converts a color from sRGB to linear RGB.
inline float3 toLinear(float3 srgb)
{
  float3 low  = srgb / 12.92F;
  float3 high = pow((srgb + float3(0.055F)) / 1.055F, float3(2.4F));
  return lerp(low, high, float3(greaterThan(srgb, float3(0.04045F))));
}

/*-------------------------------------------------------------------------------------------------
# Function `tonemapACES`
> An approximation by Stephen Hill to the Academy Color Encoding System's
> filmic curve for displaying HDR images on LDR output devices.

From https://github.com/TheRealMJP/BakingLab/blob/master/BakingLab/ACES.hlsl,
via https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
-------------------------------------------------------------------------------------------------*/
inline float3 tonemapACES(float3 color)
{
  // Input transform
  const float3x3 ACESInputMat = float3x3(0.59719F, 0.07600F, 0.02840F,   // Row 1
                                         0.35458F, 0.90834F, 0.13383F,   // Row 2
                                         0.04823F, 0.01566F, 0.83777F);  // Row 3
  color                       = mul(color, ACESInputMat);

  // RRT and ODT fit
  float3 a = color * (color + float3(0.0245786F)) - float3(0.000090537F);
  float3 b = color * (float3(0.983729F) * color + float3(0.4329510F)) + float3(0.238081F);
  color    = a / b;  // Always OK because of the large constant term in b's polynomial
  // Output transform
  const float3x3 ACESOutputMat = float3x3(1.60475F, -0.10208F, -0.00327F,  //
                                          -0.53108F, 1.10813F, -0.07276F,  //
                                          -0.07367F, -0.00605F, 1.07602F);
  color                        = mul(color, ACESOutputMat);
  return toLinear(color);
}


[[vk::push_constant]]
PostPush push;

[[vk::binding(0, 0)]]
Texture2D textures[];

[[vk::binding(1, 0)]]
SamplerState samplers[];

[[vk::binding(2, 0)]]
RWTexture2D<float4> storage_images[];

float saturate(float x)
{
  return clamp(x, 0.0f, 1.0f);
}

float3 load_clamped(Texture2D tex, int2 p)
{
  int2 max_p = int2((int)push.width - 1, (int)push.height - 1);
  int2 cp    = clamp(p, int2(0, 0), max_p);
  return tex.Load(int3(cp, 0)).rgb;
}

float load_depth_clamped(Texture2D tex, int2 p)
{
  int2 max_p = int2((int)push.width - 1, (int)push.height - 1);
  int2 cp    = clamp(p, int2(0, 0), max_p);
  return tex.Load(int3(cp, 0)).r;
}

float compute_coc(float depth_raw)
{
  float coc = abs(depth_raw - push.dof_focus_depth) / max(push.dof_focus_range, 1e-5f);
  coc       = saturate(coc);
  coc       = coc * coc;
  return coc;
}

float3 apply_dof(Texture2D src, Texture2D depth_tex, int2 pix)
{
  float3 center = load_clamped(src, pix);
  if(push.dof_enabled == 0)
    return center;

  float center_depth = load_depth_clamped(depth_tex, pix);
  float center_coc   = compute_coc(center_depth);
  float radius       = center_coc * push.dof_max_blur_radius;
  if(radius < 0.75f)
    return center;

  static const int TAP_COUNT = 16;
  static const float2 POISSON[TAP_COUNT] = {
    float2(-0.6134f,  0.6175f), float2( 0.1700f, -0.0402f), float2(-0.2994f, -0.4217f), float2( 0.6457f,  0.4932f),
    float2(-0.0254f, -0.7689f), float2( 0.4812f, -0.5418f), float2(-0.7682f, -0.1489f), float2( 0.7705f, -0.2136f),
    float2( 0.0313f,  0.8656f), float2(-0.4663f,  0.1832f), float2( 0.3191f,  0.2897f), float2(-0.1960f, -0.1638f),
    float2( 0.0740f, -0.3245f), float2( 0.4021f,  0.0172f), float2(-0.1207f,  0.4208f), float2(-0.4098f, -0.6490f)
  };

  float3 sum  = center;
  float  wsum = 1.0f;

  [unroll]
  for(int i = 0; i < TAP_COUNT; ++i)
  {
    float2 ofs = POISSON[i] * radius;
    int2   sp  = pix + int2(round(ofs.x), round(ofs.y));

    float3 sample_col   = load_clamped(src, sp);
    float  sample_depth = load_depth_clamped(depth_tex, sp);
    float  sample_coc   = compute_coc(sample_depth);

    float depth_diff       = abs(sample_depth - center_depth);
    float depth_similarity = exp(-depth_diff * 300.0f);
    float coc_weight       = lerp(0.25f, 1.0f, max(center_coc, sample_coc));
    float w                = depth_similarity * coc_weight;

    sum += sample_col * w;
    wsum += w;
  }

  float3 blurred  = sum / max(wsum, 1.0f);
  float  blur_mix = smoothstep(0.05f, 1.0f, center_coc);
  return lerp(center, blurred, blur_mix);
}

[numthreads(16, 16, 1)]
void cs_main(uint3 id: SV_DispatchThreadID)
{
  if(id.x >= push.width || id.y >= push.height)
    return;

  int2 pix = int2(id.xy);

  Texture2D           src = textures[push.src_texture_id];
  Texture2D           depth_tex = textures[push.depth_texture_id];
  Texture2D           bloom_tex = textures[push.bloom_texture_id];
  RWTexture2D<float4> dst = storage_images[push.output_image_id];

float3 color = apply_dof(src, depth_tex, pix);

float2 uv = (float2(pix) + 0.5f) / float2(push.width, push.height);
float3 bloom = bloom_tex.SampleLevel(samplers[push.sampler_id], uv, 0.0f).rgb;
color += bloom * push.bloom_intensity;

// exposure control
color *= exp2(push.exposure);

color = tonemapACES(color);


//

  dst[pix] = float4(color, 1.0);
}

