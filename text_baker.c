#include "text_baker.h"

#include <stdlib.h>
#include <string.h>

#include "fs.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "external/stb/stb_truetype.h"

static uint8_t* read_file_bytes(const char* path, size_t* out_size)
{
    void*  data = NULL;
    size_t size = 0;

    if(!fs_read_all(path, &data, &size))
        return NULL;

    if(out_size)
        *out_size = size;

    return (uint8_t*)data;
}

bool text_bake_font_rgba(const char* font_path,
                         float       pixel_height,
                         uint32_t    atlas_width,
                         uint32_t    atlas_height,
                         BakedGlyph  glyphs[96],
                         uint8_t**   out_rgba_pixels,
                         size_t*     out_rgba_size)
{
    if(!font_path || !glyphs || !out_rgba_pixels || atlas_width == 0 || atlas_height == 0)
        return false;

    *out_rgba_pixels = NULL;
    if(out_rgba_size)
        *out_rgba_size = 0;

    size_t   font_size  = 0;
    uint8_t* font_bytes = read_file_bytes(font_path, &font_size);
    if(!font_bytes)
        return false;

    size_t   mono_size   = (size_t)atlas_width * (size_t)atlas_height;
    uint8_t* mono_bitmap = calloc(mono_size, 1);
    if(!mono_bitmap)
    {
        free(font_bytes);
        return false;
    }

    stbtt_bakedchar baked[96] = {0};
    int             bake_res  = stbtt_BakeFontBitmap(font_bytes, 0, pixel_height, mono_bitmap, (int)atlas_width,
                                                     (int)atlas_height, 32, 96, baked);
    free(font_bytes);
    if(bake_res <= 0)
    {
        free(mono_bitmap);
        return false;
    }

    for(int i = 0; i < 96; i++)
    {
        glyphs[i].x0       = baked[i].x0;
        glyphs[i].y0       = baked[i].y0;
        glyphs[i].x1       = baked[i].x1;
        glyphs[i].y1       = baked[i].y1;
        glyphs[i].xoff     = baked[i].xoff;
        glyphs[i].yoff     = baked[i].yoff;
        glyphs[i].xadvance = baked[i].xadvance;
    }

    size_t   rgba_size = mono_size * 4;
    uint8_t* rgba      = malloc(rgba_size);
    if(!rgba)
    {
        free(mono_bitmap);
        return false;
    }

    for(size_t i = 0; i < mono_size; i++)
    {
        uint8_t a       = mono_bitmap[i];
        rgba[i * 4]     = 255;
        rgba[i * 4 + 1] = 255;
        rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = a;
    }

    free(mono_bitmap);

    *out_rgba_pixels = rgba;
    if(out_rgba_size)
        *out_rgba_size = rgba_size;

    return true;
}
