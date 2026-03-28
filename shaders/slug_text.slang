struct Push
{
	uint64_t vertex_ptr;
	uint64_t index_ptr;
	float4 slug_matrix[4];
	float4 slug_viewport;
	uint   curve_texture_id;
	uint   band_texture_id;
	uint2  _pad0;
};

struct VSOutput
{
	float4 position : SV_Position;
	float4 color : U_COLOR;
	float2 texcoord : U_TEXCOORD;
	nointerpolation float4 banding : U_BANDING;
	nointerpolation int4 glyph : U_GLYPH;
};

[[vk::binding(0, 0)]]
Texture2D textures[];

[[vk::push_constant]]
Push push;

void SlugUnpack(float4 tex, float4 bnd, out float4 vbnd, out int4 vgly)
{
	uint2 g = asuint(tex.zw);
	vgly = int4(g.x & 0xFFFFU, g.x >> 16U, g.y & 0xFFFFU, g.y >> 16U);
	vbnd = bnd;
}

float2 SlugDilate(float4 pos, float4 tex, float4 jac, float4 m0, float4 m1, float4 m3, float2 dim, out float2 vpos)
{
	float2 n = normalize(pos.zw);
	float s = dot(m3.xy, pos.xy) + m3.w;
	float t = dot(m3.xy, n);

	float u = (s * dot(m0.xy, n) - t * (dot(m0.xy, pos.xy) + m0.w)) * dim.x;
	float v = (s * dot(m1.xy, n) - t * (dot(m1.xy, pos.xy) + m1.w)) * dim.y;

	float s2 = s * s;
	float st = s * t;
	float uv = u * u + v * v;
	float2 d = pos.zw * (s2 * (st + sqrt(uv)) / (uv - st * st));

	vpos = pos.xy + d;
	return (float2(tex.x + dot(d, jac.xy), tex.y + dot(d, jac.zw)));
}

[shader("vertex")]
VSOutput vs_main(uint vertexID : SV_VertexID)
{
	uint* indices = (uint*)push.index_ptr;
	uint  src_index = indices[vertexID];

	uint* vtx = (uint*)push.vertex_ptr;
	uint  base = src_index * 20;

	float4 pos = float4(asfloat(vtx[base + 0]), asfloat(vtx[base + 1]), asfloat(vtx[base + 2]), asfloat(vtx[base + 3]));
	float4 tex = float4(asfloat(vtx[base + 4]), asfloat(vtx[base + 5]), asfloat(vtx[base + 6]), asfloat(vtx[base + 7]));
	float4 jac = float4(asfloat(vtx[base + 8]), asfloat(vtx[base + 9]), asfloat(vtx[base + 10]), asfloat(vtx[base + 11]));
	float4 bnd = float4(asfloat(vtx[base + 12]), asfloat(vtx[base + 13]), asfloat(vtx[base + 14]), asfloat(vtx[base + 15]));
	float4 col = float4(asfloat(vtx[base + 16]), asfloat(vtx[base + 17]), asfloat(vtx[base + 18]), asfloat(vtx[base + 19]));

	float2 p;
	VSOutput vresult;

	vresult.texcoord = SlugDilate(pos, tex, jac, push.slug_matrix[0], push.slug_matrix[1], push.slug_matrix[3],
								  push.slug_viewport.xy, p);

	vresult.position.x = p.x * push.slug_matrix[0].x + p.y * push.slug_matrix[0].y + push.slug_matrix[0].w;
	vresult.position.y = p.x * push.slug_matrix[1].x + p.y * push.slug_matrix[1].y + push.slug_matrix[1].w;
	vresult.position.z = p.x * push.slug_matrix[2].x + p.y * push.slug_matrix[2].y + push.slug_matrix[2].w;
	vresult.position.w = p.x * push.slug_matrix[3].x + p.y * push.slug_matrix[3].y + push.slug_matrix[3].w;

	SlugUnpack(tex, bnd, vresult.banding, vresult.glyph);
	vresult.color = col;
	return vresult;
}

#define kLogBandTextureWidth 12

float4 TexelLoad2D(Texture2D tex, int2 p)
{
	return tex.Load(int3(p, 0));
}

uint CalcRootCode(float y1, float y2, float y3)
{
	uint i1 = asuint(y1) >> 31U;
	uint i2 = asuint(y2) >> 30U;
	uint i3 = asuint(y3) >> 29U;

	uint shift = (i2 & 2U) | (i1 & ~2U);
	shift = (i3 & 4U) | (shift & ~4U);

	return ((0x2E74U >> shift) & 0x0101U);
}

float2 SolveHorizPoly(float4 p12, float2 p3)
{
	float2 a = p12.xy - p12.zw * 2.0 + p3;
	float2 b = p12.xy - p12.zw;
	float ra = 1.0 / a.y;
	float rb = 0.5 / b.y;

	float d = sqrt(max(b.y * b.y - a.y * p12.y, 0.0));
	float t1 = (b.y - d) * ra;
	float t2 = (b.y + d) * ra;

	if(abs(a.y) < 1.0 / 65536.0)
		t1 = t2 = p12.y * rb;

	return float2((a.x * t1 - b.x * 2.0) * t1 + p12.x, (a.x * t2 - b.x * 2.0) * t2 + p12.x);
}

