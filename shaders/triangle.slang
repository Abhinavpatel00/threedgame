#ifndef DEBUG_PRINTF
#define DEBUG_PRINTF 0
#endif

struct PackedFace
{
  uint data0;
  uint data1;
};

static const uint VOXEL_CHUNK_SIZE = 128;


struct Push
{
    uint64_t face_ptr;
    uint     face_count;
    float    aspect;
    float3   cam_pos;    // camera world position
    uint     pad1;       // alignment
    float3   cam_dir;    // camera forward (normalized)
    uint     pad2;
    float4x4 view_proj;
    uint     texture_id;
    uint     sampler_id;
};
// struct VSOutput
// {
//   float4               position : SV_Position;
//   float2               uv : TEXCOORD0;
//   nointerpolation uint tex_id : TEXCOORD1;
//   nointerpolation uint face : TEXCOORD3;
//   float3               world_pos : TEXCOORD2;
// };

[[vk::binding(0, 0)]]
Texture2D textures[];

[[vk::binding(1, 0)]]
SamplerState samplers[];

[[vk::push_constant]]
Push push;
//
// static const uint3 offsets[6][4] = {
//   { uint3(1, 0, 0), uint3(1, 1, 0), uint3(1, 1, 1), uint3(1, 0, 1) },  // +X
//   { uint3(0, 0, 0), uint3(0, 0, 1), uint3(0, 1, 1), uint3(0, 1, 0) },  // -X
//   { uint3(0, 1, 0), uint3(0, 1, 1), uint3(1, 1, 1), uint3(1, 1, 0) },  // +Y
//   { uint3(0, 0, 0), uint3(1, 0, 0), uint3(1, 0, 1), uint3(0, 0, 1) },  // -Y
//   { uint3(0, 0, 1), uint3(1, 0, 1), uint3(1, 1, 1), uint3(0, 1, 1) },  // +Z
//   { uint3(0, 0, 0), uint3(0, 1, 0), uint3(1, 1, 0), uint3(1, 0, 0) }   // -Z
// };
//
// [shader("vertex")]
// VSOutput vs_main(uint vertexID: SV_VertexID)
// {
//   VSOutput o;
//
//   static const uint corner_lut[6] = { 0, 1, 2, 0, 2, 3 };
//
//   uint faceID   = vertexID / 6;
//   uint cornerID = corner_lut[vertexID % 6];
//
//   if(faceID >= push.face_count)
//   {
//     o.position = float4(0.0, 0.0, 0.0, 0.0);
//     o.uv       = float2(0.0, 0.0);
//     o.tex_id   = 0;
//     return o;
//   }
//
//   PackedFace* faces = (PackedFace*)push.face_ptr;
//   PackedFace  f     = faces[faceID];
//
//   uint x    = (f.data0) & 255;
//   uint y    = (f.data0 >> 8) & 255;
//   uint z    = (f.data0 >> 16) & 4095;
//   uint face = (f.data0 >> 28) & 7;
//
//   uint tex_id  = f.data1 & 0xFFFF;
//   int  chunk_x = int((f.data1 >> 16) & 0xFF) - 128;
//   int  chunk_z = int((f.data1 >> 24) & 0xFF) - 128;
//
//   uint3 base = uint3(x, y, z);
//
//   uint3 offset = offsets[face][cornerID];
//
//   int3   chunk_base = int3(chunk_x * int(VOXEL_CHUNK_SIZE), 0, chunk_z * int(VOXEL_CHUNK_SIZE));
//   float3 world      = float3(int3(base + offset) + chunk_base);
//
//   o.position = mul(push.view_proj, float4(world, 1));
//
//   float2 uv = cornerID == 0 ? float2(0, 0) : cornerID == 1 ? float2(1, 0) : cornerID == 2 ? float2(1, 1) : float2(0, 1);
//   o.world_pos = world;
//   o.face      = face;
//
//
//   o.uv     = uv;
//   o.tex_id = tex_id;
//
// #if DEBUG_PRINTF
//   if(vertexID < 6)
//   {
//     printf("voxel vs: vid=%u face=%u corner=%u xyz=(%u,%u,%u) tex=%u\n", vertexID, faceID, cornerID, x, y, z, o.tex_id);
//   }
// #endif
//
//   return o;
// }
//
// [shader("fragment")]
// float4 fs_main(VSOutput input) : SV_Target0
// {
//     Texture2D    tex  = textures[NonUniformResourceIndex(input.tex_id)];
//     SamplerState samp = samplers[push.sampler_id];
//
//     float3 albedo = tex.Sample(samp, input.uv).rgb;
//
//     // --- Torch: light follows camera ---
//     float3 light_pos   = push.cam_pos;               // ← was hardcoded
//     float3 light_dir   = normalize(push.cam_dir);     // ← was hardcoded
//
//     float3 light_color = float3(1.0, 0.85, 0.5);     // warm torch color
//     float  light_radius = 30.0;
//     float  innerCone    = 0.97;                       // tight inner beam
//     float  outerCone    = 0.85;
//
//     // Face normals
//     float3 normal =
//         input.face == 0 ? float3( 1, 0, 0) :
//         input.face == 1 ? float3(-1, 0, 0) :
//         input.face == 2 ? float3( 0, 1, 0) :
//         input.face == 3 ? float3( 0,-1, 0) :
//         input.face == 4 ? float3( 0, 0, 1) :
//                           float3( 0, 0,-1);
//
//     // Light vector
//     float3 L    = light_pos - input.world_pos;
//     float  dist = length(L);
//     L /= dist;
//
//     // Attenuation
//     float attenuation = saturate(1.0 - (dist / light_radius));
//     attenuation *= attenuation;
//
//     // Diffuse
//     float NdotL = saturate(dot(normal, L));
//
//     // Spotlight cone
//     float theta   = dot(L, -light_dir);
//     float epsilon = innerCone - outerCone;
//     float cone    = saturate((theta - outerCone) / epsilon);
//
//     // Final
//     float3 ambient = albedo * 0.04;
//     float3 diffuse = albedo * light_color * NdotL * attenuation * cone * 5.0;
//
//     return float4(ambient + diffuse, 1.0);
// }
//
//
struct VSOutput
{
    float4 position : SV_Position;
};

[shader("vertex")]
VSOutput vs_main(uint vertexID : SV_VertexID)
{
    VSOutput o;

  float3 verts[3] =
    {
        float3( 0.0, 0.0, 0.0),
        float3( 1.0, 0.0, 0.0),
        float3( 0.0, 1.0, 0.0)
    };

    float3 world = verts[vertexID];

    o.position = mul(push.view_proj, float4(world,1.0));
    return o;
}

[shader("fragment")]
float4 fs_main() : SV_Target0
{
    return float4(1,0,0,1);
}
