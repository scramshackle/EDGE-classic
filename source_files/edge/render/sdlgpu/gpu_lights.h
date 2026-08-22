#pragma once

#include <SDL3/SDL.h>
#include <stdint.h>

struct LightGrid;

struct GpuLightViewParameters
{
    float light_view[4];
    float light_range[4];
};

void GpuCreateLightBuffers(void);

void GpuDestroyLightBuffers(void);

void GpuResetLightFrame(void);

void GpuUploadLightGrid(const LightGrid *grid);

int GpuCurrentLightView(void);

const GpuLightViewParameters *GpuLightView(int index);

void GpuFlushLightBuffers(void);

void GpuBindLightBuffers(SDL_GPURenderPass *pass);