float2 SolveVertPoly(float4 p12, float2 p3)
{
	float2 a = p12.xy - p12.zw * 2.0 + p3;
	float2 b = p12.xy - p12.zw;
	float ra = 1.0 / a.x;
	float rb = 0.5 / b.x;

	float d = sqrt(max(b.x * b.x - a.x * p12.x, 0.0));
	float t1 = (b.x - d) * ra;
	float t2 = (b.x + d) * ra;

	if(abs(a.x) < 1.0 / 65536.0)
		t1 = t2 = p12.x * rb;

	return float2((a.y * t1 - b.y * 2.0) * t1 + p12.y, (a.y * t2 - b.y * 2.0) * t2 + p12.y);
}

int2 CalcBandLoc(int2 glyphLoc, uint offset)
{
	int2 bandLoc = int2(glyphLoc.x + int(offset), glyphLoc.y);
	bandLoc.y += bandLoc.x >> kLogBandTextureWidth;
	bandLoc.x &= (1 << kLogBandTextureWidth) - 1;
	return bandLoc;
}

float CalcCoverage(float xcov, float ycov, float xwgt, float ywgt)
{
	float coverage = max(abs(xcov * xwgt + ycov * ywgt) / max(xwgt + ywgt, 1.0 / 65536.0), min(abs(xcov), abs(ycov)));
	return saturate(coverage);
}

float SlugRender(Texture2D curveData, Texture2D bandData, float2 renderCoord, float4 bandTransform, int4 glyphData)
{
	int curveIndex;

	float2 emsPerPixel = fwidth(renderCoord);
	float2 pixelsPerEm = 1.0 / emsPerPixel;

	int2 bandMax = glyphData.zw;
	bandMax.y &= 0x00FF;

	int2 bandIndex = clamp(int2(renderCoord * bandTransform.xy + bandTransform.zw), int2(0, 0), bandMax);
	int2 glyphLoc = glyphData.xy;

	float xcov = 0.0;
	float xwgt = 0.0;

	float4 hbandTex = TexelLoad2D(bandData, int2(glyphLoc.x + bandIndex.y, glyphLoc.y));
	uint2 hbandData = uint2((uint)max(hbandTex.x, 0.0), (uint)max(hbandTex.y, 0.0));
	int2 hbandLoc = CalcBandLoc(glyphLoc, hbandData.y);

	for(curveIndex = 0; curveIndex < int(hbandData.x); curveIndex++)
	{
		float4 curveRefTex = TexelLoad2D(bandData, int2(hbandLoc.x + curveIndex, hbandLoc.y));
		int2 curveLoc = int2((int)max(curveRefTex.x, 0.0), (int)max(curveRefTex.y, 0.0));

		float4 p12 = TexelLoad2D(curveData, curveLoc) - float4(renderCoord, renderCoord);
		float2 p3 = TexelLoad2D(curveData, int2(curveLoc.x + 1, curveLoc.y)).xy - renderCoord;

		if(max(max(p12.x, p12.z), p3.x) * pixelsPerEm.x < -0.5)
			break;

		uint code = CalcRootCode(p12.y, p12.w, p3.y);
		if(code != 0U)
		{
			float2 r = SolveHorizPoly(p12, p3) * pixelsPerEm.x;

			if((code & 1U) != 0U)
			{
				xcov += saturate(r.x + 0.5);
				xwgt = max(xwgt, saturate(1.0 - abs(r.x) * 2.0));
			}

			if(code > 1U)
			{
				xcov -= saturate(r.y + 0.5);
				xwgt = max(xwgt, saturate(1.0 - abs(r.y) * 2.0));
			}
		}
	}

	float ycov = 0.0;
	float ywgt = 0.0;

	float4 vbandTex = TexelLoad2D(bandData, int2(glyphLoc.x + bandMax.y + 1 + bandIndex.x, glyphLoc.y));
	uint2 vbandData = uint2((uint)max(vbandTex.x, 0.0), (uint)max(vbandTex.y, 0.0));
	int2 vbandLoc = CalcBandLoc(glyphLoc, vbandData.y);

	for(curveIndex = 0; curveIndex < int(vbandData.x); curveIndex++)
	{
		float4 curveRefTex = TexelLoad2D(bandData, int2(vbandLoc.x + curveIndex, vbandLoc.y));
		int2 curveLoc = int2((int)max(curveRefTex.x, 0.0), (int)max(curveRefTex.y, 0.0));

		float4 p12 = TexelLoad2D(curveData, curveLoc) - float4(renderCoord, renderCoord);
		float2 p3 = TexelLoad2D(curveData, int2(curveLoc.x + 1, curveLoc.y)).xy - renderCoord;

		if(max(max(p12.y, p12.w), p3.y) * pixelsPerEm.y < -0.5)
			break;

		uint code = CalcRootCode(p12.x, p12.z, p3.x);
		if(code != 0U)
		{
			float2 r = SolveVertPoly(p12, p3) * pixelsPerEm.y;

			if((code & 1U) != 0U)
			{
				ycov -= saturate(r.x + 0.5);
				ywgt = max(ywgt, saturate(1.0 - abs(r.x) * 2.0));
			}

			if(code > 1U)
			{
				ycov += saturate(r.y + 0.5);
				ywgt = max(ywgt, saturate(1.0 - abs(r.y) * 2.0));
			}
		}
	}

	return CalcCoverage(xcov, ycov, xwgt, ywgt);
}

[shader("fragment")]
float4 fs_main(VSOutput vresult) : SV_Target0
{
	Texture2D curveTexture = textures[push.curve_texture_id];
	Texture2D bandTexture = textures[push.band_texture_id];

	float coverage = SlugRender(curveTexture, bandTexture, vresult.texcoord, vresult.banding, vresult.glyph);
	return vresult.color * coverage;
}
