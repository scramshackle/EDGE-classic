#pragma once

#include <SDL3/SDL.h>

#include "i_defs_gl.h"

struct GpuImage
{
    SDL_GPUTexture *texture;
    SDL_GPUSampler *sampler;
};

void RegisterGpuImage(GLuint id, SDL_GPUTexture *texture, SDL_GPUSampler *sampler);

void DeleteGpuImage(SDL_GPUDevice *device, GLuint id);

void ShutdownGpuImages(SDL_GPUDevice *device);

const GpuImage *GetGpuImage(GLuint id);
