//----------------------------------------------------------------------------
//  EDGE Movie Rendering (SDL3_GPU)
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

#include <SDL3/SDL.h>

#include <vector>

#include "epi.h"
#include "gpu_device.h"
#include "gpu_immediate.h"
#include "i_system.h"
#include "r_movie.h"
#include "r_units.h"

static SDL_GPUTexture        *movie_planes[3]      = {nullptr, nullptr, nullptr};
static SDL_GPUSampler        *movie_sampler        = nullptr;
static SDL_GPUTransferBuffer *movie_transfer       = nullptr;
static size_t                 movie_transfer_bytes = 0;
static MoviePlaneSizes        movie_sizes          = {0, 0, 0, 0};

static std::vector<uint8_t> movie_staging;
static bool                 movie_staging_dirty = false;

static SDL_GPUTexture *CreatePlaneTexture(SDL_GPUDevice *device, int width, int height)
{
    SDL_GPUTextureCreateInfo info;
    EPI_CLEAR_MEMORY(&info, SDL_GPUTextureCreateInfo, 1);

    info.type                 = SDL_GPU_TEXTURETYPE_2D;
    info.format               = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
    info.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width                = (uint32_t)width;
    info.height               = (uint32_t)height;
    info.layer_count_or_depth = 1;
    info.num_levels           = 1;
    info.sample_count         = SDL_GPU_SAMPLECOUNT_1;

    SDL_GPUTexture *texture = SDL_CreateGPUTexture(device, &info);

    if (!texture)
        LogPrint("GpuMovie: SDL_CreateGPUTexture failed: %s\n", SDL_GetError());

    return texture;
}

void MovieSetupPlanes(const MoviePlaneSizes &sizes)
{
    MovieReleasePlanes();

    SDL_GPUDevice *device = gpu_device.Handle();

    if (!device)
        return;

    movie_sizes = sizes;

    movie_planes[0] = CreatePlaneTexture(device, sizes.luma_width, sizes.luma_height);
    movie_planes[1] = CreatePlaneTexture(device, sizes.chroma_width, sizes.chroma_height);
    movie_planes[2] = CreatePlaneTexture(device, sizes.chroma_width, sizes.chroma_height);

    SDL_GPUSamplerCreateInfo sampler_info;
    EPI_CLEAR_MEMORY(&sampler_info, SDL_GPUSamplerCreateInfo, 1);

    sampler_info.min_filter     = SDL_GPU_FILTER_LINEAR;
    sampler_info.mag_filter     = SDL_GPU_FILTER_LINEAR;
    sampler_info.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

    movie_sampler = SDL_CreateGPUSampler(device, &sampler_info);

    size_t luma_bytes   = (size_t)sizes.luma_width * (size_t)sizes.luma_height;
    size_t chroma_bytes = (size_t)sizes.chroma_width * (size_t)sizes.chroma_height;

    movie_transfer_bytes = luma_bytes + chroma_bytes * 2;

    SDL_GPUTransferBufferCreateInfo transfer_info;
    EPI_CLEAR_MEMORY(&transfer_info, SDL_GPUTransferBufferCreateInfo, 1);

    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size  = (uint32_t)movie_transfer_bytes;

    movie_transfer = SDL_CreateGPUTransferBuffer(device, &transfer_info);

    movie_staging.assign(movie_transfer_bytes, 0);
    movie_staging_dirty = false;

    if (!movie_transfer)
        LogPrint("GpuMovie: SDL_CreateGPUTransferBuffer failed: %s\n", SDL_GetError());

}

void MovieUploadPlanes(const uint8_t *luma, const uint8_t *chroma_blue, const uint8_t *chroma_red)
{
    if (!movie_planes[0] || movie_staging.empty())
        return;

    size_t luma_bytes   = (size_t)movie_sizes.luma_width * (size_t)movie_sizes.luma_height;
    size_t chroma_bytes = (size_t)movie_sizes.chroma_width * (size_t)movie_sizes.chroma_height;

    memcpy(movie_staging.data(), luma, luma_bytes);
    memcpy(movie_staging.data() + luma_bytes, chroma_blue, chroma_bytes);
    memcpy(movie_staging.data() + luma_bytes + chroma_bytes, chroma_red, chroma_bytes);

    movie_staging_dirty = true;
}

