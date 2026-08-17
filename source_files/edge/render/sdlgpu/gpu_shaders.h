#pragma once

#include <SDL3/SDL.h>
#include <stddef.h>
#include <stdint.h>

#include "HandmadeMath.h"
#include "r_units.h"

constexpr int32_t kGpuMaximumClipPlanes = 6;

constexpr uint32_t kGpuAttributePosition        = 0;
constexpr uint32_t kGpuAttributeTextureCoords   = 1;
constexpr uint32_t kGpuAttributeColor           = 2;

constexpr uint32_t kGpuAttributeModelPositionFrame1     = 0;
constexpr uint32_t kGpuAttributeModelPositionFrame2     = 1;
constexpr uint32_t kGpuAttributeModelTextureCoordinates = 2;
constexpr uint32_t kGpuAttributeModelColor              = 3;

constexpr uint32_t kGpuModelBufferSlotPositionFrame1     = 0;
constexpr uint32_t kGpuModelBufferSlotPositionFrame2     = 1;
constexpr uint32_t kGpuModelBufferSlotTextureCoordinates = 2;
constexpr uint32_t kGpuModelBufferSlotColor              = 3;

constexpr uint32_t kGpuVertexUniformSlot   = 0;
constexpr uint32_t kGpuFragmentUniformSlot = 0;

constexpr uint32_t kGpuSamplerSlotTexture0 = 0;
constexpr uint32_t kGpuSamplerSlotTexture1 = 1;

constexpr uint32_t kGpuSamplerSlotMovieLuma       = 0;
constexpr uint32_t kGpuSamplerSlotMovieChromaBlue = 1;
constexpr uint32_t kGpuSamplerSlotMovieChromaRed  = 2;

struct GpuMovieVertexParameters
{
    HMM_Mat4 mvp;
};

struct GpuMovieFragmentParameters
{
    float plane_scales[4];
};

enum GpuFragmentFlag
{
    kGpuFragmentFlagMultiTexture = (1 << 0),
    kGpuFragmentFlagLine         = (1 << 1),
    kGpuFragmentFlagSkipRGB      = (1 << 2),
    kGpuFragmentFlagSkyPass      = (1 << 3)
};

enum GpuFogMode
{
    kGpuFogModeNone        = 0,
    kGpuFogModeLinear      = 1,
    kGpuFogModeExponential = 2
};

struct GpuVertexParameters
{
    HMM_Mat4 mvp;
    HMM_Mat4 tm;
    HMM_Mat4 mv;
    float    clipplane[kGpuMaximumClipPlanes][4];
    float    sky_pass;
    float    sky_fog_depth;
    float    light_depth;
    float    vertex_padding;
};

struct GpuFragmentParameters
{
    int32_t flags;
    float   alpha_test;
    int32_t clipplanes;
    int32_t fog_mode;
    float   fog_color[4];
    float   fog_density;
    float   fog_start;
    float   fog_end;
    float   fog_scale;

    HMM_Mat4 sky_inverse_projection;
    HMM_Mat4 sky_inverse_view;
    float    sky_viewport[4];
    float    sky_stretch_mode;
    float    sky_u_scale;
    float    sky_ty;
    float    sky_u_offset;
    float    sky_v_offset;
    float    sky_vertical_fov_slope;
    float    sky_horizon_shift;
    float    sky_padding;
};

struct GpuLightVertexParameters
{
    HMM_Mat4 mvp;
};

struct GpuLightFragmentParameters
{
    float surface_normal[4];
    float light_position_radius[kMaximumLightsPerPass][4];
    float light_color[kMaximumLightsPerPass][4];

    int32_t light_count;
    float   normal_horizontal;
    float   surface_mode;
    float   alpha;

    float alpha_test;
    float light_padding0;
    float light_padding1;
    float light_padding2;
};

struct GpuModelVertexParameters
{
    HMM_Mat4 mvp;
    HMM_Mat4 mv;
    HMM_Mat4 model_transform;
    float    clipplane[kGpuMaximumClipPlanes][4];
    float    lerp;
    float    vertex_padding0;
    float    texture_scale[2];
    float    texture_offset[2];
    float    vertex_padding1[2];
};

