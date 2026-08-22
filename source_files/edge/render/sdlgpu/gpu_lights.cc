#include "gpu_lights.h"

#include <string.h>

#include <vector>

#include "epi.h"
#include "gpu_device.h"
#include "gpu_pipeline.h"
#include "gpu_shaders.h"
#include "i_system.h"
#include "con_var.h"
#include "r_lightgrid.h"
#include "r_misc.h"
#include "r_backend.h"
#include "r_state.h"

struct GpuLightRecord
{
    float position_radius[4];
    float color_additive[4];
};

struct GpuLightCull
{
    float    frustum[4];
    float    viewport[4];
    uint32_t grid[4];
    uint32_t range[4];

    int cluster_total;
};

static std::vector<GpuLightRecord> frame_lights;
static std::vector<GpuLightCull>   frame_culls;

static int frame_cluster_total = 0;

static std::vector<GpuLightViewParameters> frame_views;

static int current_light_view = -1;

static SDL_GPUBuffer *light_buffer  = nullptr;
static SDL_GPUBuffer *cluster_buffer = nullptr;
static SDL_GPUBuffer *index_buffer  = nullptr;

static SDL_GPUBuffer *counter_buffer = nullptr;

static SDL_GPUTransferBuffer *light_transfer   = nullptr;
static SDL_GPUTransferBuffer *counter_transfer = nullptr;

static size_t light_capacity   = 0;
static size_t cluster_capacity = 0;
static size_t index_capacity   = 0;
static size_t counter_capacity = 0;

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

static bool EnsureDeviceBuffer(SDL_GPUBuffer **buffer, size_t *capacity, size_t bytes, SDL_GPUBufferUsageFlags usage,
                               const char *what)
{
    if (bytes <= *capacity && *buffer)
        return true;

    SDL_GPUDevice *device = gpu_device.Handle();

    if (!device)
        return false;

    size_t wanted = 65536;

    while (wanted < bytes)
        wanted *= 2;

    if (*buffer)
        SDL_ReleaseGPUBuffer(device, *buffer);

    SDL_GPUBufferCreateInfo buffer_info;
    EPI_CLEAR_MEMORY(&buffer_info, SDL_GPUBufferCreateInfo, 1);

    buffer_info.usage = usage;
    buffer_info.size  = (uint32_t)wanted;

    *buffer = SDL_CreateGPUBuffer(device, &buffer_info);

    if (!*buffer)
    {
        LogPrint("GpuLights: SDL_CreateGPUBuffer (%s) failed: %s\n", what, SDL_GetError());
        *capacity = 0;
        return false;
    }

    *capacity = wanted;

    return true;
}

void GpuCreateLightBuffers(void)
{
    GpuResetLightFrame();

    EnsureBuffer(&light_buffer, &light_transfer, &light_capacity, sizeof(GpuLightRecord), "lights");

    SDL_GPUBufferUsageFlags storage_usage =
        SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;

    EnsureDeviceBuffer(&cluster_buffer, &cluster_capacity, sizeof(uint32_t), storage_usage, "clusters");
    EnsureDeviceBuffer(&index_buffer, &index_capacity, sizeof(uint32_t), storage_usage, "light indices");
    EnsureDeviceBuffer(&counter_buffer, &counter_capacity, sizeof(uint32_t),
                       storage_usage | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, "light counter");

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

        if (counter_buffer)
            SDL_ReleaseGPUBuffer(device, counter_buffer);

        if (counter_transfer)
            SDL_ReleaseGPUTransferBuffer(device, counter_transfer);

        if (light_transfer)
            SDL_ReleaseGPUTransferBuffer(device, light_transfer);

    }

    light_buffer   = nullptr;
    cluster_buffer = nullptr;
    index_buffer   = nullptr;

    light_transfer   = nullptr;

    counter_buffer   = nullptr;
    counter_transfer = nullptr;

    light_capacity   = 0;
    cluster_capacity = 0;
    index_capacity   = 0;
    counter_capacity = 0;

    GpuResetLightFrame();
}

