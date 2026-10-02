#include "gpu_lights.h"

#include <string.h>

#include <vector>

#include "epi.h"
#include "gpu_device.h"
#include "gpu_shaders.h"
#include "i_system.h"
#include "r_lightgrid.h"
#include "r_backend.h"

struct GpuLightRecord
{
    float position_radius[4];
    float color_additive[4];
};

static std::vector<GpuLightRecord> frame_lights;
static std::vector<uint32_t>       frame_clusters;
static std::vector<uint32_t>       frame_indices;

static std::vector<GpuLightViewParameters> frame_views;

static int current_light_view = -1;

static SDL_GPUBuffer *light_buffer  = nullptr;
static SDL_GPUBuffer *cluster_buffer = nullptr;
static SDL_GPUBuffer *index_buffer  = nullptr;

static SDL_GPUTransferBuffer *light_transfer   = nullptr;
static SDL_GPUTransferBuffer *cluster_transfer = nullptr;
static SDL_GPUTransferBuffer *index_transfer   = nullptr;

static size_t light_capacity   = 0;
static size_t cluster_capacity = 0;
static size_t index_capacity   = 0;

static bool EnsureBuffer(SDL_GPUBuffer **buffer, SDL_GPUTransferBuffer **transfer, size_t *capacity, size_t bytes,
                         const char *what)
{
    if (bytes <= *capacity && *buffer && *transfer)
        return true;

    SDL_GPUDevice *device = gpu_device.Handle();

    if (!device)
        return false;

    size_t wanted = 4096;

    while (wanted < bytes)
        wanted *= 2;

    if (*buffer)
    {
        SDL_ReleaseGPUBuffer(device, *buffer);
        *buffer = nullptr;
    }

    if (*transfer)
    {
        SDL_ReleaseGPUTransferBuffer(device, *transfer);
        *transfer = nullptr;
    }

    SDL_GPUBufferCreateInfo buffer_info;
    EPI_CLEAR_MEMORY(&buffer_info, SDL_GPUBufferCreateInfo, 1);

    buffer_info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    buffer_info.size  = (uint32_t)wanted;

    *buffer = SDL_CreateGPUBuffer(device, &buffer_info);

    if (!*buffer)
    {
        LogPrint("GpuLights: SDL_CreateGPUBuffer (%s) failed: %s\n", what, SDL_GetError());
        return false;
    }

    SDL_GPUTransferBufferCreateInfo transfer_info;
    EPI_CLEAR_MEMORY(&transfer_info, SDL_GPUTransferBufferCreateInfo, 1);

    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size  = (uint32_t)wanted;

    *transfer = SDL_CreateGPUTransferBuffer(device, &transfer_info);

    if (!*transfer)
    {
        LogPrint("GpuLights: SDL_CreateGPUTransferBuffer (%s) failed: %s\n", what, SDL_GetError());
        SDL_ReleaseGPUBuffer(device, *buffer);
        *buffer = nullptr;
        return false;
    }

    *capacity = wanted;

    return true;
}

void GpuCreateLightBuffers(void)
{
    GpuResetLightFrame();

    EnsureBuffer(&light_buffer, &light_transfer, &light_capacity, sizeof(GpuLightRecord), "lights");
    EnsureBuffer(&cluster_buffer, &cluster_transfer, &cluster_capacity, sizeof(uint32_t), "clusters");
    EnsureBuffer(&index_buffer, &index_transfer, &index_capacity, sizeof(uint32_t), "light indices");
}

void GpuDestroyLightBuffers(void)
{
    SDL_GPUDevice *device = gpu_device.Handle();

    if (device)
    {
        if (light_buffer)
            SDL_ReleaseGPUBuffer(device, light_buffer);

        if (cluster_buffer)
            SDL_ReleaseGPUBuffer(device, cluster_buffer);

        if (index_buffer)
            SDL_ReleaseGPUBuffer(device, index_buffer);

        if (light_transfer)
            SDL_ReleaseGPUTransferBuffer(device, light_transfer);

        if (cluster_transfer)
            SDL_ReleaseGPUTransferBuffer(device, cluster_transfer);

        if (index_transfer)
            SDL_ReleaseGPUTransferBuffer(device, index_transfer);
    }

    light_buffer   = nullptr;
    cluster_buffer = nullptr;
    index_buffer   = nullptr;

    light_transfer   = nullptr;
    cluster_transfer = nullptr;
    index_transfer   = nullptr;

    light_capacity   = 0;
    cluster_capacity = 0;
    index_capacity   = 0;

    GpuResetLightFrame();
}

void GpuResetLightFrame(void)
{
    frame_lights.clear();
    frame_clusters.clear();
    frame_indices.clear();
    frame_views.clear();

    current_light_view = -1;
}

int GpuCurrentLightView(void)
{
    return current_light_view;
}

