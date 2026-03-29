#pragma once

#include "renderer.h"

bool text_system_init(const char* font_path, float pixel_height);
void text_system_begin_frame(void);
void draw_text_2d(const char* text, float x, float y, float scale, vec4 color, float depth);
bool text_system_prepare_gpu_data(VkCommandBuffer cmd);
void text_system_handle_prepare_failure(void);
void text_system_render(VkCommandBuffer cmd);
