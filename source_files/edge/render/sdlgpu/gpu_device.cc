#include "gpu_device.h"

#include "epi.h"
#include "i_system.h"

GpuDevice gpu_device;

bool GpuDevice::Init(SDL_Window *window)
{
    window_ = window;

    SDL_PropertiesID props = SDL_CreateProperties();

    SDL_SetStringProperty(props, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, "vulkan");
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);

    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_CLIP_DISTANCE_BOOLEAN, false);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_DEPTH_CLAMPING_BOOLEAN, false);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_INDIRECT_DRAW_FIRST_INSTANCE_BOOLEAN, false);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_ANISOTROPY_BOOLEAN, false);

    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_VULKAN_REQUIRE_HARDWARE_ACCELERATION_BOOLEAN, false);

#ifdef EDGE_EXTRA_CHECKS
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, true);
#endif

    device_ = SDL_CreateGPUDeviceWithProperties(props);

    SDL_DestroyProperties(props);

    if (!device_)
    {
        LogPrint("GpuDevice: SDL_CreateGPUDeviceWithProperties failed: %s\n", SDL_GetError());
        return false;
    }

    if (!SDL_ClaimWindowForGPUDevice(device_, window_))
    {
        LogPrint("GpuDevice: SDL_ClaimWindowForGPUDevice failed: %s\n", SDL_GetError());
        SDL_DestroyGPUDevice(device_);
        device_ = nullptr;
        return false;
    }

    swapchain_format_ = SDL_GetGPUSwapchainTextureFormat(device_, window_);

    if (SDL_GPUTextureSupportsFormat(device_, SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT, SDL_GPU_TEXTURETYPE_2D,
                                     SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET))
        depth_format_ = SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
    else if (SDL_GPUTextureSupportsFormat(device_, SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT, SDL_GPU_TEXTURETYPE_2D,
                                          SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET))
        depth_format_ = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
    else
        depth_format_ = SDL_GPU_TEXTUREFORMAT_D16_UNORM;

    SDL_PropertiesID device_props = SDL_GetGPUDeviceProperties(device_);

    LogPrint("SDL_GPU: driver '%s'\n", SDL_GetGPUDeviceDriver(device_));
    LogPrint("SDL_GPU: device '%s'\n", SDL_GetStringProperty(device_props, SDL_PROP_GPU_DEVICE_NAME_STRING, "unknown"));
    LogPrint("SDL_GPU: driver name '%s' version '%s'\n",
             SDL_GetStringProperty(device_props, SDL_PROP_GPU_DEVICE_DRIVER_NAME_STRING, "unknown"),
             SDL_GetStringProperty(device_props, SDL_PROP_GPU_DEVICE_DRIVER_VERSION_STRING, "unknown"));

    return true;
}

void GpuDevice::Shutdown()
{
    if (!device_)
        return;

    if (color_texture_)
    {
        SDL_ReleaseGPUTexture(device_, color_texture_);
        color_texture_ = nullptr;
    }

    if (depth_texture_)
    {
        SDL_ReleaseGPUTexture(device_, depth_texture_);
        depth_texture_ = nullptr;
    }

    if (download_buffer_)
    {
        SDL_ReleaseGPUTransferBuffer(device_, download_buffer_);
        download_buffer_          = nullptr;
        download_buffer_capacity_ = 0;
    }

    SDL_ReleaseWindowFromGPUDevice(device_, window_);
    SDL_DestroyGPUDevice(device_);

    device_ = nullptr;
    window_ = nullptr;
}