const GpuLightViewParameters *GpuLightView(int index)
{
    if (index < 0 || index >= (int)frame_views.size())
        return nullptr;

    return &frame_views[(size_t)index];
}

void GpuUploadLightGrid(const LightGrid *grid)
{
    current_light_view = -1;

    if (!grid || grid->Empty())
        return;

    int light_base   = (int)frame_lights.size();
    int cluster_base = (int)frame_clusters.size();

    for (size_t i = 0; i < grid->lights.size(); i++)
    {
        const LightGridLight &light = grid->lights[i];

        GpuLightRecord record;

        record.position_radius[0] = light.eye_position.X;
        record.position_radius[1] = light.eye_position.Y;
        record.position_radius[2] = light.eye_position.Z;
        record.position_radius[3] = light.radius;

        record.color_additive[0] = light.color.X / 255.0f;
        record.color_additive[1] = light.color.Y / 255.0f;
        record.color_additive[2] = light.color.Z / 255.0f;
        record.color_additive[3] = light.additive;

        frame_lights.push_back(record);
    }

    int cluster_total = grid->ClusterTotal();

    for (int cluster = 0; cluster < cluster_total; cluster++)
    {
        uint32_t count  = grid->cluster_counts[(size_t)cluster];
        uint32_t offset = (uint32_t)frame_indices.size();

        for (uint32_t k = 0; k < count; k++)
            frame_indices.push_back((uint32_t)light_base +
                                    grid->cluster_list[(size_t)grid->cluster_offsets[(size_t)cluster] + k]);

        frame_clusters.push_back(count ? ((offset << 8) | count) : 0u);
    }

    GpuLightViewParameters view;

    view.light_view[0] = (float)grid->view_x;
    view.light_view[1] = (float)render_backend->RenderTargetHeight() - (float)grid->view_y;
    view.light_view[2] = (float)grid->clusters_x;
    view.light_view[3] = (float)grid->clusters_y;

    view.light_range[0] = grid->cluster_near;
    view.light_range[1] = grid->cluster_far;
    view.light_range[2] = 1.0f;
    view.light_range[3] = (float)cluster_base;

    current_light_view = (int)frame_views.size();

    frame_views.push_back(view);
}

static void UploadOne(SDL_GPUBuffer *buffer, SDL_GPUTransferBuffer *transfer, const void *data, size_t bytes)
{
    if (!buffer || !transfer || bytes == 0)
        return;

    SDL_GPUDevice *device = gpu_device.Handle();

    void *mapped = SDL_MapGPUTransferBuffer(device, transfer, true);

    if (!mapped)
    {
        LogPrint("GpuLights: SDL_MapGPUTransferBuffer failed: %s\n", SDL_GetError());
        return;
    }

    memcpy(mapped, data, bytes);

    SDL_UnmapGPUTransferBuffer(device, transfer);

    SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(gpu_device.CommandBuffer());

    SDL_GPUTransferBufferLocation source;
    source.transfer_buffer = transfer;
    source.offset          = 0;

    SDL_GPUBufferRegion destination;
    destination.buffer = buffer;
    destination.offset = 0;
    destination.size   = (uint32_t)bytes;

    SDL_UploadToGPUBuffer(copy_pass, &source, &destination, true);

    SDL_EndGPUCopyPass(copy_pass);
}

void GpuFlushLightBuffers(void)
{
    if (frame_lights.empty() || frame_clusters.empty())
        return;

    if (frame_indices.empty())
        frame_indices.push_back(0);

    size_t light_bytes   = frame_lights.size() * sizeof(GpuLightRecord);
    size_t cluster_bytes = frame_clusters.size() * sizeof(uint32_t);
    size_t index_bytes   = frame_indices.size() * sizeof(uint32_t);

    if (!EnsureBuffer(&light_buffer, &light_transfer, &light_capacity, light_bytes, "lights") ||
        !EnsureBuffer(&cluster_buffer, &cluster_transfer, &cluster_capacity, cluster_bytes, "clusters") ||
        !EnsureBuffer(&index_buffer, &index_transfer, &index_capacity, index_bytes, "light indices"))
        return;

    UploadOne(light_buffer, light_transfer, frame_lights.data(), light_bytes);
    UploadOne(cluster_buffer, cluster_transfer, frame_clusters.data(), cluster_bytes);
    UploadOne(index_buffer, index_transfer, frame_indices.data(), index_bytes);
}

void GpuBindLightBuffers(SDL_GPURenderPass *pass)
{
    if (!pass || !light_buffer || !cluster_buffer || !index_buffer)
        return;

    SDL_GPUBuffer *buffers[3] = {light_buffer, cluster_buffer, index_buffer};

    SDL_BindGPUFragmentStorageBuffers(pass, kGpuStorageSlotLights, buffers, 3);
}
