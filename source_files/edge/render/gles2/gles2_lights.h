#pragma once

#include <stdint.h>

#include "HandmadeMath.h"
#include "i_defs_gl.h"

struct LightGrid;

struct Gles2LightGridState
{
    bool active = false;

    GLuint data_texture   = 0;
    GLuint header_texture = 0;
    GLuint list_texture   = 0;

    float view_origin[2] = {0.0f, 0.0f};

    float header_texel_step[2] = {0.0f, 0.0f};

    float list_width      = 1.0f;
    float list_texel_step[2] = {0.0f, 0.0f};

    float data_texel_step = 0.0f;

    float bounds_minimum[3] = {0.0f, 0.0f, 0.0f};
    float bounds_range[3]   = {1.0f, 1.0f, 1.0f};

    float radius_scale = 1.0f;
};

void Gles2CreateLightGridTextures(void);

void Gles2DestroyLightGridTextures(void);

void Gles2UploadLightGrid(const LightGrid *grid);

const Gles2LightGridState *Gles2CurrentLightGrid(void);
