//----------------------------------------------------------------------------
//  EDGE GPU Rendering (Unit system)
//----------------------------------------------------------------------------
//
//  Copyright (c) 1999-2024 The EDGE Team.
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 3
//  of the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//----------------------------------------------------------------------------
//
//  Based on the DOOM source code, released by Id Software under the
//  following copyright:
//
//    Copyright (C) 1993-1996 by id Software, Inc.
//
//----------------------------------------------------------------------------

#pragma once

#include "epi_color.h"
#include "epi_vector.h"
#include "i_defs_gl.h"

constexpr uint16_t kDummyClamp             = 789;
constexpr uint8_t  kMaximumPolygonVertices = 64;
constexpr uint16_t kMaximumLocalVertices   = 65535;
constexpr uint16_t kDefaultAutomapLines    = kMaximumLocalVertices / 2;

// a single vertex to pass to the GPU
struct RendererVertex
{
    RGBAColor rgba;
    epi::Vec3 position;
    epi::Vec2 texture_coordinates[2];
};

constexpr int kSpriteLightLevels = 32;

struct SpriteInstance
{
    float     origin[4];
    float     extent[4];
    float     texture_coordinates[4];
    float     fuzz[4];
    float     light[2];
    RGBAColor rgba;
    uint32_t  padding;
};

struct SpriteLightTable
{
    float whites[kSpriteLightLevels][4];
    float parameters[4];
};

constexpr float kSpriteInstanceMirrorFlip = 1.0f;
constexpr float kSpriteInstanceFuzzy      = 2.0f;

extern epi::Vec4 render_unit_sprite_view[2];

extern RGBAColor culling_fog_color;

extern float    static_batch_light_row_offset;

extern bool     render_unit_whiten;
extern int      render_unit_filter;
extern epi::Vec4 render_unit_blur;
extern epi::Vec4 render_unit_liquid;

void StartUnitBatch(bool sort_em);
void FinishUnitBatch(void);
void RenderCurrentUnits(void);

void BeginRetainedUnits(void);
void EndRetainedUnits(void);
void ReplayRetainedUnits(void);

enum BlendingMode
{
    kBlendingNone = 0,

    kBlendingMasked = (1 << 0),         // drop fragments when alpha == 0
    kBlendingLess   = (1 << 1),         // drop fragments when alpha < color.a
    kBlendingAlpha  = (1 << 2),         // alpha-blend with the framebuffer
    kBlendingAdd    = (1 << 3),         // additive-blend with the framebuffer

    kBlendingCullBack  = (1 << 4),      // enable back-face culling
    kBlendingCullFront = (1 << 5),      // enable front-face culling
    kBlendingNoZBuffer = (1 << 6),      // don't update the Z buffer
    kBlendingClampY    = (1 << 7),      // force texture to be Y clamped

    kBlendingNoFog = (1 << 8),          // force disable fog (including culling fog)

    kBlendingRepeatX = (1 << 9),        // force texture to repeat on X axis
    kBlendingRepeatY = (1 << 10),       // force texture to repeat on Y axis

    kBlendingGEqual = (1 << 11),        // drop fragments when alpha >= 1.0f - color.a
                                        // Dasho - This is super specific and only
                                        // used by the "pixelfade" wipe :/

    kBlendingInvert        = (1 << 12), // color inversion (simple invuln fx)
    kBlendingNegativeGamma = (1 << 13),
    kBlendingPositiveGamma = (1 << 14)
};

enum CustomTextureEnvironment
{
    kTextureEnvironmentDisable,
    // the texture unit is disabled (complete pass-through).

    kTextureEnvironmentSkipRGB,
    // causes the RGB of the texture to be skipped, i.e. the
    // output of the texture unit is the same as the input
    // for the RGB components.  The alpha component is treated
    // normally, i.e. passed on to next texture unit.

    kTextureEnvironmentLightFalloff
};

