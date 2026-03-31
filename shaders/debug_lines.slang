#include "common_bindless.slang"

struct Push
{
    uint64_t pos_ptr;
    uint64_t idx_ptr;

    float4x4 model;
    float4 color;

    uint index_count;
    uint _pad0;
    uint _pad1;
    uint _pad2;
};

struct VSOutput
{
    float4 position : SV_Position;
    float4 color : TEXCOORD0;
};

struct FSOutput
{
    float4 color : SV_Target0;
    float4 toon_data : SV_Target1;
};

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

    float4 world = mul(push.model, float4(p, 1.0));
    o.position = mul(global_ubo.viewproj, world);
    o.color = push.color;
    return o;
}

[shader("fragment")]
FSOutput fs_main(VSOutput input)
{
    FSOutput o;
    o.color = input.color;
    o.toon_data = float4(0.0, 0.0, 0.0, 0.0);
    return o;
}
