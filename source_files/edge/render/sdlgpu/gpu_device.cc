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

    if (depth_texture_)
    {
        SDL_ReleaseGPUTexture(device_, depth_texture_);
        depth_texture_ = nullptr;
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

    if (mode == 0)
    {
        if (SDL_WindowSupportsGPUPresentMode(device_, window_, SDL_GPU_PRESENTMODE_IMMEDIATE))
            present = SDL_GPU_PRESENTMODE_IMMEDIATE;
    }
    else if (mode == 2)
    {
        if (SDL_WindowSupportsGPUPresentMode(device_, window_, SDL_GPU_PRESENTMODE_MAILBOX))
            present = SDL_GPU_PRESENTMODE_MAILBOX;
    }

    if (!SDL_SetGPUSwapchainParameters(device_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, present))
        LogPrint("GpuDevice: SDL_SetGPUSwapchainParameters failed: %s\n", SDL_GetError());
}

bool GpuDevice::CreateDepthTexture(int32_t width, int32_t height)
{
    if (depth_texture_ && depth_width_ == width && depth_height_ == height)
        return true;

    if (depth_texture_)
    {
        SDL_ReleaseGPUTexture(device_, depth_texture_);
        depth_texture_ = nullptr;
    }

    SDL_GPUTextureCreateInfo info;
    EPI_CLEAR_MEMORY(&info, SDL_GPUTextureCreateInfo, 1);

    info.type                 = SDL_GPU_TEXTURETYPE_2D;
    info.format               = depth_format_;
    info.usage                = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    info.width                = (uint32_t)width;
    info.height               = (uint32_t)height;
    info.layer_count_or_depth = 1;
    info.num_levels           = 1;
    info.sample_count         = SDL_GPU_SAMPLECOUNT_1;

    depth_texture_ = SDL_CreateGPUTexture(device_, &info);

    if (!depth_texture_)
    {
        LogPrint("GpuDevice: SDL_CreateGPUTexture (depth) failed: %s\n", SDL_GetError());
        return false;
    }

    depth_width_  = width;
    depth_height_ = height;

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

    if (!CreateDepthTexture((int32_t)swapchain_width, (int32_t)swapchain_height))
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

    color_target.texture = swapchain_texture_;

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

void GpuDevice::SubmitFrame()
{
    EndPass();

    if (!command_buffer_)
        return;

    SDL_SubmitGPUCommandBuffer(command_buffer_);

    command_buffer_    = nullptr;
    swapchain_texture_ = nullptr;
}
