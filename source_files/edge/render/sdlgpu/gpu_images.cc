#include "gpu_images.h"

#include <string.h>

#include <unordered_map>
#include <vector>

#include "epi.h"
#include "gpu_device.h"
#include "i_system.h"
#include "r_backend.h"

static constexpr size_t kGpuImagePixelSize = 4;

static constexpr size_t kGpuMaximumSamplers = 256;

struct GpuSamplerEntry
{
    SDL_GPUSamplerCreateInfo info;
    SDL_GPUSampler          *sampler;
};

static std::vector<GpuSamplerEntry> gpu_samplers;

static std::unordered_map<GLuint, GpuImage> gpu_images;

static std::vector<GpuImage> deleted_gpu_images;

static SDL_GPUSampler *GetGpuSampler(SDL_GPUDevice *device, const SDL_GPUSamplerCreateInfo *info)
{
    for (size_t i = 0; i < gpu_samplers.size(); i++)
    {
        if (!memcmp(&gpu_samplers[i].info, info, sizeof(SDL_GPUSamplerCreateInfo)))
            return gpu_samplers[i].sampler;
    }

    if (gpu_samplers.size() == kGpuMaximumSamplers)
        FatalError("GpuImages: sampler overflow\n");

    SDL_GPUSampler *sampler = SDL_CreateGPUSampler(device, info);

    if (!sampler)
    {
        LogPrint("GpuImages: SDL_CreateGPUSampler failed: %s\n", SDL_GetError());
        return nullptr;
    }

    GpuSamplerEntry entry;

    entry.info    = *info;
    entry.sampler = sampler;

    gpu_samplers.push_back(entry);

    return sampler;
}

static size_t GpuImageLevelBytes(const GpuImageLevel *level)
{
    return (size_t)level->width * (size_t)level->height * kGpuImagePixelSize;
}

bool CreateGpuImage(SDL_GPUDevice *device, GLuint id, const GpuImageLevel *levels, int32_t level_count,
                    const SDL_GPUSamplerCreateInfo *sampler_info)
{
    if (!device || !levels || level_count <= 0)
        return false;

    DeleteGpuImage(id);

    SDL_GPUSampler *sampler = GetGpuSampler(device, sampler_info);

    if (!sampler)
        return false;

    SDL_GPUTextureCreateInfo texture_info;
    EPI_CLEAR_MEMORY(&texture_info, SDL_GPUTextureCreateInfo, 1);

    texture_info.type                 = SDL_GPU_TEXTURETYPE_2D;
    texture_info.format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    texture_info.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texture_info.width                = (uint32_t)levels[0].width;
    texture_info.height               = (uint32_t)levels[0].height;
    texture_info.layer_count_or_depth = 1;
    texture_info.num_levels           = (uint32_t)level_count;
    texture_info.sample_count         = SDL_GPU_SAMPLECOUNT_1;

    SDL_GPUTexture *texture = SDL_CreateGPUTexture(device, &texture_info);

    if (!texture)
    {
        LogPrint("GpuImages: SDL_CreateGPUTexture failed: %s\n", SDL_GetError());
        return false;
    }

    size_t total_bytes = 0;

    for (int32_t i = 0; i < level_count; i++)
    {
        if (levels[i].pixels)
            total_bytes += GpuImageLevelBytes(&levels[i]);
    }

    if (total_bytes)
    {
        SDL_GPUTransferBufferCreateInfo transfer_info;
        EPI_CLEAR_MEMORY(&transfer_info, SDL_GPUTransferBufferCreateInfo, 1);

        transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transfer_info.size  = (uint32_t)total_bytes;

        SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(device, &transfer_info);

        if (!transfer)
        {
            LogPrint("GpuImages: SDL_CreateGPUTransferBuffer failed: %s\n", SDL_GetError());
            SDL_ReleaseGPUTexture(device, texture);
            return false;
        }

        uint8_t *mapped = (uint8_t *)SDL_MapGPUTransferBuffer(device, transfer, false);

        if (!mapped)
        {
            LogPrint("GpuImages: SDL_MapGPUTransferBuffer failed: %s\n", SDL_GetError());
            SDL_ReleaseGPUTransferBuffer(device, transfer);
            SDL_ReleaseGPUTexture(device, texture);
            return false;
        }

        size_t offset = 0;

        for (int32_t i = 0; i < level_count; i++)
        {
            if (!levels[i].pixels)
                continue;

            size_t bytes = GpuImageLevelBytes(&levels[i]);

            memcpy(mapped + offset, levels[i].pixels, bytes);

            offset += bytes;
        }

        SDL_UnmapGPUTransferBuffer(device, transfer);

        SDL_GPUCommandBuffer *command_buffer = gpu_device.BeginUpload();

        if (!command_buffer)
        {
            SDL_ReleaseGPUTransferBuffer(device, transfer);
            SDL_ReleaseGPUTexture(device, texture);
            return false;
        }

        SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(command_buffer);

        offset = 0;

        for (int32_t i = 0; i < level_count; i++)
        {
            if (!levels[i].pixels)
                continue;

            SDL_GPUTextureTransferInfo source;
            EPI_CLEAR_MEMORY(&source, SDL_GPUTextureTransferInfo, 1);

            source.transfer_buffer = transfer;
            source.offset          = (uint32_t)offset;
            source.pixels_per_row  = (uint32_t)levels[i].width;
            source.rows_per_layer  = (uint32_t)levels[i].height;

            SDL_GPUTextureRegion destination;
            EPI_CLEAR_MEMORY(&destination, SDL_GPUTextureRegion, 1);

            destination.texture   = texture;
            destination.mip_level = (uint32_t)i;
            destination.w         = (uint32_t)levels[i].width;
            destination.h         = (uint32_t)levels[i].height;
            destination.d         = 1;

            SDL_UploadToGPUTexture(copy_pass, &source, &destination, false);

            offset += GpuImageLevelBytes(&levels[i]);
        }

        SDL_EndGPUCopyPass(copy_pass);

        gpu_device.EndUpload(command_buffer);

        SDL_ReleaseGPUTransferBuffer(device, transfer);
    }

    GpuImage image;

    image.texture       = texture;
    image.sampler       = sampler;
    image.update_buffer = nullptr;
    image.width         = levels[0].width;
    image.height        = levels[0].height;
    image.levels        = level_count;
    image.update_frame  = -1;

    gpu_images[id] = image;

    return true;
}

