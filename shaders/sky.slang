// Preetham atmospheric scattering sky
// Ported from opengl_sky/main.c

struct SkyPush
{
    float4x4 inv_proj;

    float4   basis_right;
    float4   basis_up;
    float4   basis_back;

    float    time;
    float    cirrus;
    float    cumulus;
    float    pad0;
};

[[vk::push_constant]] SkyPush push;


struct VSOutput
{
    float4 position : SV_Position;
    float3 pos      : TEXCOORD0;   // world-space ray direction
    float3 fsun     : TEXCOORD1;   // sun direction (animated)
};

[shader("vertex")]
VSOutput vs_main(uint id : SV_VertexID)
{
    float2 data[4] = {
        float2(-1.0,  1.0),
        float2(-1.0, -1.0),
        float2( 1.0,  1.0),
        float2( 1.0, -1.0),
    };

    float2 ndc = data[id];

    // Match the original GLSL exactly:
    // pos = transpose(mat3(V)) * (inverse(P) * gl_Position).xyz;
    float4 clip_pos = float4(ndc, 0.0, 1.0);
    float3 view_dir = mul(push.inv_proj, clip_pos).xyz;
    float3 world_dir = view_dir.x * push.basis_right.xyz
                     + view_dir.y * push.basis_up.xyz
                     + view_dir.z * push.basis_back.xyz;

    VSOutput o;
    o.position = float4(ndc, 0.0, 1.0);
    o.pos      = world_dir;
    o.fsun     = float3(0.0, sin(push.time * 0.01), cos(push.time * 0.01));
    return o;
}

// ── Helpers ──────────────────────────────────────────────────────────────────

float hash_f(float n)
{
    return fract(sin(n) * 43758.5453123);
}

float noise_f(float3 x)
{
    float3 f = fract(x);
    float n  = dot(floor(x), float3(1.0, 157.0, 113.0));
    return lerp(
        lerp(lerp(hash_f(n +   0.0), hash_f(n +   1.0), f.x),
             lerp(hash_f(n + 157.0), hash_f(n + 158.0), f.x), f.y),
        lerp(lerp(hash_f(n + 113.0), hash_f(n + 114.0), f.x),
             lerp(hash_f(n + 270.0), hash_f(n + 271.0), f.x), f.y), f.z);
}

static const float3x3 m3 = float3x3(
     0.0,  1.60,  1.20,
    -1.6,  0.72, -0.96,
    -1.2, -0.96,  1.28);

float fbm(float3 p)
{
    float f = 0.0;
    f += noise_f(p)         / 2;  p = mul(m3, p) * 1.1;
    f += noise_f(p)         / 4;  p = mul(m3, p) * 1.2;
    f += noise_f(p)         / 6;  p = mul(m3, p) * 1.3;
    f += noise_f(p)         / 12; p = mul(m3, p) * 1.4;
    f += noise_f(p)         / 24;
    return f;
}

// ── Fragment ─────────────────────────────────────────────────────────────────

[shader("fragment")]
float4 fs_main(VSOutput input) : SV_Target0
{
    float3 pos  = input.pos;
    float3 fsun = input.fsun;

    if (pos.y < 0.0)
        discard;

    const float Br = 0.0025;
    const float Bm = 0.0003;
    const float g  = 0.9800;
    const float3 nitrogen = float3(0.650, 0.570, 0.475);
    const float3 Kr = Br / pow(nitrogen, float3(4.0, 4.0, 4.0));
    const float3 Km = Bm / pow(nitrogen, float3(0.84, 0.84, 0.84));

    // Atmosphere scattering
    float  mu      = dot(normalize(pos), normalize(fsun));
    float  rayleigh = 3.0 / (8.0 * 3.14) * (1.0 + mu * mu);
    float3 mie     = (Kr + Km * (1.0 - g * g) / (2.0 + g * g)
                      / pow(1.0 + g * g - 2.0 * g * mu, 1.5)) / (Br + Bm);

    float3 day_extinction = exp(
        -exp(-((pos.y + fsun.y * 4.0) * (exp(-pos.y * 16.0) + 0.1) / 80.0) / Br)
        * (exp(-pos.y * 16.0) + 0.1) * Kr / Br)
        * exp(-pos.y * exp(-pos.y * 8.0) * 4.0)
        * exp(-pos.y * 2.0) * 4.0;

    float3 night_extinction = float3(1.0 - exp(fsun.y)) * 0.2;
    float3 extinction       = lerp(day_extinction, night_extinction, -fsun.y * 0.2 + 0.5);

    float3 color = rayleigh * mie * extinction;

    // Cirrus clouds
    {
        float density = smoothstep(1.0 - push.cirrus, 1.0,
                                   fbm(pos.xyz / pos.y * 2.0 + push.time * 0.05)) * 0.3;
        color = lerp(color, extinction * 4.0, density * max(pos.y, 0.0));
    }

    // Cumulus clouds (3 octaves)
    for (int i = 0; i < 3; i++)
    {
        float density = smoothstep(1.0 - push.cumulus, 1.0,
                                   fbm((0.7 + float(i) * 0.01) * pos.xyz / pos.y + push.time * 0.3));
        color = lerp(color, extinction * density * 5.0, min(density, 1.0) * max(pos.y, 0.0));
    }

    // Dithering noise
    color += noise_f(pos * 1000.0) * 0.01;

    return float4(color, 1.0);
}
