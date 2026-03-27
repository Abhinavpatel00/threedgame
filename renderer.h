
#pragma once

#include "vk.h"

extern Renderer renderer;
typedef struct
{
    uint32_t fullscreen;
    uint32_t postprocess;
    uint32_t triangle;
    uint32_t triangle_wireframe;

    uint32_t beam;
    uint32_t sky;
} EnginePipelines;

extern EnginePipelines pipelines;

#define VALIDATION false

void graphics_init(void);
