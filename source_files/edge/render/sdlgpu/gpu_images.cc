#include "gpu_images.h"

#include <unordered_map>

static std::unordered_map<GLuint, GpuImage> gpu_images;

void RegisterGpuImage(GLuint id, SDL_GPUTexture *texture, SDL_GPUSampler *sampler)
{
    GpuImage image;

    image.texture = texture;
    image.sampler = sampler;

    gpu_images[id] = image;
}

void DeleteGpuImage(SDL_GPUDevice *device, GLuint id)
{
    auto itr = gpu_images.find(id);

    if (itr == gpu_images.end())
        return;

    if (itr->second.texture)
        SDL_ReleaseGPUTexture(device, itr->second.texture);

    gpu_images.erase(itr);
}

void ShutdownGpuImages(SDL_GPUDevice *device)
{
    for (auto itr = gpu_images.begin(); itr != gpu_images.end(); itr++)
    {
        if (itr->second.texture)
            SDL_ReleaseGPUTexture(device, itr->second.texture);
    }

    gpu_images.clear();
}

const GpuImage *GetGpuImage(GLuint id)
{
    auto itr = gpu_images.find(id);

    if (itr == gpu_images.end())
        return nullptr;

    return &itr->second;
}
