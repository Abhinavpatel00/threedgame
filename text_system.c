#include "text_system.h"

#include "external/stb/stb_truetype.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fs.h"

typedef struct SlugCurve
{
    float p0x, p0y;
    float p1x, p1y;
    float p2x, p2y;
} SlugCurve;

typedef struct SlugBand
{
    uint32_t* curve_indices;
} SlugBand;

typedef struct SlugGlyph
{
    bool      valid;
    int       codepoint;
    int       glyph_index;
    float     x_min;
    float     y_min;
    float     x_max;
    float     y_max;
    float     advance;
    uint16_t  glyph_loc_x;
    uint16_t  glyph_loc_y;
    uint32_t  curve_texel_start;
    SlugCurve* curves;
    SlugBand   h_bands[8];
    SlugBand   v_bands[8];
} SlugGlyph;

typedef struct SlugVertex
{
    vec4 pos;
    vec4 tex;
    vec4 jac;
    vec4 bnd;
    vec4 col;
} SlugVertex;

typedef struct TextSystem
{
    stbtt_fontinfo font;
    uint8_t*       font_bytes;
    float          pixel_height;
    float          em_to_px;
    int            ascent;
    int            descent;
    int            line_gap;

    TextureID curve_texture;
    TextureID band_texture;
    uint32_t  curve_width;
    uint32_t  curve_height;
    uint32_t  band_width;
    uint32_t  band_height;

    SlugGlyph glyphs[96];

    SlugVertex* vertices;
    uint32_t*   indices;

    BufferSlice vertex_slice;
    BufferSlice index_slice;
    VkDeviceSize vertex_capacity_bytes;
    VkDeviceSize index_capacity_bytes;
    bool        ready;
} TextSystem;

PUSH_CONSTANT(SlugTextPush,
              VkDeviceAddress vertex_ptr;
              VkDeviceAddress index_ptr;
              float           slug_matrix[4][4];
              float           slug_viewport[4];
              uint32_t        curve_texture_id;
              uint32_t        band_texture_id;
              uint32_t        _pad0;
              uint32_t        _pad1;
);

static TextSystem g_text_sys;

static inline float bitcast_u32_to_f32(uint32_t value)
{
    union
    {
        uint32_t u;
        float    f;
    } v;
    v.u = value;
    return v.f;
}

static uint8_t* read_file_bytes_local(const char* path, size_t* out_size)
{
    void*  data = NULL;
    size_t size = 0;

    if(!fs_read_all(path, &data, &size))
        return NULL;

    if(out_size)
        *out_size = size;
    return (uint8_t*)data;
}

static SlugCurve line_to_quadratic(float x0, float y0, float x1, float y1)
{
    const float line_epsilon = 0.125f;
    float       mx           = 0.5f * (x0 + x1);
    float       my           = 0.5f * (y0 + y1);
    float       dx           = x1 - x0;
    float       dy           = y1 - y0;

    SlugCurve c = {.p0x = x0, .p0y = y0, .p1x = mx, .p1y = my, .p2x = x1, .p2y = y1};
    if(fabsf(dx) > 0.1f && fabsf(dy) > 0.1f)
    {
        float len = sqrtf(dx * dx + dy * dy);
        if(len > 0.0f)
        {
            float inv = line_epsilon / len;
            c.p1x     = mx - dy * inv;
            c.p1y     = my + dx * inv;
        }
    }
    return c;
}

static void sort_band_indices(const SlugCurve* curves, uint32_t* indices, bool by_x)
{
    uint32_t n = (uint32_t)arrlen(indices);
    for(uint32_t i = 0; i < n; i++)
    {
        for(uint32_t j = i + 1; j < n; j++)
        {
            SlugCurve a = curves[indices[i]];
            SlugCurve b = curves[indices[j]];

            float akey = by_x ? fmaxf(fmaxf(a.p0x, a.p1x), a.p2x) : fmaxf(fmaxf(a.p0y, a.p1y), a.p2y);
            float bkey = by_x ? fmaxf(fmaxf(b.p0x, b.p1x), b.p2x) : fmaxf(fmaxf(b.p0y, b.p1y), b.p2y);
            if(bkey > akey)
            {
                uint32_t t  = indices[i];
                indices[i]  = indices[j];
                indices[j]  = t;
            }
        }
    }
}

