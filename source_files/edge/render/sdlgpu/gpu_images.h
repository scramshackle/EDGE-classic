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
};

struct GpuImageLevel
{
    int32_t     width;
    int32_t     height;
    const void *pixels;
};

bool CreateGpuImage(SDL_GPUDevice *device, GLuint id, const GpuImageLevel *levels, int32_t level_count,
                    const SDL_GPUSamplerCreateInfo *sampler_info);

bool UpdateGpuImage(SDL_GPUDevice *device, GLuint id, int32_t width, int32_t height, const void *pixels);

void DeleteGpuImage(GLuint id);

void FlushDeletedGpuImages(SDL_GPUDevice *device);

void ShutdownGpuImages(SDL_GPUDevice *device);

const GpuImage *GetGpuImage(GLuint id);
