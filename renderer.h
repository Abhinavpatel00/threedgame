
#pragma once

#include "vk.h"

extern Renderer renderer;
typedef struct
{
    uint32_t fullscreen;
    uint32_t toon_outline;
    uint32_t dof_prepare;
    uint32_t postprocess;
    uint32_t bloom_downsample;
    uint32_t bloom_upsample;
    uint32_t gltf_minimal;
    uint32_t triangle;
    uint32_t triangle_wireframe;
    uint32_t sprite;
    uint32_t slug_text;

    uint32_t beam;
    uint32_t sky;
} EnginePipelines;

extern EnginePipelines pipelines;

#define VALIDATION false 

void graphics_init(void);

void gfx_pipelines();