static bool build_slug_glyph(int codepoint, SlugGlyph* out)
{
    memset(out, 0, sizeof(*out));
    out->valid     = true;
    out->codepoint = codepoint;

    int glyph_index = stbtt_FindGlyphIndex(&g_text_sys.font, codepoint);
    out->glyph_index = glyph_index;

    int advance = 0;
    int lsb     = 0;
    stbtt_GetGlyphHMetrics(&g_text_sys.font, glyph_index, &advance, &lsb);
    (void)lsb;
    out->advance = (float)advance;

    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if(stbtt_GetGlyphBox(&g_text_sys.font, glyph_index, &x0, &y0, &x1, &y1))
    {
        out->x_min = (float)x0;
        out->y_min = (float)y0;
        out->x_max = (float)x1;
        out->y_max = (float)y1;
    }

    stbtt_vertex* verts  = NULL;
    int           vcount = stbtt_GetGlyphShape(&g_text_sys.font, glyph_index, &verts);

    bool  has_start = false;
    float start_x = 0.0f, start_y = 0.0f;
    float cur_x = 0.0f, cur_y = 0.0f;

    for(int i = 0; i < vcount; i++)
    {
        stbtt_vertex* v = &verts[i];
        switch(v->type)
        {
            case STBTT_vmove:
                if(has_start && (fabsf(cur_x - start_x) > 0.1f || fabsf(cur_y - start_y) > 0.1f))
                    arrput(out->curves, line_to_quadratic(cur_x, cur_y, start_x, start_y));

                cur_x     = (float)v->x;
                cur_y     = (float)v->y;
                start_x   = cur_x;
                start_y   = cur_y;
                has_start = true;
                break;

            case STBTT_vline:
                arrput(out->curves, line_to_quadratic(cur_x, cur_y, (float)v->x, (float)v->y));
                cur_x = (float)v->x;
                cur_y = (float)v->y;
                break;

            case STBTT_vcurve:
            {
                SlugCurve c = {.p0x = cur_x, .p0y = cur_y, .p1x = (float)v->cx, .p1y = (float)v->cy, .p2x = (float)v->x, .p2y = (float)v->y};
                arrput(out->curves, c);
                cur_x = (float)v->x;
                cur_y = (float)v->y;
            }
            break;

            case STBTT_vcubic:
            {
                float cx1 = (float)v->cx, cy1 = (float)v->cy;
                float cx2 = (float)v->cx1, cy2 = (float)v->cy1;
                float ex  = (float)v->x, ey = (float)v->y;

                float m01x = 0.5f * (cur_x + cx1), m01y = 0.5f * (cur_y + cy1);
                float m12x = 0.5f * (cx1 + cx2), m12y = 0.5f * (cy1 + cy2);
                float m23x = 0.5f * (cx2 + ex), m23y = 0.5f * (cy2 + ey);
                float m012x = 0.5f * (m01x + m12x), m012y = 0.5f * (m01y + m12y);
                float m123x = 0.5f * (m12x + m23x), m123y = 0.5f * (m12y + m23y);
                float midx = 0.5f * (m012x + m123x), midy = 0.5f * (m012y + m123y);

                SlugCurve c0 = {.p0x = cur_x, .p0y = cur_y, .p1x = m01x, .p1y = m01y, .p2x = midx, .p2y = midy};
                SlugCurve c1 = {.p0x = midx, .p0y = midy, .p1x = m123x, .p1y = m123y, .p2x = ex, .p2y = ey};
                arrput(out->curves, c0);
                arrput(out->curves, c1);
                cur_x = ex;
                cur_y = ey;
            }
            break;

            default:
                break;
        }
    }

    if(has_start && (fabsf(cur_x - start_x) > 0.1f || fabsf(cur_y - start_y) > 0.1f))
        arrput(out->curves, line_to_quadratic(cur_x, cur_y, start_x, start_y));

    if(verts)
        stbtt_FreeShape(&g_text_sys.font, verts);

    float width  = out->x_max - out->x_min;
    float height = out->y_max - out->y_min;
    uint32_t curve_count = (uint32_t)arrlen(out->curves);

    if(width > 0.0f && height > 0.0f && curve_count > 0)
    {
        for(uint32_t ci = 0; ci < curve_count; ci++)
        {
            SlugCurve c = out->curves[ci];
            float cy_min = fminf(fminf(c.p0y, c.p1y), c.p2y);
            float cy_max = fmaxf(fmaxf(c.p0y, c.p1y), c.p2y);
            float cx_min = fminf(fminf(c.p0x, c.p1x), c.p2x);
            float cx_max = fmaxf(fmaxf(c.p0x, c.p1x), c.p2x);

            int hb0 = (int)floorf((cy_min - out->y_min) / height * 8.0f);
            int hb1 = (int)floorf((cy_max - out->y_min) / height * 8.0f);
            int vb0 = (int)floorf((cx_min - out->x_min) / width * 8.0f);
            int vb1 = (int)floorf((cx_max - out->x_min) / width * 8.0f);

            if(hb0 < 0)
                hb0 = 0;
            if(hb1 > 7)
                hb1 = 7;
            if(vb0 < 0)
                vb0 = 0;
            if(vb1 > 7)
                vb1 = 7;

            for(int b = hb0; b <= hb1; b++)
                arrput(out->h_bands[b].curve_indices, ci);
            for(int b = vb0; b <= vb1; b++)
                arrput(out->v_bands[b].curve_indices, ci);
        }

        for(int b = 0; b < 8; b++)
        {
            sort_band_indices(out->curves, out->h_bands[b].curve_indices, true);
            sort_band_indices(out->curves, out->v_bands[b].curve_indices, false);
        }
    }

    return true;
}