void GpuDevice::SetVerticalSync(int mode)
{
    if (!device_ || !window_)
        return;

    SDL_GPUPresentMode present = SDL_GPU_PRESENTMODE_VSYNC;
    const char        *present_name = "vsync";

    if (mode == 0)
    {
        if (SDL_WindowSupportsGPUPresentMode(device_, window_, SDL_GPU_PRESENTMODE_IMMEDIATE))
        {
            present      = SDL_GPU_PRESENTMODE_IMMEDIATE;
            present_name = "immediate";
        }
        else if (SDL_WindowSupportsGPUPresentMode(device_, window_, SDL_GPU_PRESENTMODE_MAILBOX))
        {
            present      = SDL_GPU_PRESENTMODE_MAILBOX;
            present_name = "mailbox";
        }
    }
    else if (mode == 2)
    {
        if (SDL_WindowSupportsGPUPresentMode(device_, window_, SDL_GPU_PRESENTMODE_MAILBOX))
        {
            present      = SDL_GPU_PRESENTMODE_MAILBOX;
            present_name = "mailbox";
        }
    }

    if (!SDL_SetGPUSwapchainParameters(device_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, present))
    {
        LogPrint("GpuDevice: SDL_SetGPUSwapchainParameters failed: %s\n", SDL_GetError());
        return;
    }

    LogPrint("SDL_GPU: present mode '%s' (vsync %d)\n", present_name, mode);
}

bool GpuDevice::CreateFrameTextures(int32_t width, int32_t height)
{
    if (color_texture_ && depth_texture_ && target_width_ == width && target_height_ == height)
        return true;

    if (color_texture_)
    {
        SDL_ReleaseGPUTexture(device_, color_texture_);
        color_texture_ = nullptr;
    }

    if (depth_texture_)
    {
        SDL_ReleaseGPUTexture(device_, depth_texture_);
        depth_texture_ = nullptr;
    }

    SDL_GPUTextureCreateInfo info;
    EPI_CLEAR_MEMORY(&info, SDL_GPUTextureCreateInfo, 1);

    info.type                 = SDL_GPU_TEXTURETYPE_2D;
    info.format               = swapchain_format_;
    info.usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width                = (uint32_t)width;
    info.height               = (uint32_t)height;
    info.layer_count_or_depth = 1;
    info.num_levels           = 1;
    info.sample_count         = SDL_GPU_SAMPLECOUNT_1;

    color_texture_ = SDL_CreateGPUTexture(device_, &info);

    if (!color_texture_)
    {
        LogPrint("GpuDevice: SDL_CreateGPUTexture (color) failed: %s\n", SDL_GetError());
        return false;
    }

    info.format = depth_format_;
    info.usage  = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;

    depth_texture_ = SDL_CreateGPUTexture(device_, &info);

    if (!depth_texture_)
    {
        LogPrint("GpuDevice: SDL_CreateGPUTexture (depth) failed: %s\n", SDL_GetError());
        SDL_ReleaseGPUTexture(device_, color_texture_);
        color_texture_ = nullptr;
        return false;
    }

    target_width_  = width;
    target_height_ = height;

    return true;
}

bool GpuDevice::AcquireFrame(int32_t width, int32_t height)
{
    EPI_UNUSED(width);
    EPI_UNUSED(height);

    if (!device_)
        return false;

    command_buffer_ = SDL_AcquireGPUCommandBuffer(device_);

    if (!command_buffer_)
    {
        LogPrint("GpuDevice: SDL_AcquireGPUCommandBuffer failed: %s\n", SDL_GetError());
        return false;
    }

    uint32_t swapchain_width  = 0;
    uint32_t swapchain_height = 0;

    if (!SDL_WaitAndAcquireGPUSwapchainTexture(command_buffer_, window_, &swapchain_texture_, &swapchain_width,
                                               &swapchain_height))
    {
        LogPrint("GpuDevice: SDL_WaitAndAcquireGPUSwapchainTexture failed: %s\n", SDL_GetError());
        SDL_CancelGPUCommandBuffer(command_buffer_);
        command_buffer_ = nullptr;
        return false;
    }

    if (!swapchain_texture_)
    {
        SDL_SubmitGPUCommandBuffer(command_buffer_);
        command_buffer_ = nullptr;
        return false;
    }

    if (!CreateFrameTextures((int32_t)swapchain_width, (int32_t)swapchain_height))
    {
        SDL_SubmitGPUCommandBuffer(command_buffer_);
        command_buffer_    = nullptr;
        swapchain_texture_ = nullptr;
        return false;
    }

    color_written_ = false;

    return true;
}

