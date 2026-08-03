#pragma once

#include <SDL3/SDL.h>
#include <stddef.h>
#include <stdint.h>

#include "HandmadeMath.h"

constexpr int32_t kGpuMaximumClipPlanes = 6;

constexpr uint32_t kGpuAttributePosition        = 0;
constexpr uint32_t kGpuAttributeTextureCoords   = 1;
constexpr uint32_t kGpuAttributeColor           = 2;

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
    float    vertex_padding[2];
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

static_assert(offsetof(GpuVertexParameters, mvp) == 0, "GpuVertexParameters::mvp offset");
static_assert(offsetof(GpuVertexParameters, tm) == 64, "GpuVertexParameters::tm offset");
static_assert(offsetof(GpuVertexParameters, mv) == 128, "GpuVertexParameters::mv offset");
static_assert(offsetof(GpuVertexParameters, clipplane) == 192, "GpuVertexParameters::clipplane offset");
static_assert(offsetof(GpuVertexParameters, sky_pass) == 288, "GpuVertexParameters::sky_pass offset");
static_assert(offsetof(GpuVertexParameters, sky_fog_depth) == 292, "GpuVertexParameters::sky_fog_depth offset");
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

bool CreateWorldShaders(SDL_GPUDevice *device);

bool CreateMovieShaders(SDL_GPUDevice *device);

void DestroyMovieShaders(SDL_GPUDevice *device);

SDL_GPUShader *MovieVertexShader(void);

SDL_GPUShader *MovieFragmentShader(void);

void DestroyWorldShaders(SDL_GPUDevice *device);

SDL_GPUShader *WorldVertexShader();

SDL_GPUShader *WorldFragmentShader();