static bool upload_float_texture(TextureID id, uint32_t width, uint32_t height, const float* pixels)
{
    if(id == UINT32_MAX || !pixels)
        return false;

    Texture* tex = &textures[id];
    size_t   bytes = (size_t)width * (size_t)height * 4 * sizeof(float);

    Buffer staging = {0};
    if(!create_buffer(&renderer, (VkDeviceSize)bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST, &staging))
        return false;

    memcpy(staging.mapping, pixels, bytes);

    VkCommandBuffer cmd = vk_begin_one_time_cmd(renderer.device, renderer.one_time_gfx_pool);

    VkImageMemoryBarrier barrier1 = {
        .sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout                       = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout                       = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcAccessMask                   = 0,
        .dstAccessMask                   = VK_ACCESS_TRANSFER_WRITE_BIT,
        .image                           = tex->image,
        .subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.baseMipLevel   = 0,
        .subresourceRange.levelCount     = 1,
        .subresourceRange.baseArrayLayer = 0,
        .subresourceRange.layerCount     = 1,
    };

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier1);

    VkBufferImageCopy region = {
        .bufferOffset                    = 0,
        .bufferRowLength                 = 0,
        .bufferImageHeight               = 0,
        .imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .imageSubresource.mipLevel       = 0,
        .imageSubresource.baseArrayLayer = 0,
        .imageSubresource.layerCount     = 1,
        .imageExtent                     = {width, height, 1},
    };

    vkCmdCopyBufferToImage(cmd, staging.buffer, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier barrier2 = barrier1;
    barrier2.oldLayout            = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier2.newLayout            = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier2.srcAccessMask        = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier2.dstAccessMask        = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier2);

    vk_end_one_time_cmd(renderer.device, renderer.graphics_queue, renderer.one_time_gfx_pool, cmd);
    destroy_buffer(&renderer, &staging);
    return true;
}

