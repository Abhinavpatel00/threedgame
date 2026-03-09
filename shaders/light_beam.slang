struct LightBeam
{
  float4 pos_width;
  float4 sun_height;
  float4 misc;
};

struct Push
{
  uint64_t beam_ptr;
 
  uint64_t pad;
 float4x4 view_proj;
};

[[vk::push_constant]]
Push push;


// device-address buffer
// vertex → fragment data
struct VSOutput
{
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
  float  opacity : TEXCOORD1;
};


// quad generated in shader
static const float2 quad[6] = { float2(-1, -1), float2(1, -1), float2(1, 1),
                                float2(-1, -1), float2(1, 1),  float2(-1, 1) };
[shader("vertex")]
VSOutput vs_main(uint vertexID: SV_VertexID, uint instanceID: SV_InstanceID)
{
  VSOutput o;

  let       beams = (LightBeam*)push.beam_ptr;
  LightBeam beam  = beams[instanceID];

  float2 q = quad[vertexID];

  float3 pos   = beam.pos_width.xyz;
  float  width = beam.pos_width.w;

  float3 sun    = normalize(beam.sun_height.xyz);
  float  height = beam.sun_height.w;

  // extract camera right vector from view-projection
  float3 camRight = normalize(float3(push.view_proj[0][0], push.view_proj[1][0], push.view_proj[2][0]));

  // beam vertical axis follows sunlight
  float3 up = sun;

  // construct billboard plane
  float3 worldPos = pos + camRight * q.x * width + up * q.y * height;

  o.pos = mul(push.view_proj, float4(worldPos, 1));

  o.uv      = q * 0.5 + 0.5;
  o.opacity = beam.misc.x;

  return o;
}

// --------------------

// --------------------
// Fragment Shader
// --------------------
[shader("fragment")]
float4 fs_main(VSOutput input)
    : SV_Target0
{

  // distance from beam center
  float x = abs(input.uv.x * 2.0 - 1.0);
  float  core      = exp(-x * x * 8.0);
  float  halo      = 1.0 - smoothstep(0.0, 1.0, x);
  float  vertical  = pow(saturate(input.uv.y), 0.5);
  float  alpha     = (core * 0.8 + halo * 0.2) * vertical * input.opacity;
  float3 warmGold  = float3(1.0, 0.85, 0.45);
  float3 whiteCore = float3(1.0, 0.97, 0.88);
  float3 color     = lerp(warmGold, whiteCore, core);
  return float4(color, alpha);
}