bool UpdateGpuImage(SDL_GPUDevice *device, GLuint id, int32_t width, int32_t height, const void *pixels)
{
    if (!device)
        return false;

    std::unordered_map<GLuint, GpuImage>::iterator itr = gpu_images.find(id);

    if (itr == gpu_images.end())
        return false;

    GpuImage *image = &itr->second;

    if (image->width != width || image->height != height)
        return false;

    int64_t frame_number = render_backend->GetFrameNumber();

    if (image->update_frame == frame_number)
        FatalError("GpuImages: texture %u updated twice on the same frame\n", id);

    image->update_frame = frame_number;

    size_t bytes = (size_t)width * (size_t)height * kGpuImagePixelSize;

    if (!image->update_buffer)
    {
        SDL_GPUTransferBufferCreateInfo transfer_info;
        EPI_CLEAR_MEMORY(&transfer_info, SDL_GPUTransferBufferCreateInfo, 1);

        transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transfer_info.size  = (uint32_t)bytes;

        image->update_buffer = SDL_CreateGPUTransferBuffer(device, &transfer_info);

        if (!image->update_buffer)
        {
            LogPrint("GpuImages: SDL_CreateGPUTransferBuffer (update) failed: %s\n", SDL_GetError());
            return false;
        }
    }

    void *mapped = SDL_MapGPUTransferBuffer(device, image->update_buffer, true);

    if (!mapped)
    {
        LogPrint("GpuImages: SDL_MapGPUTransferBuffer (update) failed: %s\n", SDL_GetError());
        return false;
    }

    if (pixels)
        memcpy(mapped, pixels, bytes);
    else
        memset(mapped, 0, bytes);

    SDL_UnmapGPUTransferBuffer(device, image->update_buffer);

    SDL_GPUCommandBuffer *command_buffer = gpu_device.BeginUpload();

    if (!command_buffer)
        return false;

    SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(command_buffer);

    SDL_GPUTextureTransferInfo source;
    EPI_CLEAR_MEMORY(&source, SDL_GPUTextureTransferInfo, 1);

    source.transfer_buffer = image->update_buffer;
    source.pixels_per_row  = (uint32_t)width;
    source.rows_per_layer  = (uint32_t)height;

    SDL_GPUTextureRegion destination;
    EPI_CLEAR_MEMORY(&destination, SDL_GPUTextureRegion, 1);

    destination.texture = image->texture;
    destination.w       = (uint32_t)width;
    destination.h       = (uint32_t)height;
    destination.d       = 1;

    SDL_UploadToGPUTexture(copy_pass, &source, &destination, image->levels == 1);

    SDL_EndGPUCopyPass(copy_pass);

    gpu_device.EndUpload(command_buffer);

    return true;
}

void DeleteGpuImage(GLuint id)
{
    std::unordered_map<GLuint, GpuImage>::iterator itr = gpu_images.find(id);

    if (itr == gpu_images.end())
        return;

    deleted_gpu_images.push_back(itr->second);

    gpu_images.erase(itr);
}

void FlushDeletedGpuImages(SDL_GPUDevice *device)
{
    if (device)
    {
        for (size_t i = 0; i < deleted_gpu_images.size(); i++)
        {
            if (deleted_gpu_images[i].texture)
                SDL_ReleaseGPUTexture(device, deleted_gpu_images[i].texture);

            if (deleted_gpu_images[i].update_buffer)
                SDL_ReleaseGPUTransferBuffer(device, deleted_gpu_images[i].update_buffer);
        }
    }

    deleted_gpu_images.clear();
}

void ShutdownGpuImages(SDL_GPUDevice *device)
{
    FlushDeletedGpuImages(device);

    std::unordered_map<GLuint, GpuImage>::iterator itr;

    for (itr = gpu_images.begin(); itr != gpu_images.end(); itr++)
    {
        if (!device)
            continue;

        if (itr->second.texture)
            SDL_ReleaseGPUTexture(device, itr->second.texture);

        if (itr->second.update_buffer)
            SDL_ReleaseGPUTransferBuffer(device, itr->second.update_buffer);
    }

    gpu_images.clear();

    if (device)
    {
        for (size_t i = 0; i < gpu_samplers.size(); i++)
            SDL_ReleaseGPUSampler(device, gpu_samplers[i].sampler);
    }

    gpu_samplers.clear();
}

const GpuImage *GetGpuImage(GLuint id)
{
    std::unordered_map<GLuint, GpuImage>::const_iterator itr = gpu_images.find(id);

    if (itr == gpu_images.end())
        return nullptr;

    return &itr->second;
}
