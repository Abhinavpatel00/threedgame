struct Push
{
    uint64_t pos_ptr;
    uint64_t idx_ptr;
    uint64_t nrm_ptr;
    uint64_t uv_ptr;

    float4x4 model;
    float4x4 view_proj;

    float4 base_color_factor;
    float3 emissive_factor;
    float metallic_factor;
    float roughness_factor;

    uint base_color_tex;
    uint normal_tex;
    uint mr_tex;
    uint emissive_tex;
    uint sampler_id;
    uint flags;
    uint index_count;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 world_pos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
};

[[vk::binding(0, 0)]]
Texture2D textures[];

[[vk::binding(1, 0)]]
SamplerState samplers[];

[[vk::push_constant]]
Push push;

[shader("vertex")]
VSOutput vs_main(uint vertexID : SV_VertexID)
{
    VSOutput o;

    uint* indices = (uint*)push.idx_ptr;
    float* positions = (float*)push.pos_ptr;

    uint idx = indices[vertexID];

    float3 p = float3(
        positions[idx * 3 + 0],
        positions[idx * 3 + 1],
        positions[idx * 3 + 2]
    );

    float3 n = float3(0.0, 1.0, 0.0);
    if((push.flags & 0x2) != 0)
    {
        float* normals = (float*)push.nrm_ptr;
        n = float3(
            normals[idx * 3 + 0],
            normals[idx * 3 + 1],
            normals[idx * 3 + 2]
        );
    }

    float2 uv = float2(0.0, 0.0);
    if((push.flags & 0x1) != 0)
    {
        float* uvs = (float*)push.uv_ptr;
        uv = float2(
            uvs[idx * 2 + 0],
            uvs[idx * 2 + 1]
        );
    }

    float4 world = mul(push.model, float4(p, 1.0));
    float3x3 nrm_mat = (float3x3)push.model;

    o.position = mul(push.view_proj, world);
    o.world_pos = world.xyz;
    o.normal = normalize(mul(nrm_mat, n));
    o.uv = uv;

    return o;
}

[shader("fragment")]
float4 fs_main(VSOutput i) : SV_Target0
{
    SamplerState samp = samplers[push.sampler_id];

    float4 base_color = push.base_color_factor;
    if(push.base_color_tex != 0xFFFFFFFF)
    {
        base_color *= textures[push.base_color_tex].Sample(samp, i.uv);
    }

    float3 normal = normalize(i.normal);
    float3 L = normalize(float3(0.5, 0.9, 0.25));
    float3 V = normalize(-i.world_pos);
    float3 H = normalize(L + V);

    float ndotl = max(dot(normal, L), 0.0);
    float ndoth = max(dot(normal, H), 0.0);

    float rough = clamp(push.roughness_factor, 0.04, 1.0);
    float metal = clamp(push.metallic_factor, 0.0, 1.0);

    float diffuse_term = ndotl;
    float spec_power = lerp(64.0, 4.0, rough);
    float spec = pow(ndoth, spec_power) * ndotl;

    float3 albedo = base_color.rgb;
    float3 diffuse = albedo * (1.0 - metal) * diffuse_term;
    float3 specular = lerp(float3(0.04, 0.04, 0.04), albedo, metal) * spec;

    float3 emissive = push.emissive_factor;
    if(push.emissive_tex != 0xFFFFFFFF)
        emissive *= textures[push.emissive_tex].Sample(samp, i.uv).rgb;

    float3 color = diffuse + specular + emissive;
    return float4(color, base_color.a);
}