static bool slug_build_and_upload_atlas(void)
{
    const uint32_t tex_width = 4096;

    uint32_t total_curve_texels = 0;
    for(int i = 0; i < 96; i++)
    {
        uint32_t n = (uint32_t)arrlen(g_text_sys.glyphs[i].curves);
        total_curve_texels += n * 2;
    }
    if(total_curve_texels == 0)
        total_curve_texels = 1;

    uint32_t curve_height = (total_curve_texels + tex_width - 1) / tex_width;
    float*   curve_data   = calloc((size_t)tex_width * curve_height * 4, sizeof(float));
    if(!curve_data)
        return false;

    uint32_t curve_texel = 0;
    for(int gi = 0; gi < 96; gi++)
    {
        SlugGlyph* g = &g_text_sys.glyphs[gi];
        g->curve_texel_start = curve_texel;
        for(uint32_t ci = 0; ci < (uint32_t)arrlen(g->curves); ci++)
        {
            SlugCurve c = g->curves[ci];

            uint32_t i0   = curve_texel;
            uint32_t off0 = ((i0 / tex_width) * tex_width + (i0 % tex_width)) * 4;
            curve_data[off0 + 0] = c.p0x;
            curve_data[off0 + 1] = c.p0y;
            curve_data[off0 + 2] = c.p1x;
            curve_data[off0 + 3] = c.p1y;

            uint32_t i1   = curve_texel + 1;
            uint32_t off1 = ((i1 / tex_width) * tex_width + (i1 % tex_width)) * 4;
            curve_data[off1 + 0] = c.p2x;
            curve_data[off1 + 1] = c.p2y;

            curve_texel += 2;
        }
    }

    uint32_t total_band_texels = 0;
    for(int gi = 0; gi < 96; gi++)
    {
        SlugGlyph* g = &g_text_sys.glyphs[gi];
        if(arrlen(g->curves) == 0)
            continue;

        uint32_t header_count = 16;
        uint32_t pad          = tex_width - (total_band_texels % tex_width);
        if(pad < header_count && pad < tex_width)
            total_band_texels += pad;

        total_band_texels += header_count;
        for(int b = 0; b < 8; b++)
            total_band_texels += (uint32_t)arrlen(g->h_bands[b].curve_indices);
        for(int b = 0; b < 8; b++)
            total_band_texels += (uint32_t)arrlen(g->v_bands[b].curve_indices);
    }
    if(total_band_texels == 0)
        total_band_texels = 1;

    uint32_t band_height = (total_band_texels + tex_width - 1) / tex_width;
    float*   band_data   = calloc((size_t)tex_width * band_height * 4, sizeof(float));
    if(!band_data)
    {
        free(curve_data);
        return false;
    }

    uint32_t band_texel = 0;
    for(int gi = 0; gi < 96; gi++)
    {
        SlugGlyph* g = &g_text_sys.glyphs[gi];
        if(arrlen(g->curves) == 0)
            continue;

        uint32_t header_count = 16;
        uint32_t cur_x = band_texel % tex_width;
        if(cur_x + header_count > tex_width)
            band_texel = ((band_texel / tex_width) + 1) * tex_width;

        uint32_t glyph_start = band_texel;
        g->glyph_loc_x       = (uint16_t)(glyph_start % tex_width);
        g->glyph_loc_y       = (uint16_t)(glyph_start / tex_width);

        uint32_t band_offsets[16] = {0};
        uint32_t cursor           = header_count;
        for(int b = 0; b < 8; b++)
        {
            band_offsets[b] = cursor;
            cursor += (uint32_t)arrlen(g->h_bands[b].curve_indices);
        }
        for(int b = 0; b < 8; b++)
        {
            band_offsets[8 + b] = cursor;
            cursor += (uint32_t)arrlen(g->v_bands[b].curve_indices);
        }

        for(int b = 0; b < 8; b++)
        {
            uint32_t tl  = glyph_start + (uint32_t)b;
            uint32_t off = ((tl / tex_width) * tex_width + (tl % tex_width)) * 4;
            band_data[off + 0] = (float)arrlen(g->h_bands[b].curve_indices);
            band_data[off + 1] = (float)band_offsets[b];
        }
        for(int b = 0; b < 8; b++)
        {
            uint32_t tl  = glyph_start + 8 + (uint32_t)b;
            uint32_t off = ((tl / tex_width) * tex_width + (tl % tex_width)) * 4;
            band_data[off + 0] = (float)arrlen(g->v_bands[b].curve_indices);
            band_data[off + 1] = (float)band_offsets[8 + b];
        }

        for(int b = 0; b < 8; b++)
        {
            uint32_t list_start = glyph_start + band_offsets[b];
            for(uint32_t j = 0; j < (uint32_t)arrlen(g->h_bands[b].curve_indices); j++)
            {
                uint32_t ci       = g->h_bands[b].curve_indices[j];
                uint32_t texel_id = g->curve_texel_start + ci * 2;
                uint32_t tl       = list_start + j;
                uint32_t off      = ((tl / tex_width) * tex_width + (tl % tex_width)) * 4;
                band_data[off + 0] = (float)(texel_id % tex_width);
                band_data[off + 1] = (float)(texel_id / tex_width);
            }
        }
        for(int b = 0; b < 8; b++)
        {
            uint32_t list_start = glyph_start + band_offsets[8 + b];
            for(uint32_t j = 0; j < (uint32_t)arrlen(g->v_bands[b].curve_indices); j++)
            {
                uint32_t ci       = g->v_bands[b].curve_indices[j];
                uint32_t texel_id = g->curve_texel_start + ci * 2;
                uint32_t tl       = list_start + j;
                uint32_t off      = ((tl / tex_width) * tex_width + (tl % tex_width)) * 4;
                band_data[off + 0] = (float)(texel_id % tex_width);
                band_data[off + 1] = (float)(texel_id / tex_width);
            }
        }

        band_texel = glyph_start + cursor;
    }

    TextureCreateDesc curve_desc = {
        .width     = tex_width,
        .height    = curve_height,
        .mip_count = 1,
        .format    = VK_FORMAT_R32G32B32A32_SFLOAT,
        .usage     = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    };
    TextureCreateDesc band_desc = curve_desc;
    band_desc.height            = band_height;

    g_text_sys.curve_texture = create_texture(&renderer, &curve_desc);
    g_text_sys.band_texture  = create_texture(&renderer, &band_desc);
    if(g_text_sys.curve_texture == UINT32_MAX || g_text_sys.band_texture == UINT32_MAX)
    {
        free(curve_data);
        free(band_data);
        return false;
    }

    bool ok = upload_float_texture(g_text_sys.curve_texture, tex_width, curve_height, curve_data)
              && upload_float_texture(g_text_sys.band_texture, tex_width, band_height, band_data);

    g_text_sys.curve_width  = tex_width;
    g_text_sys.curve_height = curve_height;
    g_text_sys.band_width   = tex_width;
    g_text_sys.band_height  = band_height;

    free(curve_data);
    free(band_data);
    return ok;
}