static void FlushStagedPlanes(void)
{
    SDL_GPUDevice *device = gpu_device.Handle();

    if (!device || !movie_staging_dirty || !movie_transfer || !gpu_device.FrameAcquired())
        return;

    movie_staging_dirty = false;

    size_t luma_bytes   = (size_t)movie_sizes.luma_width * (size_t)movie_sizes.luma_height;
    size_t chroma_bytes = (size_t)movie_sizes.chroma_width * (size_t)movie_sizes.chroma_height;

    uint8_t *mapped = (uint8_t *)SDL_MapGPUTransferBuffer(device, movie_transfer, true);

    if (!mapped)
        return;

    memcpy(mapped, movie_staging.data(), movie_staging.size());

    SDL_UnmapGPUTransferBuffer(device, movie_transfer);

    SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(gpu_device.CommandBuffer());

    size_t offsets[3] = {0, luma_bytes, luma_bytes + chroma_bytes};
    int    widths[3]  = {movie_sizes.luma_width, movie_sizes.chroma_width, movie_sizes.chroma_width};
    int    heights[3] = {movie_sizes.luma_height, movie_sizes.chroma_height, movie_sizes.chroma_height};

    for (int i = 0; i < 3; i++)
    {
        SDL_GPUTextureTransferInfo source;
        EPI_CLEAR_MEMORY(&source, SDL_GPUTextureTransferInfo, 1);
        source.transfer_buffer = movie_transfer;
        source.offset          = (uint32_t)offsets[i];
        source.pixels_per_row  = (uint32_t)widths[i];
        source.rows_per_layer  = (uint32_t)heights[i];

        SDL_GPUTextureRegion destination;
        EPI_CLEAR_MEMORY(&destination, SDL_GPUTextureRegion, 1);
        destination.texture = movie_planes[i];
        destination.w       = (uint32_t)widths[i];
        destination.h       = (uint32_t)heights[i];
        destination.d       = 1;

        SDL_UploadToGPUTexture(copy_pass, &source, &destination, true);
    }

    SDL_EndGPUCopyPass(copy_pass);
}

void MovieDrawFrame(float x1, float y1, float x2, float y2, float luma_scale_x, float luma_scale_y,
                    float chroma_scale_x, float chroma_scale_y)
{
    if (!movie_planes[0] || !movie_sampler)
        return;

    FlushStagedPlanes();

    RendererVertex *quad = gpu_immediate.ReserveVertices(4);

    if (!quad)
        return;

    EPI_CLEAR_MEMORY(quad, RendererVertex, 4);

    quad[0].position               = {{x1, y1, 0.0f}};
    quad[0].texture_coordinates[0] = {{0.0f, 0.0f}};
    quad[1].position               = {{x2, y1, 0.0f}};
    quad[1].texture_coordinates[0] = {{1.0f, 0.0f}};
    quad[2].position               = {{x2, y2, 0.0f}};
    quad[2].texture_coordinates[0] = {{1.0f, 1.0f}};
    quad[3].position               = {{x1, y2, 0.0f}};
    quad[3].texture_coordinates[0] = {{0.0f, 1.0f}};

    float plane_scales[4] = {luma_scale_x, luma_scale_y, chroma_scale_x, chroma_scale_y};

    gpu_immediate.RecordMovieDraw(movie_planes[0], movie_planes[1], movie_planes[2], movie_sampler, plane_scales);
}

void MovieReleasePlanes(void)
{
    SDL_GPUDevice *device = gpu_device.Handle();

    if (!device)
        return;

    for (int i = 0; i < 3; i++)
    {
        if (movie_planes[i])
        {
            SDL_ReleaseGPUTexture(device, movie_planes[i]);
            movie_planes[i] = nullptr;
        }
    }

    if (movie_sampler)
    {
        SDL_ReleaseGPUSampler(device, movie_sampler);
        movie_sampler = nullptr;
    }

    if (movie_transfer)
    {
        SDL_ReleaseGPUTransferBuffer(device, movie_transfer);
        movie_transfer = nullptr;
    }

    movie_transfer_bytes = 0;
    movie_sizes          = {0, 0, 0, 0};

    movie_staging.clear();
    movie_staging_dirty = false;
}

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
