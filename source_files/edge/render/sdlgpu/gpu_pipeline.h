#pragma once

#include <SDL3/SDL.h>
#include <stdint.h>

#include "i_defs_gl.h"

enum GpuPipelineFlag
{
    kGpuPipelineDepthTest                           = 1 << 0,
    kGpuPipelineDepthWrite                          = 1 << 1,
    kGpuPipelineDepthGreater                        = 1 << 2,
    kGpuPipelineBlend                               = 1 << 3,
    kGpuPipelineBlendSourceSourceAlpha              = 1 << 4,
    kGpuPipelineBlendSourceOneMinusDestinationColor = 1 << 5,
    kGpuPipelineBlendSourceDestinationColor         = 1 << 6,
    kGpuPipelineBlendSourceZero                     = 1 << 7,
    kGpuPipelineBlendDestinationOne                 = 1 << 8,
    kGpuPipelineBlendDestinationOneMinusSourceAlpha = 1 << 9,
    kGpuPipelineBlendDestinationSourceColor         = 1 << 10,
    kGpuPipelineBlendDestinationZero                = 1 << 11,
    kGpuPipelineCullFront                           = 1 << 12,
    kGpuPipelineCullBack                            = 1 << 13
};

enum GpuPrimitiveType
{
    kGpuPrimitiveTriangleList = 0,
    kGpuPrimitiveTriangleStrip,
    kGpuPrimitiveLineList,
    kGpuPrimitiveTypeTotal
};

bool InitPipelines(SDL_GPUDevice *device, SDL_GPUTextureFormat color_format, SDL_GPUTextureFormat depth_format);

void ShutdownPipelines(SDL_GPUDevice *device);

SDL_GPUGraphicsPipeline *GetMoviePipeline(void);

SDL_GPUGraphicsPipeline *GetPipeline(uint32_t pipeline_flags, GLenum source_blend, GLenum destination_blend,
                                     GpuPrimitiveType primitive);