struct GpuModelFragmentParameters
{
    int32_t clipplanes;
    float   alpha;
    float   alpha_test;
    float   additive_pass;

    int32_t fog_mode;
    float   fog_density;
    float   fog_start;
    float   fog_end;

    float fog_color[4];
};

static_assert(kMaximumLightsPerPass == 4, "GpuLightFragmentParameters assumes 4 lights per pass");
static_assert(sizeof(GpuLightVertexParameters) == 64, "GpuLightVertexParameters size");
static_assert(offsetof(GpuLightFragmentParameters, surface_normal) == 0,
              "GpuLightFragmentParameters::surface_normal offset");
static_assert(offsetof(GpuLightFragmentParameters, light_position_radius) == 16,
              "GpuLightFragmentParameters::light_position_radius offset");
static_assert(offsetof(GpuLightFragmentParameters, light_color) == 80,
              "GpuLightFragmentParameters::light_color offset");
static_assert(offsetof(GpuLightFragmentParameters, light_count) == 144,
              "GpuLightFragmentParameters::light_count offset");
static_assert(offsetof(GpuLightFragmentParameters, normal_horizontal) == 148,
              "GpuLightFragmentParameters::normal_horizontal offset");
static_assert(offsetof(GpuLightFragmentParameters, surface_mode) == 152,
              "GpuLightFragmentParameters::surface_mode offset");
static_assert(offsetof(GpuLightFragmentParameters, alpha) == 156, "GpuLightFragmentParameters::alpha offset");
static_assert(offsetof(GpuLightFragmentParameters, alpha_test) == 160, "GpuLightFragmentParameters::alpha_test offset");
static_assert(sizeof(GpuLightFragmentParameters) == 176, "GpuLightFragmentParameters size");

static_assert(offsetof(GpuModelVertexParameters, mvp) == 0, "GpuModelVertexParameters::mvp offset");
static_assert(offsetof(GpuModelVertexParameters, mv) == 64, "GpuModelVertexParameters::mv offset");
static_assert(offsetof(GpuModelVertexParameters, model_transform) == 128,
              "GpuModelVertexParameters::model_transform offset");
static_assert(offsetof(GpuModelVertexParameters, clipplane) == 192, "GpuModelVertexParameters::clipplane offset");
static_assert(offsetof(GpuModelVertexParameters, lerp) == 288, "GpuModelVertexParameters::lerp offset");
static_assert(offsetof(GpuModelVertexParameters, texture_scale) == 296,
              "GpuModelVertexParameters::texture_scale offset");
static_assert(offsetof(GpuModelVertexParameters, texture_offset) == 304,
              "GpuModelVertexParameters::texture_offset offset");
static_assert(sizeof(GpuModelVertexParameters) == 320, "GpuModelVertexParameters size");

static_assert(offsetof(GpuModelFragmentParameters, clipplanes) == 0, "GpuModelFragmentParameters::clipplanes offset");
static_assert(offsetof(GpuModelFragmentParameters, alpha) == 4, "GpuModelFragmentParameters::alpha offset");
static_assert(offsetof(GpuModelFragmentParameters, alpha_test) == 8, "GpuModelFragmentParameters::alpha_test offset");
static_assert(offsetof(GpuModelFragmentParameters, additive_pass) == 12,
              "GpuModelFragmentParameters::additive_pass offset");
static_assert(offsetof(GpuModelFragmentParameters, fog_mode) == 16, "GpuModelFragmentParameters::fog_mode offset");
static_assert(offsetof(GpuModelFragmentParameters, fog_density) == 20,
              "GpuModelFragmentParameters::fog_density offset");
static_assert(offsetof(GpuModelFragmentParameters, fog_start) == 24, "GpuModelFragmentParameters::fog_start offset");
static_assert(offsetof(GpuModelFragmentParameters, fog_end) == 28, "GpuModelFragmentParameters::fog_end offset");
static_assert(offsetof(GpuModelFragmentParameters, fog_color) == 32, "GpuModelFragmentParameters::fog_color offset");
static_assert(sizeof(GpuModelFragmentParameters) == 48, "GpuModelFragmentParameters size");