void GpuDevice::BeginPass(GpuLoadOperation color_load, GpuLoadOperation depth_load)
{
    if (!FrameAcquired())
        return;

    EndPass();

    SDL_GPUColorTargetInfo color_target;
    EPI_CLEAR_MEMORY(&color_target, SDL_GPUColorTargetInfo, 1);

    color_target.texture = color_texture_;

    if (color_load == kGpuLoadOperationLoad && !color_written_)
        color_load = kGpuLoadOperationClear;

    switch (color_load)
    {
    case kGpuLoadOperationClear:
        color_target.load_op = SDL_GPU_LOADOP_CLEAR;
        break;
    case kGpuLoadOperationDontCare:
        color_target.load_op = SDL_GPU_LOADOP_DONT_CARE;
        break;
    default:
        color_target.load_op = SDL_GPU_LOADOP_LOAD;
        break;
    }

    color_target.store_op         = SDL_GPU_STOREOP_STORE;
    color_target.clear_color.r    = epi::GetRGBARed(clear_color_) / 255.0f;
    color_target.clear_color.g    = epi::GetRGBAGreen(clear_color_) / 255.0f;
    color_target.clear_color.b    = epi::GetRGBABlue(clear_color_) / 255.0f;
    color_target.clear_color.a    = 1.0f;

    SDL_GPUDepthStencilTargetInfo depth_target;
    EPI_CLEAR_MEMORY(&depth_target, SDL_GPUDepthStencilTargetInfo, 1);

    depth_target.texture = depth_texture_;

    switch (depth_load)
    {
    case kGpuLoadOperationClear:
        depth_target.load_op = SDL_GPU_LOADOP_CLEAR;
        break;
    case kGpuLoadOperationDontCare:
        depth_target.load_op = SDL_GPU_LOADOP_DONT_CARE;
        break;
    default:
        depth_target.load_op = SDL_GPU_LOADOP_LOAD;
        break;
    }

    depth_target.store_op         = SDL_GPU_STOREOP_STORE;
    depth_target.stencil_load_op  = SDL_GPU_LOADOP_DONT_CARE;
    depth_target.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth_target.clear_depth      = 1.0f;
    depth_target.cycle            = false;

    render_pass_ = SDL_BeginGPURenderPass(command_buffer_, &color_target, 1, &depth_target);

    if (!render_pass_)
        LogPrint("GpuDevice: SDL_BeginGPURenderPass failed: %s\n", SDL_GetError());

    color_written_ = true;
}

void GpuDevice::EndPass()
{
    if (!render_pass_)
        return;

    SDL_EndGPURenderPass(render_pass_);
    render_pass_ = nullptr;
}

SDL_GPUCommandBuffer *GpuDevice::BeginUpload()
{
    if (!device_)
        return nullptr;

    if (command_buffer_ && !render_pass_)
        return command_buffer_;

    SDL_GPUCommandBuffer *upload_buffer = SDL_AcquireGPUCommandBuffer(device_);

    if (!upload_buffer)
        LogPrint("GpuDevice: SDL_AcquireGPUCommandBuffer (upload) failed: %s\n", SDL_GetError());

    return upload_buffer;
}

void GpuDevice::EndUpload(SDL_GPUCommandBuffer *upload_buffer)
{
    if (!upload_buffer || upload_buffer == command_buffer_)
        return;

    SDL_SubmitGPUCommandBuffer(upload_buffer);
}

void GpuDevice::SubmitFrame()
{
    EndPass();

    if (!command_buffer_)
        return;

    if (swapchain_texture_ && color_texture_)
    {
        SDL_GPUBlitInfo blit;
        EPI_CLEAR_MEMORY(&blit, SDL_GPUBlitInfo, 1);

        blit.source.texture = color_texture_;
        blit.source.w       = (uint32_t)target_width_;
        blit.source.h       = (uint32_t)target_height_;

        blit.destination.texture = swapchain_texture_;
        blit.destination.w       = (uint32_t)target_width_;
        blit.destination.h       = (uint32_t)target_height_;

        blit.load_op   = SDL_GPU_LOADOP_DONT_CARE;
        blit.flip_mode = SDL_FLIP_NONE;
        blit.filter    = SDL_GPU_FILTER_NEAREST;

        SDL_BlitGPUTexture(command_buffer_, &blit);
    }

    SDL_SubmitGPUCommandBuffer(command_buffer_);

    command_buffer_    = nullptr;
    swapchain_texture_ = nullptr;
}