bool text_system_init(const char* font_path, float pixel_height)
{
    memset(&g_text_sys, 0, sizeof(g_text_sys));
    g_text_sys.curve_texture = UINT32_MAX;
    g_text_sys.band_texture  = UINT32_MAX;
    g_text_sys.pixel_height  = pixel_height;

    size_t font_size = 0;
    g_text_sys.font_bytes = read_file_bytes_local(font_path, &font_size);
    if(!g_text_sys.font_bytes)
        return false;

    if(!stbtt_InitFont(&g_text_sys.font, g_text_sys.font_bytes, stbtt_GetFontOffsetForIndex(g_text_sys.font_bytes, 0)))
        return false;

    stbtt_GetFontVMetrics(&g_text_sys.font, &g_text_sys.ascent, &g_text_sys.descent, &g_text_sys.line_gap);
    g_text_sys.em_to_px = stbtt_ScaleForPixelHeight(&g_text_sys.font, pixel_height);

    for(int cp = 32; cp < 128; cp++)
    {
        if(!build_slug_glyph(cp, &g_text_sys.glyphs[cp - 32]))
            return false;
    }

    if(!slug_build_and_upload_atlas())
        return false;

    g_text_sys.ready = true;
    return true;
}

void text_system_begin_frame(void)
{
    arrsetlen(g_text_sys.vertices, 0);
    arrsetlen(g_text_sys.indices, 0);
}