static_assert(offsetof(GpuVertexParameters, mvp) == 0, "GpuVertexParameters::mvp offset");
static_assert(offsetof(GpuVertexParameters, tm) == 64, "GpuVertexParameters::tm offset");
static_assert(offsetof(GpuVertexParameters, mv) == 128, "GpuVertexParameters::mv offset");
static_assert(offsetof(GpuVertexParameters, clipplane) == 192, "GpuVertexParameters::clipplane offset");
static_assert(offsetof(GpuVertexParameters, sky_pass) == 288, "GpuVertexParameters::sky_pass offset");
static_assert(offsetof(GpuVertexParameters, sky_fog_depth) == 292, "GpuVertexParameters::sky_fog_depth offset");
static_assert(offsetof(GpuVertexParameters, light_depth) == 296, "GpuVertexParameters::light_depth offset");
static_assert(sizeof(GpuVertexParameters) == 304, "GpuVertexParameters size");

static_assert(offsetof(GpuFragmentParameters, flags) == 0, "GpuFragmentParameters::flags offset");
static_assert(offsetof(GpuFragmentParameters, alpha_test) == 4, "GpuFragmentParameters::alpha_test offset");
static_assert(offsetof(GpuFragmentParameters, clipplanes) == 8, "GpuFragmentParameters::clipplanes offset");
static_assert(offsetof(GpuFragmentParameters, fog_mode) == 12, "GpuFragmentParameters::fog_mode offset");
static_assert(offsetof(GpuFragmentParameters, fog_color) == 16, "GpuFragmentParameters::fog_color offset");
static_assert(offsetof(GpuFragmentParameters, fog_density) == 32, "GpuFragmentParameters::fog_density offset");
static_assert(offsetof(GpuFragmentParameters, fog_start) == 36, "GpuFragmentParameters::fog_start offset");
static_assert(offsetof(GpuFragmentParameters, fog_end) == 40, "GpuFragmentParameters::fog_end offset");
static_assert(offsetof(GpuFragmentParameters, fog_scale) == 44, "GpuFragmentParameters::fog_scale offset");
static_assert(offsetof(GpuFragmentParameters, sky_inverse_projection) == 48,
              "GpuFragmentParameters::sky_inverse_projection offset");
static_assert(offsetof(GpuFragmentParameters, sky_inverse_view) == 112,
              "GpuFragmentParameters::sky_inverse_view offset");
static_assert(offsetof(GpuFragmentParameters, sky_viewport) == 176, "GpuFragmentParameters::sky_viewport offset");
static_assert(offsetof(GpuFragmentParameters, sky_stretch_mode) == 192,
              "GpuFragmentParameters::sky_stretch_mode offset");
static_assert(offsetof(GpuFragmentParameters, sky_horizon_shift) == 216,
              "GpuFragmentParameters::sky_horizon_shift offset");
static_assert(sizeof(GpuFragmentParameters) == 224, "GpuFragmentParameters size");

bool CreateModelShaders(SDL_GPUDevice *device);

void DestroyModelShaders(SDL_GPUDevice *device);

SDL_GPUShader *ModelVertexShader();

SDL_GPUShader *ModelFragmentShader();

bool CreateLightShaders(SDL_GPUDevice *device);

void DestroyLightShaders(SDL_GPUDevice *device);

SDL_GPUShader *LightVertexShader();

SDL_GPUShader *LightFragmentShader();

bool CreateWorldShaders(SDL_GPUDevice *device);

bool CreateMovieShaders(SDL_GPUDevice *device);

void DestroyMovieShaders(SDL_GPUDevice *device);

SDL_GPUShader *MovieVertexShader(void);

SDL_GPUShader *MovieFragmentShader(void);

void DestroyWorldShaders(SDL_GPUDevice *device);

SDL_GPUShader *WorldVertexShader();

SDL_GPUShader *WorldFragmentShader();