bool GpuDevice::ReadColorTarget(int32_t width, int32_t height, int32_t stride, uint8_t *dest)
{
    return ReadColorRegion(0, 0, width, height, stride, dest);
}

bool GpuDevice::ReadColorRegion(int32_t x, int32_t y, int32_t width, int32_t height, int32_t stride, uint8_t *dest)
{
    if (!device_ || !color_texture_ || !dest)
        return false;

    if (x < 0 || y < 0)
        return false;

    if (x + width > target_width_)
        width = target_width_ - x;

    if (y + height > target_height_)
        height = target_height_ - y;

    if (width <= 0 || height <= 0)
        return false;

    uint32_t bytes = (uint32_t)width * (uint32_t)height * 4;

    if (download_buffer_ && download_buffer_capacity_ < bytes)
    {
        SDL_ReleaseGPUTransferBuffer(device_, download_buffer_);
        download_buffer_          = nullptr;
        download_buffer_capacity_ = 0;
    }

    if (!download_buffer_)
    {
        SDL_GPUTransferBufferCreateInfo transfer_info;
        EPI_CLEAR_MEMORY(&transfer_info, SDL_GPUTransferBufferCreateInfo, 1);

        transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        transfer_info.size  = bytes;

        download_buffer_ = SDL_CreateGPUTransferBuffer(device_, &transfer_info);

        if (!download_buffer_)
        {
            LogPrint("GpuDevice: SDL_CreateGPUTransferBuffer (download) failed: %s\n", SDL_GetError());
            return false;
        }

        download_buffer_capacity_ = bytes;
    }

    SDL_GPUCommandBuffer *command_buffer = SDL_AcquireGPUCommandBuffer(device_);

    if (!command_buffer)
    {
        LogPrint("GpuDevice: SDL_AcquireGPUCommandBuffer (download) failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(command_buffer);

    SDL_GPUTextureRegion source;
    EPI_CLEAR_MEMORY(&source, SDL_GPUTextureRegion, 1);

    source.texture = color_texture_;
    source.x       = (uint32_t)x;
    source.y       = (uint32_t)(target_height_ - y - height);
    source.w       = (uint32_t)width;
    source.h       = (uint32_t)height;
    source.d       = 1;

    SDL_GPUTextureTransferInfo destination;
    EPI_CLEAR_MEMORY(&destination, SDL_GPUTextureTransferInfo, 1);

    destination.transfer_buffer = download_buffer_;
    destination.pixels_per_row  = (uint32_t)width;
    destination.rows_per_layer  = (uint32_t)height;

    SDL_DownloadFromGPUTexture(copy_pass, &source, &destination);

    SDL_EndGPUCopyPass(copy_pass);

    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command_buffer);

    if (!fence)
    {
        LogPrint("GpuDevice: SDL_SubmitGPUCommandBufferAndAcquireFence failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_WaitForGPUFences(device_, true, &fence, 1);
    SDL_ReleaseGPUFence(device_, fence);

    const uint8_t *mapped = (const uint8_t *)SDL_MapGPUTransferBuffer(device_, download_buffer_, false);

    if (!mapped)
    {
        LogPrint("GpuDevice: SDL_MapGPUTransferBuffer (download) failed: %s\n", SDL_GetError());
        return false;
    }

    bool swizzle = (swapchain_format_ == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
                    swapchain_format_ == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB);

    for (int32_t row = 0; row < height; row++)
    {
        const uint8_t *source_row = mapped + (size_t)(height - 1 - row) * (size_t)width * 4;
        uint8_t       *dest_row   = dest + (size_t)row * (size_t)stride;

        if (swizzle)
        {
            for (int32_t column = 0; column < width; column++)
            {
                dest_row[column * 4 + 0] = source_row[column * 4 + 2];
                dest_row[column * 4 + 1] = source_row[column * 4 + 1];
                dest_row[column * 4 + 2] = source_row[column * 4 + 0];
                dest_row[column * 4 + 3] = source_row[column * 4 + 3];
            }
        }
        else
        {
            memcpy(dest_row, source_row, (size_t)width * 4);
        }
    }

    SDL_UnmapGPUTransferBuffer(device_, download_buffer_);

    return true;
}