void draw_text_2d(const char* text, float x, float y, float scale, vec4 color, float depth)
{
    if(!g_text_sys.ready || !text)
        return;

    float em_px    = g_text_sys.em_to_px * scale;
    float cursor_x = x;
    float baseline = y + (float)g_text_sys.ascent * em_px;

    size_t text_len = strlen(text);

    for(size_t i = 0; i < text_len; i++)
    {
        unsigned char c = (unsigned char)text[i];

        if(c == '\n')
        {
            cursor_x = x;
            baseline += g_text_sys.pixel_height * scale;
            continue;
        }

        if(c == '\t')
        {
            cursor_x += g_text_sys.pixel_height * scale;
            continue;
        }

        if(c < 32 || c >= 128)
            continue;

        SlugGlyph* glyph = &g_text_sys.glyphs[c - 32];

        float x0 = cursor_x + glyph->x_min * em_px;
        float x1 = cursor_x + glyph->x_max * em_px;
        float y0 = baseline - glyph->y_max * em_px;
        float y1 = baseline - glyph->y_min * em_px;

        float advance_px = glyph->advance * em_px;
        int   next_c     = (i + 1 < text_len) ? (unsigned char)text[i + 1] : 0;
        float kern_px    = (next_c >= 32 && next_c < 128) ? stbtt_GetCodepointKernAdvance(&g_text_sys.font, c, next_c) * em_px : 0.0f;

        if(arrlen(glyph->curves) == 0 || (glyph->x_max - glyph->x_min) <= 0.0f || (glyph->y_max - glyph->y_min) <= 0.0f)
        {
            cursor_x += advance_px + kern_px;
            continue;
        }

        float inv_obj_to_em = 1.0f / em_px;
        float w_em          = glyph->x_max - glyph->x_min;
        float h_em          = glyph->y_max - glyph->y_min;

        float glyph_loc = bitcast_u32_to_f32(((uint32_t)glyph->glyph_loc_y << 16) | glyph->glyph_loc_x);
        float band_max  = bitcast_u32_to_f32((7u << 16) | 7u);

        float band_scale_x  = (w_em > 0.0f) ? (8.0f / w_em) : 0.0f;
        float band_scale_y  = (h_em > 0.0f) ? (8.0f / h_em) : 0.0f;
        float band_offset_x = -glyph->x_min * band_scale_x;
        float band_offset_y = -glyph->y_min * band_scale_y;

        uint32_t base = (uint32_t)arrlen(g_text_sys.vertices);
        SlugVertex v0 = {
            .pos = {x0, y1, -1.0f, -1.0f},
            .tex = {glyph->x_min, glyph->y_min, glyph_loc, band_max},
            .jac = {inv_obj_to_em, 0.0f, 0.0f, inv_obj_to_em},
            .bnd = {band_scale_x, band_scale_y, band_offset_x, band_offset_y},
            .col = {color[0], color[1], color[2], color[3]},
        };
        SlugVertex v1 = {
            .pos = {x1, y1, 1.0f, -1.0f},
            .tex = {glyph->x_max, glyph->y_min, glyph_loc, band_max},
            .jac = {inv_obj_to_em, 0.0f, 0.0f, inv_obj_to_em},
            .bnd = {band_scale_x, band_scale_y, band_offset_x, band_offset_y},
            .col = {color[0], color[1], color[2], color[3]},
        };
        SlugVertex v2 = {
            .pos = {x1, y0, 1.0f, 1.0f},
            .tex = {glyph->x_max, glyph->y_max, glyph_loc, band_max},
            .jac = {inv_obj_to_em, 0.0f, 0.0f, inv_obj_to_em},
            .bnd = {band_scale_x, band_scale_y, band_offset_x, band_offset_y},
            .col = {color[0], color[1], color[2], color[3]},
        };
        SlugVertex v3 = {
            .pos = {x0, y0, -1.0f, 1.0f},
            .tex = {glyph->x_min, glyph->y_max, glyph_loc, band_max},
            .jac = {inv_obj_to_em, 0.0f, 0.0f, inv_obj_to_em},
            .bnd = {band_scale_x, band_scale_y, band_offset_x, band_offset_y},
            .col = {color[0], color[1], color[2], color[3]},
        };

        arrput(g_text_sys.vertices, v0);
        arrput(g_text_sys.vertices, v1);
        arrput(g_text_sys.vertices, v2);
        arrput(g_text_sys.vertices, v3);

        arrput(g_text_sys.indices, base + 0);
        arrput(g_text_sys.indices, base + 1);
        arrput(g_text_sys.indices, base + 2);
        arrput(g_text_sys.indices, base + 0);
        arrput(g_text_sys.indices, base + 2);
        arrput(g_text_sys.indices, base + 3);

        (void)depth;
        cursor_x += advance_px + kern_px;
    }
}