struct ModelMeshData
{
    const float *frame_positions    = nullptr;
    const float *frame_normals      = nullptr;
    const float *texture_coordinates = nullptr;

    int frame_count  = 0;
    int vertex_count = 0;
};

struct ModelDrawInfo
{
    uint32_t handle = 0;

    int   frame1 = 0;
    int   frame2 = 0;
    float lerp   = 0.0f;

    epi::Mat4 transform = epi::IdentityMatrix();

    float alpha      = 1.0f;
    float alpha_test = 0.0f;

    epi::Vec2 texture_scale  = {1.0f, 1.0f};
    epi::Vec2 texture_offset = {0.0f, 0.0f};

    int first_vertex = 0;
    int vertex_count = 0;
    int first_index  = 0;
    int index_count  = 0;

    bool world_lit = false;
    int  glow_set  = -1;

    int color_lookup = 0;

    const SpriteLightTable *light_table = nullptr;

    float    light_level       = 255.0f;
    float    light_fixed_depth = 0.0f;
    bool     light_depth_fixed = false;
    bool     fuzzy             = false;
    epi::Vec3 tint              = {1.0f, 1.0f, 1.0f};
};

struct RendererScissor
{
    int32_t x      = 0;
    int32_t y      = 0;
    int32_t width  = 0;
    int32_t height = 0;
};

struct SkyPassInfo
{
    epi::Mat4 inverse_projection = epi::IdentityMatrix();
    epi::Mat4 inverse_view       = epi::IdentityMatrix();
    epi::Vec2 viewport_origin    = {0.0f, 0.0f};
    epi::Vec2 viewport_size      = {1.0f, 1.0f};
    int      stretch_mode       = 0;
    float    ty                 = 2.0f;
    float    u_scale            = 1.0f;
    float    u_offset           = 0.0f;
    float    v_offset           = 0.0f;
    float    fog_depth          = 0.0f;
    float    vertical_fov_slope = 1.0f;
    float    horizon_shift      = 0.0f;
    GLuint   cube_texture       = 0;
    int      is_box             = 0;
    int      is_geometry        = 0;
};

RendererVertex *BeginRenderUnit(GLuint shape, int max_vert, GLuint env1, GLuint tex1, GLuint env2, GLuint tex2,
                                int pass, BlendingMode blending, RGBAColor fog_color = kRGBANoValue,
                                float fog_density = 0, const SkyPassInfo *sky_pass = nullptr,
                                const RendererScissor *scissor = nullptr, bool light_depth = false,
                                bool world_lit = false, int glow_set = -1);
void            EndRenderUnit(int actual_vert);

uint32_t CreateStaticVertexBuffer(const RendererVertex *vertices, int count);
uint32_t CreateStaticVertexBufferWithCapacity(const RendererVertex *vertices, int count, int capacity);
void     UpdateStaticVertexBuffer(uint32_t handle, int first, const RendererVertex *vertices, int count);
void     FlushStaticVertexUploads(void);
void     DeleteStaticVertexBuffer(uint32_t handle);
void     AddStaticRenderUnit(uint32_t handle, GLuint shape, int first, int count, GLuint env1, GLuint tex1, GLuint env2,
                             GLuint tex2, int pass, BlendingMode blending, RGBAColor fog_color, float fog_density,
                             const SkyPassInfo *sky_pass = nullptr, bool world_lit = false, int glow_set = -1);

SpriteInstance *ReserveSpriteInstances(int count, int *first);
void            AddSpriteRenderUnit(int first, int count, GLuint texture, GLuint fuzz_texture, BlendingMode blending,
                                    RGBAColor fog_color, float fog_density, int color_lookup, bool world_lit,
                                    int glow_set, const SpriteLightTable *light_table, uint8_t alpha,
                                    uint32_t buffer = 0);
uint32_t        CreateSpriteInstanceBuffer(const SpriteInstance *instances, int count, int capacity);
void            UpdateSpriteInstanceBuffer(uint32_t handle, int first, const SpriteInstance *instances, int count);

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
