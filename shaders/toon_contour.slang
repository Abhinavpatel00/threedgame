struct ToonOutlinePush
{
	uint depth_tex;
	uint sampler_id;
	float2 inv_resolution;
	float depth_threshold;
	float edge_strength;
};

struct VSOutput
{
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
};

[[vk::binding(0, 0)]]
Texture2D textures[];

[[vk::binding(1, 0)]]
SamplerState samplers[];

[[vk::push_constant]]
ToonOutlinePush push;

[shader("vertex")]
VSOutput vs_main(uint id : SV_VertexID)
{
	static const float2 p[3] = {
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
float4 fs_main(VSOutput input) : SV_Target0
{
	Texture2D depth_tex = textures[NonUniformResourceIndex(push.depth_tex)];
	SamplerState samp = samplers[NonUniformResourceIndex(push.sampler_id)];

	float2 dx = float2(push.inv_resolution.x, 0.0);
	float2 dy = float2(0.0, push.inv_resolution.y);

	float d0 = depth_tex.SampleLevel(samp, input.uv, 0).r;
	float d1 = depth_tex.SampleLevel(samp, input.uv + dx, 0).r;
	float d2 = depth_tex.SampleLevel(samp, input.uv - dx, 0).r;
	float d3 = depth_tex.SampleLevel(samp, input.uv + dy, 0).r;
	float d4 = depth_tex.SampleLevel(samp, input.uv - dy, 0).r;

	float edge_metric = max(max(abs(d1 - d0), abs(d2 - d0)), max(abs(d3 - d0), abs(d4 - d0)));
	float edge = saturate((edge_metric - push.depth_threshold) * push.edge_strength);

	return float4(0.0, 0.0, 0.0, edge);
}
