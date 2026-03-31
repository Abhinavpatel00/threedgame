#include "helpers.h"

#include "mu/mu.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

uint32_t hash32_bytes(const void* data, size_t size)
{
    return (uint32_t)XXH32(data, size, 0);
}

uint64_t hash64_bytes(const void* data, size_t size)
{
    return (uint64_t)XXH64(data, size, 0);
}

uint32_t round_up(uint32_t a, uint32_t b)
{
    return (a + b - 1) & ~(b - 1);
}
uint64_t round_up_64(uint64_t a, uint64_t b)
{
    return (a + b - 1) & ~(b - 1);
}

size_t c99_strnlen(const char* s, size_t maxlen)
{
    size_t i = 0;
    if(!s)
        return 0;
    for(; i < maxlen && s[i]; i++)
    {
    }
    return i;
}

static mu_pcg32 g_rng;
static bool     g_rng_seeded = false;

void rand_seed(uint64_t seed)
{
    mu_pcg32_init(&g_rng, seed, seed ^ 0xda3e39cb94b95bdbULL);
    g_rng_seeded = true;
}

static uint32_t rand_u32(void)
{
    if(!g_rng_seeded)
        rand_seed(1u);

    return mu_pcg32_next_u32(&g_rng);
}

float rand_float01(void)
{
    return (float)rand_u32() * (1.0f / 4294967295.0f);
}

float rand_float_range(float min, float max)
{
    if(max < min)
    {
        float tmp = min;
        min = max;
        max = tmp;
    }

    return min + (max - min) * rand_float01();
}

int rand_int_range(int min, int max)
{
    if(max < min)
    {
        int tmp = min;
        min = max;
        max = tmp;
    }

    int span = max - min + 1;
    if(span <= 1)
        return min;

    uint32_t span_u    = (uint32_t)span;
    uint32_t threshold = (uint32_t)(-span_u) % span_u;
    for(;;)
    {
        uint32_t r = rand_u32();
        if(r >= threshold)
            return min + (int)(r % span_u);
    }
}

void compose_transform_pos_rot(const vec3 position, const vec3 rotation, mat4 out_transform)
{
    mat4 rot;
    glm_mat4_identity(out_transform);
    glm_translate(out_transform, position);

    glm_mat4_identity(rot);
    glm_rotate_x(rot, rotation[0], rot);
    glm_rotate_y(rot, rotation[1], rot);
    glm_rotate_z(rot, rotation[2], rot);
    glm_mat4_mul(out_transform, rot, out_transform);
}

bool segment_aabb_intersect(const vec3 p0, const vec3 p1, const vec3 b_center, const vec3 b_half)
{
    const float eps = 1e-6f;
    float       tmin = 0.0f;
    float       tmax = 1.0f;
    vec3        dir;

    dir[0] = p1[0] - p0[0];
    dir[1] = p1[1] - p0[1];
    dir[2] = p1[2] - p0[2];

    for(int axis = 0; axis < 3; ++axis)
    {
        float origin = p0[axis];
        float minb   = b_center[axis] - b_half[axis];
        float maxb   = b_center[axis] + b_half[axis];
        float d      = dir[axis];

        if(fabsf(d) < eps)
        {
            if(origin < minb || origin > maxb)
                return false;
            continue;
        }

        float inv_d = 1.0f / d;
        float t1    = (minb - origin) * inv_d;
        float t2    = (maxb - origin) * inv_d;
        float tnear = fminf(t1, t2);
        float tfar  = fmaxf(t1, t2);

        if(tnear > tmin)
            tmin = tnear;
        if(tfar < tmax)
            tmax = tfar;
        if(tmin > tmax)
            return false;
    }

    return true;
}

void build_aabb_wireframe(const vec3 center, const vec3 half, vec3 out_positions[8], uint32_t out_indices[24])
{
    if(!out_positions || !out_indices)
        return;

    float x0 = center[0] - half[0];
    float x1 = center[0] + half[0];
    float y0 = center[1] - half[1];
    float y1 = center[1] + half[1];
    float z0 = center[2] - half[2];
    float z1 = center[2] + half[2];

    glm_vec3_copy((vec3){x0, y0, z0}, out_positions[0]);
    glm_vec3_copy((vec3){x1, y0, z0}, out_positions[1]);
    glm_vec3_copy((vec3){x0, y1, z0}, out_positions[2]);
    glm_vec3_copy((vec3){x1, y1, z0}, out_positions[3]);
    glm_vec3_copy((vec3){x0, y0, z1}, out_positions[4]);
    glm_vec3_copy((vec3){x1, y0, z1}, out_positions[5]);
    glm_vec3_copy((vec3){x0, y1, z1}, out_positions[6]);
    glm_vec3_copy((vec3){x1, y1, z1}, out_positions[7]);

    static const uint32_t k_indices[24] = {
        0, 1,
        1, 3,
        3, 2,
        2, 0,
        4, 5,
        5, 7,
        7, 6,
        6, 4,
        0, 4,
        1, 5,
        2, 6,
        3, 7,
    };

    memcpy(out_indices, k_indices, sizeof(k_indices));
}
