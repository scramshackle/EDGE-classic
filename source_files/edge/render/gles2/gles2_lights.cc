#include "gles2_lights.h"

#include <math.h>
#include <string.h>

#include <vector>

#include "gles2_loader.h"
#include "r_lightgrid.h"

static Gles2LightGridState light_grid_state;

static constexpr int kGles2LightDataWidth  = 256;
static constexpr int kGles2LightDataHeight = 4;

static int header_texture_width  = 0;
static int header_texture_height = 0;
static int list_texture_width    = 0;
static int list_texture_height   = 0;

static std::vector<uint8_t> light_data_pixels;
static std::vector<uint8_t> header_pixels;
static std::vector<uint8_t> list_pixels;

static int NextPowerOfTwo(int value)
{
    int result = 1;

    while (result < value)
        result *= 2;

    return result;
}

static void EncodeUnitPair(uint8_t *destination, float value)
{
    if (value < 0.0f)
        value = 0.0f;

    if (value > 1.0f)
        value = 1.0f;

    float scaled = value * 255.0f;

    float high = floorf(scaled);

    float low = (scaled - high) * 255.0f;

    destination[0] = (uint8_t)high;
    destination[1] = (uint8_t)(low + 0.5f);
}

void Gles2CreateLightGridTextures(void)
{
    Gles2DestroyLightGridTextures();

    glGenTextures(1, &light_grid_state.data_texture);
    glGenTextures(1, &light_grid_state.header_texture);
    glGenTextures(1, &light_grid_state.list_texture);

    GLuint textures[3] = {light_grid_state.data_texture, light_grid_state.header_texture,
                          light_grid_state.list_texture};

    for (int i = 0; i < 3; i++)
    {
        glBindTexture(GL_TEXTURE_2D, textures[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    light_data_pixels.assign((size_t)kGles2LightDataWidth * kGles2LightDataHeight * 4, 0);

    glBindTexture(GL_TEXTURE_2D, light_grid_state.data_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kGles2LightDataWidth, kGles2LightDataHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 light_data_pixels.data());

    light_grid_state.data_texel_step = 1.0f / (float)kGles2LightDataWidth;

    header_texture_width  = 0;
    header_texture_height = 0;
    list_texture_width    = 0;
    list_texture_height   = 0;

    glBindTexture(GL_TEXTURE_2D, 0);
}

void Gles2DestroyLightGridTextures(void)
{
    if (light_grid_state.data_texture)
    {
        glDeleteTextures(1, &light_grid_state.data_texture);
        light_grid_state.data_texture = 0;
    }

    if (light_grid_state.header_texture)
    {
        glDeleteTextures(1, &light_grid_state.header_texture);
        light_grid_state.header_texture = 0;
    }

    if (light_grid_state.list_texture)
    {
        glDeleteTextures(1, &light_grid_state.list_texture);
        light_grid_state.list_texture = 0;
    }

    light_data_pixels.clear();
    header_pixels.clear();
    list_pixels.clear();

    header_texture_width  = 0;
    header_texture_height = 0;
    list_texture_width    = 0;
    list_texture_height   = 0;

    light_grid_state.active = false;
}

const Gles2LightGridState *Gles2CurrentLightGrid(void)
{
    return &light_grid_state;
}

void Gles2UploadLightGrid(const LightGrid *grid)
{
    light_grid_state.active = false;

    if (!grid || grid->Empty() || !light_grid_state.data_texture || !light_grid_state.header_texture || !light_grid_state.list_texture)
        return;

    int light_total = (int)grid->lights.size();

    if (light_total > kGles2LightDataWidth)
        light_total = kGles2LightDataWidth;

    float minimum[3] = {grid->lights[0].eye_position.Elements[0], grid->lights[0].eye_position.Elements[1],
                        grid->lights[0].eye_position.Elements[2]};
    float maximum[3] = {minimum[0], minimum[1], minimum[2]};

    float radius_scale = 1.0f;

    for (int i = 0; i < light_total; i++)
    {
        const LightGridLight &light = grid->lights[i];

        for (int axis = 0; axis < 3; axis++)
        {
            if (light.eye_position.Elements[axis] < minimum[axis])
                minimum[axis] = light.eye_position.Elements[axis];

            if (light.eye_position.Elements[axis] > maximum[axis])
                maximum[axis] = light.eye_position.Elements[axis];
        }

        if (light.radius > radius_scale)
            radius_scale = light.radius;
    }

    for (int axis = 0; axis < 3; axis++)
    {
        light_grid_state.bounds_minimum[axis] = minimum[axis];

        float range = maximum[axis] - minimum[axis];

        light_grid_state.bounds_range[axis] = (range > 0.001f) ? range : 1.0f;
    }

    light_grid_state.radius_scale = radius_scale;

    memset(light_data_pixels.data(), 0, light_data_pixels.size());

    for (int i = 0; i < light_total; i++)
    {
        const LightGridLight &light = grid->lights[i];

        uint8_t *row0 = light_data_pixels.data() + (size_t)(0 * kGles2LightDataWidth + i) * 4;
        uint8_t *row1 = light_data_pixels.data() + (size_t)(1 * kGles2LightDataWidth + i) * 4;
        uint8_t *row2 = light_data_pixels.data() + (size_t)(2 * kGles2LightDataWidth + i) * 4;

        EncodeUnitPair(row0 + 0,
                       (light.eye_position.X - light_grid_state.bounds_minimum[0]) / light_grid_state.bounds_range[0]);
        EncodeUnitPair(row0 + 2,
                       (light.eye_position.Y - light_grid_state.bounds_minimum[1]) / light_grid_state.bounds_range[1]);
        EncodeUnitPair(row1 + 0,
                       (light.eye_position.Z - light_grid_state.bounds_minimum[2]) / light_grid_state.bounds_range[2]);
        EncodeUnitPair(row1 + 2, light.radius / light_grid_state.radius_scale);

        float red   = light.color.X;
        float green = light.color.Y;
        float blue  = light.color.Z;

        row2[0] = (uint8_t)HMM_Clamp(0.0f, red, 255.0f);
        row2[1] = (uint8_t)HMM_Clamp(0.0f, green, 255.0f);
        row2[2] = (uint8_t)HMM_Clamp(0.0f, blue, 255.0f);
        row2[3] = (light.additive > 0.5f) ? 255 : 0;
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, light_grid_state.data_texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kGles2LightDataWidth, kGles2LightDataHeight, GL_RGBA, GL_UNSIGNED_BYTE,
                    light_data_pixels.data());

    int wanted_header_width  = NextPowerOfTwo(grid->tiles_x);
    int wanted_header_height = NextPowerOfTwo(grid->tiles_y);

    bool header_resized = (wanted_header_width != header_texture_width) ||
                          (wanted_header_height != header_texture_height);

    if (header_resized)
    {
        header_texture_width  = wanted_header_width;
        header_texture_height = wanted_header_height;

        header_pixels.assign((size_t)header_texture_width * header_texture_height * 4, 0);

        glBindTexture(GL_TEXTURE_2D, light_grid_state.header_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, header_texture_width, header_texture_height, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
    }
    else
        memset(header_pixels.data(), 0, header_pixels.size());

    for (int tile_y = 0; tile_y < grid->tiles_y; tile_y++)
    {
        for (int tile_x = 0; tile_x < grid->tiles_x; tile_x++)
        {
            int tile = tile_y * grid->tiles_x + tile_x;

            uint32_t offset = grid->tile_offsets[(size_t)tile];
            uint32_t count  = grid->tile_counts[(size_t)tile];

            uint8_t *texel = header_pixels.data() + ((size_t)tile_y * header_texture_width + tile_x) * 4;

            texel[0] = (uint8_t)((offset >> 16) & 0xFF);
            texel[1] = (uint8_t)((offset >> 8) & 0xFF);
            texel[2] = (uint8_t)(offset & 0xFF);
            texel[3] = (uint8_t)count;
        }
    }

    glBindTexture(GL_TEXTURE_2D, light_grid_state.header_texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, header_texture_width, header_texture_height, GL_RGBA, GL_UNSIGNED_BYTE,
                    header_pixels.data());

    size_t entry_total = grid->tile_list.size();

    int wanted_list_width  = 256;
    int wanted_list_height = NextPowerOfTwo((int)((entry_total + 255) / 256) + 1);

    bool list_resized = (wanted_list_width != list_texture_width) || (wanted_list_height != list_texture_height);

    if (list_resized)
    {
        list_texture_width  = wanted_list_width;
        list_texture_height = wanted_list_height;

        list_pixels.assign((size_t)list_texture_width * list_texture_height, 0);

        glBindTexture(GL_TEXTURE_2D, light_grid_state.list_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, list_texture_width, list_texture_height, 0, GL_LUMINANCE,
                     GL_UNSIGNED_BYTE, nullptr);
    }
    else
        memset(list_pixels.data(), 0, list_pixels.size());

    size_t list_capacity = (size_t)list_texture_width * list_texture_height;

    for (size_t entry = 0; entry < entry_total && entry < list_capacity; entry++)
        list_pixels[entry] = grid->tile_list[entry];

    glBindTexture(GL_TEXTURE_2D, light_grid_state.list_texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, list_texture_width, list_texture_height, GL_LUMINANCE, GL_UNSIGNED_BYTE,
                    list_pixels.data());

    glBindTexture(GL_TEXTURE_2D, 0);

    light_grid_state.view_origin[0] = (float)grid->view_x;
    light_grid_state.view_origin[1] = (float)grid->view_y;

    light_grid_state.header_texel_step[0] = 1.0f / (float)header_texture_width;
    light_grid_state.header_texel_step[1] = 1.0f / (float)header_texture_height;

    light_grid_state.list_width         = (float)list_texture_width;
    light_grid_state.list_texel_step[0] = 1.0f / (float)list_texture_width;
    light_grid_state.list_texel_step[1] = 1.0f / (float)list_texture_height;

    light_grid_state.active = true;
}
