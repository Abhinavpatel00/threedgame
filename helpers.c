#include "helpers.h"

#include "mu/mu.h"

#include <math.h>
#include <stdlib.h>

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
