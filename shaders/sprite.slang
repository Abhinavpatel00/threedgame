struct Push
{
    uint64_t instance_ptr;
    float2   screen_size;
    uint     sampler_id;
    float4x4 view_proj;
};

struct VSOutput
{
    float4 position   : SV_Position;
    float2 uv         : TEXCOORD0;
    float4 tint       : TEXCOORD1;
    float  opacity    : TEXCOORD2;
    uint   texture_id : TEXCOORD3;
};

[[vk::binding(0, 0)]]
Texture2D textures[];

[[vk::binding(1, 0)]]
SamplerState samplers[];

[[vk::push_constant]]
Push push;

uint load_u32(uint instance_index, uint dword_index)
{
    uint* base_ptr = (uint*)push.instance_ptr;
    return base_ptr[instance_index * 16 + dword_index];
}

float load_f32(uint instance_index, uint dword_index)
{
    return asfloat(load_u32(instance_index, dword_index));
}

[shader("vertex")]
VSOutput vs_main(uint vertexID : SV_VertexID,
                 uint instanceID : SV_InstanceID)
{
    VSOutput output;

    float2 position  = float2(load_f32(instanceID, 0), load_f32(instanceID, 1));
    float2 scale     = float2(load_f32(instanceID, 2), load_f32(instanceID, 3));
    float  rotation  = load_f32(instanceID, 4);
    float  depth     = load_f32(instanceID, 5);
    float  opacity   = load_f32(instanceID, 6);
    uint   textureID = load_u32(instanceID, 7);
    float4 uv_rect   = float4(load_f32(instanceID, 8), load_f32(instanceID, 9), load_f32(instanceID, 10), load_f32(instanceID, 11));
    float4 tint      = float4(load_f32(instanceID, 12), load_f32(instanceID, 13), load_f32(instanceID, 14), load_f32(instanceID, 15));

    uint indices[6] = {0, 1, 2, 0, 2, 3};
    float2 corners[4] =
    {
        float2(0.0, 0.0),
        float2(1.0, 0.0),
        float2(1.0, 1.0),
        float2(0.0, 1.0)
    };

    float2 local_uv = corners[indices[vertexID]];

    float2 centered = (local_uv - 0.5) * scale;
    float s = sin(rotation);
    float c = cos(rotation);
    float2 rotated = float2(centered.x * c - centered.y * s, centered.x * s + centered.y * c);

    float2 world = position + rotated;

float4 world_pos = float4(world, depth, 1.0);
output.position = mul(push.view_proj, world_pos);
    output.uv = lerp(uv_rect.xy, uv_rect.zw, local_uv);
    output.tint = tint;
    output.opacity = opacity;
    output.texture_id = textureID;

    return output;
}

[shader("fragment")]
float4 fs_main(VSOutput input) : SV_Target0
{
    Texture2D tex = textures[input.texture_id];
    SamplerState samp = samplers[push.sampler_id];

    float4 sampled = tex.Sample(samp, input.uv);
    float4 color = sampled * input.tint;
    color.a *= input.opacity;

    return color;
}
