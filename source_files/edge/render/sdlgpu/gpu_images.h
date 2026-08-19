#pragma once

#include <SDL3/SDL.h>
#include <stdint.h>

#include "i_defs_gl.h"

struct GpuImage
{
    SDL_GPUTexture *texture;
    SDL_GPUSampler *sampler;

    SDL_GPUTransferBuffer *update_buffer;

    int32_t width;
    int32_t height;
    int32_t levels;

    int64_t update_frame;

    bool external;
};

struct GpuImageLevel
{
    int32_t     width;
    int32_t     height;
    const void *pixels;
};

bool CreateGpuImage(SDL_GPUDevice *device, GLuint id, const GpuImageLevel *levels, int32_t level_count,
                    const SDL_GPUSamplerCreateInfo *sampler_info);

bool CreateGpuCubemap(SDL_GPUDevice *device, GLuint id, const GpuImageLevel faces[6]);

bool UpdateGpuImage(SDL_GPUDevice *device, GLuint id, int32_t width, int32_t height, const void *pixels);

void DeleteGpuImage(GLuint id);

void FlushDeletedGpuImages(SDL_GPUDevice *device);

void ShutdownGpuImages(SDL_GPUDevice *device);

constexpr GLuint kGpuImageOitAccumulation = 0xFFFFFF01u;
constexpr GLuint kGpuImageOitRevealage    = 0xFFFFFF02u;

bool RegisterGpuExternalImage(SDL_GPUDevice *device, GLuint id, SDL_GPUTexture *texture, int32_t width,
                              int32_t height);

void ForgetGpuExternalImage(SDL_GPUDevice *device, GLuint id);

const GpuImage *GetGpuImage(GLuint id);

const GpuImage *GetDefaultGpuCubemap(SDL_GPUDevice *device);

GLuint AllocateGpuCubemapId(void);
