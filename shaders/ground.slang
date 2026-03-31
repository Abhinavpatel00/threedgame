#include "common_bindless.slang"

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VSOutput vs_main(uint id : SV_VertexID)
{
    float2 p[3] = {
        float2(-1.0, -1.0),
        float2(3.0, -1.0),
        float2(-1.0, 3.0)
    };

    VSOutput o;
    o.position = float4(p[id], 0.0, 1.0);
    o.uv = p[id] * 0.5 + 0.5;
    return o;
}

[shader("fragment")]
float4 fs_main(VSOutput input, out float outDepth : SV_Depth) : SV_Target0
{
    float2 ndc = input.uv * 2.0 - 1.0;
    float4 clip = float4(ndc, 1.0, 1.0);

    float4 world_far = mul(global_ubo.inv_viewproj, clip);
    world_far.xyz /= world_far.w;

    float3 origin = global_ubo.camera_pos.xyz;
    float3 dir = normalize(world_far.xyz - origin);

    if(abs(dir.y) < 1e-5)
        discard;

    float t = -origin.y / dir.y;
    if(t <= 0.0)
        discard;

    float3 world = origin + dir * t;

    float grid_size = 1.0;
    float line_width = 0.02;
    float2 g = abs(frac(world.xz / grid_size) - 0.5);
    float line = (g.x < line_width || g.y < line_width) ? 1.0 : 0.0;

    float3 base_color = float3(0.08, 0.09, 0.1);
    float3 line_color = float3(0.18, 0.2, 0.22);
    float3 color = lerp(base_color, line_color, line);

    float4 clip_pos = mul(global_ubo.viewproj, float4(world, 1.0));
    outDepth = clip_pos.z / clip_pos.w;

    return float4(color, 1.0);
}