bool text_system_prepare_gpu_data(VkCommandBuffer cmd)
{
    uint32_t index_count = (uint32_t)arrlen(g_text_sys.indices);
    if(index_count == 0)
    {
        memset(&g_text_sys.vertex_slice, 0, sizeof(g_text_sys.vertex_slice));
        memset(&g_text_sys.index_slice, 0, sizeof(g_text_sys.index_slice));
        return true;
    }

    VkDeviceSize vtx_bytes = sizeof(SlugVertex) * (VkDeviceSize)arrlen(g_text_sys.vertices);
    VkDeviceSize idx_bytes = sizeof(uint32_t) * (VkDeviceSize)index_count;

    if(!g_text_sys.vertex_slice.buffer || g_text_sys.vertex_capacity_bytes < vtx_bytes)
    {
        if(g_text_sys.vertex_slice.buffer)
        {
            buffer_pool_free(g_text_sys.vertex_slice);
            memset(&g_text_sys.vertex_slice, 0, sizeof(g_text_sys.vertex_slice));
            g_text_sys.vertex_capacity_bytes = 0;
        }

        g_text_sys.vertex_slice = buffer_pool_alloc(&renderer.gpu_pool, vtx_bytes, 16);
        if(!g_text_sys.vertex_slice.buffer)
            return false;
        g_text_sys.vertex_capacity_bytes = g_text_sys.vertex_slice.size;
    }

    if(!g_text_sys.index_slice.buffer || g_text_sys.index_capacity_bytes < idx_bytes)
    {
        if(g_text_sys.index_slice.buffer)
        {
            buffer_pool_free(g_text_sys.index_slice);
            memset(&g_text_sys.index_slice, 0, sizeof(g_text_sys.index_slice));
            g_text_sys.index_capacity_bytes = 0;
        }

        g_text_sys.index_slice = buffer_pool_alloc(&renderer.gpu_pool, idx_bytes, 16);
        if(!g_text_sys.index_slice.buffer)
            return false;
        g_text_sys.index_capacity_bytes = g_text_sys.index_slice.size;
    }

    bool uploaded_vertices = renderer_upload_buffer_to_slice(&renderer, cmd, g_text_sys.vertex_slice, g_text_sys.vertices, vtx_bytes, 16);
    bool uploaded_indices  = renderer_upload_buffer_to_slice(&renderer, cmd, g_text_sys.index_slice, g_text_sys.indices, idx_bytes, 16);

    if(!uploaded_vertices || !uploaded_indices)
    {
        return false;
    }

    VkBufferMemoryBarrier2 barriers[2] = {
        {
            .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            .buffer        = g_text_sys.vertex_slice.buffer,
            .offset        = g_text_sys.vertex_slice.offset,
            .size          = vtx_bytes,
        },
        {
            .sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
            .buffer        = g_text_sys.index_slice.buffer,
            .offset        = g_text_sys.index_slice.offset,
            .size          = idx_bytes,
        },
    };

    VkDependencyInfo dep = {
        .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount = 2,
        .pBufferMemoryBarriers    = barriers,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
    return true;
}

void text_system_handle_prepare_failure(void)
{
    arrsetlen(g_text_sys.indices, 0);
    memset(&g_text_sys.vertex_slice, 0, sizeof(g_text_sys.vertex_slice));
    memset(&g_text_sys.index_slice, 0, sizeof(g_text_sys.index_slice));
}

void text_system_render(VkCommandBuffer cmd)
{
    uint32_t index_count = (uint32_t)arrlen(g_text_sys.indices);
    if(index_count == 0 || g_text_sys.curve_texture == UINT32_MAX || g_text_sys.band_texture == UINT32_MAX)
        return;

    if(!g_text_sys.vertex_slice.buffer || !g_text_sys.index_slice.buffer)
        return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_render_pipelines.pipelines[pipelines.slug_text]);
    vk_cmd_set_viewport_scissor(cmd, renderer.swapchain.extent);

    VkBufferDeviceAddressInfo vtx_addr = {
        .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = g_text_sys.vertex_slice.buffer,
    };
    VkBufferDeviceAddressInfo idx_addr = {
        .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = g_text_sys.index_slice.buffer,
    };

    float w = (float)renderer.swapchain.extent.width;
    float h = (float)renderer.swapchain.extent.height;

    SlugTextPush push = {0};
    push.vertex_ptr   = vkGetBufferDeviceAddress(renderer.device, &vtx_addr) + g_text_sys.vertex_slice.offset;
    push.index_ptr    = vkGetBufferDeviceAddress(renderer.device, &idx_addr) + g_text_sys.index_slice.offset;

    push.slug_matrix[0][0] = 2.0f / w;
    push.slug_matrix[0][1] = 0.0f;
    push.slug_matrix[0][2] = 0.0f;
    push.slug_matrix[0][3] = -1.0f;

    push.slug_matrix[1][0] = 0.0f;
    push.slug_matrix[1][1] = 2.0f / h;
    push.slug_matrix[1][2] = 0.0f;
    push.slug_matrix[1][3] = -1.0f;

    push.slug_matrix[2][0] = 0.0f;
    push.slug_matrix[2][1] = 0.0f;
    push.slug_matrix[2][2] = 0.0f;
    push.slug_matrix[2][3] = 0.0f;

    push.slug_matrix[3][0] = 0.0f;
    push.slug_matrix[3][1] = 0.0f;
    push.slug_matrix[3][2] = 0.0f;
    push.slug_matrix[3][3] = 1.0f;

    push.slug_viewport[0] = w;
    push.slug_viewport[1] = h;
    push.slug_viewport[2] = 0.0f;
    push.slug_viewport[3] = 0.0f;

    push.curve_texture_id = g_text_sys.curve_texture;
    push.band_texture_id  = g_text_sys.band_texture;

    vkCmdPushConstants(cmd, renderer.bindless_system.pipeline_layout, VK_SHADER_STAGE_ALL, 0, sizeof(SlugTextPush), &push);
    vkCmdDraw(cmd, index_count, 1, 0, 0);
}