void GpuResetLightFrame(void)
{
    frame_lights.clear();
    frame_culls.clear();
    frame_views.clear();

    frame_cluster_total = 0;

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
    int cluster_base = frame_cluster_total;

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

    GpuLightCull cull;

    cull.frustum[0] = view_x_slope;
    cull.frustum[1] = view_y_slope;
    cull.frustum[2] = grid->cluster_near;
    cull.frustum[3] = grid->cluster_far;

    cull.viewport[0] = (float)grid->view_width;
    cull.viewport[1] = (float)grid->view_height;
    cull.viewport[2] = (float)kLightGridTileSize;
    cull.viewport[3] = fliplevels.d_ ? -1.0f : 1.0f;

    cull.grid[0] = (uint32_t)grid->tiles_x;
    cull.grid[1] = (uint32_t)grid->tiles_y;
    cull.grid[2] = (uint32_t)kLightGridDepthSlices;
    cull.grid[3] = 0;

    cull.range[0] = (uint32_t)light_base;
    cull.range[1] = (uint32_t)grid->lights.size();
    cull.range[2] = (uint32_t)cluster_base;
    cull.range[3] = 0;

    cull.cluster_total = cluster_total;

    frame_culls.push_back(cull);

    frame_cluster_total += cluster_total;

    GpuLightViewParameters view;

    view.light_view[0] = (float)grid->view_x;
    view.light_view[1] = (float)render_backend->RenderTargetHeight() - (float)grid->view_y;
    view.light_view[2] = (float)grid->tiles_x;
    view.light_view[3] = (float)grid->tiles_y;

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

static bool EnsureTransfer(SDL_GPUTransferBuffer **transfer, size_t bytes, const char *what)
{
    if (*transfer)
        return true;

    SDL_GPUDevice *device = gpu_device.Handle();

    if (!device)
        return false;

    SDL_GPUTransferBufferCreateInfo transfer_info;
    EPI_CLEAR_MEMORY(&transfer_info, SDL_GPUTransferBufferCreateInfo, 1);

    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size  = (uint32_t)bytes;

    *transfer = SDL_CreateGPUTransferBuffer(device, &transfer_info);

    if (!*transfer)
    {
        LogPrint("GpuLights: SDL_CreateGPUTransferBuffer (%s) failed: %s\n", what, SDL_GetError());
        return false;
    }

    return true;
}

void GpuFlushLightBuffers(void)
{
    if (frame_lights.empty() || frame_culls.empty())
        return;

    size_t light_bytes = frame_lights.size() * sizeof(GpuLightRecord);

    if (!EnsureBuffer(&light_buffer, &light_transfer, &light_capacity, light_bytes, "lights"))
        return;

    UploadOne(light_buffer, light_transfer, frame_lights.data(), light_bytes);

    SDL_GPUBufferUsageFlags storage_usage =
        SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;

    size_t cluster_bytes = (size_t)frame_cluster_total * sizeof(uint32_t);

    size_t index_bytes = (size_t)frame_cluster_total * (size_t)kLightGridMaximumPerTile * sizeof(uint32_t);

    if (!EnsureDeviceBuffer(&cluster_buffer, &cluster_capacity, cluster_bytes, storage_usage, "clusters"))
        return;

    if (!EnsureDeviceBuffer(&index_buffer, &index_capacity, index_bytes, storage_usage, "light indices"))
        return;

    if (!EnsureDeviceBuffer(&counter_buffer, &counter_capacity, sizeof(uint32_t),
                            storage_usage | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, "light counter"))
        return;

    if (!EnsureTransfer(&counter_transfer, sizeof(uint32_t), "light counter reset"))
        return;

    uint32_t zero = 0;

    UploadOne(counter_buffer, counter_transfer, &zero, sizeof(zero));

    SDL_GPUComputePipeline *pipeline = GetLightCullPipeline();

    if (!pipeline)
        return;

    SDL_GPUStorageBufferReadWriteBinding writes[3];
    EPI_CLEAR_MEMORY(writes, SDL_GPUStorageBufferReadWriteBinding, 3);

    writes[0].buffer = cluster_buffer;
    writes[0].cycle  = true;

    writes[1].buffer = index_buffer;
    writes[1].cycle  = true;

    writes[2].buffer = counter_buffer;
    writes[2].cycle  = false;

    SDL_GPUComputePass *compute_pass = SDL_BeginGPUComputePass(gpu_device.CommandBuffer(), nullptr, 0, writes, 3);

    if (!compute_pass)
    {
        LogPrint("GpuLights: SDL_BeginGPUComputePass failed: %s\n", SDL_GetError());
        return;
    }

    SDL_BindGPUComputePipeline(compute_pass, pipeline);

    SDL_BindGPUComputeStorageBuffers(compute_pass, 0, &light_buffer, 1);

    uint32_t index_limit = (uint32_t)(index_capacity / sizeof(uint32_t));

    for (size_t i = 0; i < frame_culls.size(); i++)
    {
        GpuLightCull cull = frame_culls[i];

        cull.range[3] = index_limit;

        SDL_PushGPUComputeUniformData(gpu_device.CommandBuffer(), 0, &cull, (uint32_t)(sizeof(float) * 8 + sizeof(uint32_t) * 8));

        uint32_t groups = (uint32_t)((cull.cluster_total + 63) / 64);

        SDL_DispatchGPUCompute(compute_pass, groups, 1, 1);
    }

    SDL_EndGPUComputePass(compute_pass);
}

void GpuBindLightBuffers(SDL_GPURenderPass *pass)
{
    if (!pass || !light_buffer || !cluster_buffer || !index_buffer)
        return;

    SDL_GPUBuffer *buffers[3] = {light_buffer, cluster_buffer, index_buffer};

    SDL_BindGPUFragmentStorageBuffers(pass, kGpuStorageSlotLights, buffers, 3);
}
