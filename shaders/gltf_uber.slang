struct Push
{
    uint64_t draw_data_ptr;
    uint64_t skin_mats_ptr;
    uint64_t mat_ptr;
    uint _pad0;

    float4x4 view_proj;
};

struct DrawData
{
    uint64_t pos_ptr;
    uint64_t idx_ptr;
    uint64_t nrm_ptr;
    uint64_t uv_ptr;
    uint64_t joints_ptr;
    uint64_t weights_ptr;
    float4x4 model;
    uint material_id;
    uint skin_offset;
    uint flags;
    uint _pad0;
};

struct GPUMaterial
{
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
    uint _pad0;
    uint _pad1;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 world_pos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    nointerpolation uint material_id : TEXCOORD3;
};

[[vk::binding(0, 0)]]
Texture2D textures[];

[[vk::binding(1, 0)]]
SamplerState samplers[];

[[vk::push_constant]]
Push push;

[shader("vertex")]
VSOutput vs_main(uint vertexID : SV_VertexID, uint drawID : SV_DrawIndex)
{
    VSOutput o;

    DrawData* draws = (DrawData*)push.draw_data_ptr;
    DrawData draw = draws[drawID];

    uint* indices = (uint*)draw.idx_ptr;
    float* positions = (float*)draw.pos_ptr;

    uint idx = indices[vertexID];

    float3 p = float3(
        positions[idx * 3 + 0],
        positions[idx * 3 + 1],
        positions[idx * 3 + 2]
    );

    float3 n = float3(0.0, 1.0, 0.0);
    if((draw.flags & 0x2) != 0)
    {
        float* normals = (float*)draw.nrm_ptr;
        n = float3(
            normals[idx * 3 + 0],
            normals[idx * 3 + 1],
            normals[idx * 3 + 2]
        );
    }

    float2 uv = float2(0.0, 0.0);
    if((draw.flags & 0x1) != 0)
    {
        float* uvs = (float*)draw.uv_ptr;
        uv = float2(
            uvs[idx * 2 + 0],
            uvs[idx * 2 + 1]
        );
    }

    float4 world = mul(draw.model, float4(p, 1.0));
    float3 world_n = n;

    if((draw.flags & 0x4) != 0)
    {
        float* joints_f = (float*)draw.joints_ptr;
        float* weights = (float*)draw.weights_ptr;
        float4x4* skin_mats = (float4x4*)push.skin_mats_ptr;

        uint4 j = uint4(
            uint(joints_f[idx * 4 + 0]),
            uint(joints_f[idx * 4 + 1]),
            uint(joints_f[idx * 4 + 2]),
            uint(joints_f[idx * 4 + 3])
        );

        float4 w = float4(
            weights[idx * 4 + 0],
            weights[idx * 4 + 1],
            weights[idx * 4 + 2],
            weights[idx * 4 + 3]
        );

        float wsum = max(dot(w, 1.0), 1e-6);
        w /= wsum;

        uint base = draw.skin_offset;
        float4 skinned_pos =
            mul(skin_mats[base + j.x], float4(p, 1.0)) * w.x +
            mul(skin_mats[base + j.y], float4(p, 1.0)) * w.y +
            mul(skin_mats[base + j.z], float4(p, 1.0)) * w.z +
            mul(skin_mats[base + j.w], float4(p, 1.0)) * w.w;

        float3 skinned_nrm =
            mul((float3x3)skin_mats[base + j.x], n) * w.x +
            mul((float3x3)skin_mats[base + j.y], n) * w.y +
            mul((float3x3)skin_mats[base + j.z], n) * w.z +
            mul((float3x3)skin_mats[base + j.w], n) * w.w;

        world = mul(draw.model, skinned_pos);
        world_n = normalize(mul((float3x3)draw.model, skinned_nrm));
    }

    float3x3 nrm_mat = (float3x3)draw.model;

    o.position = mul(push.view_proj, world);
    o.world_pos = world.xyz;
    o.normal = ((draw.flags & 0x4) != 0) ? world_n : normalize(mul(nrm_mat, n));
    o.uv = uv;
    o.material_id = draw.material_id;

    return o;
}

[shader("fragment")]
float4 fs_main(VSOutput i) : SV_Target0
{
    GPUMaterial* materials = (GPUMaterial*)push.mat_ptr;
    GPUMaterial mat = materials[i.material_id];

    uint sampler_id = mat.sampler_id;
    if(sampler_id == 0xFFFFFFFF)
        sampler_id = 0;

    SamplerState samp = samplers[sampler_id];

    float4 base_color = mat.base_color_factor;
    if(mat.base_color_tex != 0xFFFFFFFF)
    {
        base_color *= textures[mat.base_color_tex].Sample(samp, i.uv);
    }

    float3 normal = normalize(i.normal);
    float3 L = normalize(float3(0.5, 0.9, 0.25));
    float3 V = normalize(-i.world_pos);
    float3 H = normalize(L + V);

    float ndotl = max(dot(normal, L), 0.0);
    float ndoth = max(dot(normal, H), 0.0);

    float rough = clamp(mat.roughness_factor, 0.04, 1.0);
    float metal = clamp(mat.metallic_factor, 0.0, 1.0);

    float diffuse_term = ndotl * 0.5 + 0.5;
    float spec_power = lerp(64.0, 4.0, rough);
    float spec = pow(ndoth, spec_power) * ndotl;
    float ambient = 0.14;

    float3 albedo = base_color.rgb;
    float3 diffuse = albedo * (1.0 - metal) * (diffuse_term + ambient);
    float3 specular = lerp(float3(0.04, 0.04, 0.04), albedo, metal) * spec;
    float hemi = normal.y * 0.5 + 0.5;
    float3 fill = albedo * lerp(0.12, 0.28, hemi);

    float3 emissive = mat.emissive_factor;
    if(mat.emissive_tex != 0xFFFFFFFF)
        emissive *= textures[mat.emissive_tex].Sample(samp, i.uv).rgb;

    float3 color = diffuse + specular + fill + emissive;
    return float4(color, base_color.a);
}
