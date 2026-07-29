#pragma once

#include <SDL3/SDL.h>

#include "epi_color.h"

enum GpuLoadOperation
{
    kGpuLoadOperationLoad = 0,
    kGpuLoadOperationClear,
    kGpuLoadOperationDontCare
};

class GpuDevice
{
  public:
    bool Init(SDL_Window *window);

    void Shutdown();

    bool AcquireFrame(int32_t width, int32_t height);

    void SubmitFrame();

    void BeginPass(GpuLoadOperation color_load, GpuLoadOperation depth_load);

    void EndPass();

    void SetClearColor(RGBAColor color)
    {
        clear_color_ = color;
    }

    void SetVerticalSync(int mode);

    SDL_GPUDevice *Handle() const
    {
        return device_;
    }

    SDL_GPUCommandBuffer *CommandBuffer() const
    {
        return command_buffer_;
    }

    SDL_GPURenderPass *RenderPass() const
    {
        return render_pass_;
    }

    SDL_GPUTexture *SwapchainTexture() const
    {
        return swapchain_texture_;
    }

    SDL_GPUTextureFormat SwapchainFormat() const
    {
        return swapchain_format_;
    }

    SDL_GPUTextureFormat DepthFormat() const
    {
        return depth_format_;
    }

    bool FrameAcquired() const
    {
        return command_buffer_ != nullptr && swapchain_texture_ != nullptr;
    }

    int32_t TargetWidth() const
    {
        return depth_width_;
    }

    int32_t TargetHeight() const
    {
        return depth_height_;
    }

  private:
    bool CreateDepthTexture(int32_t width, int32_t height);

    SDL_Window           *window_            = nullptr;
    SDL_GPUDevice        *device_            = nullptr;
    SDL_GPUCommandBuffer *command_buffer_    = nullptr;
    SDL_GPURenderPass    *render_pass_       = nullptr;
    SDL_GPUTexture       *swapchain_texture_ = nullptr;
    SDL_GPUTexture       *depth_texture_     = nullptr;

    SDL_GPUTextureFormat swapchain_format_ = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUTextureFormat depth_format_     = SDL_GPU_TEXTUREFORMAT_INVALID;

    int32_t depth_width_  = 0;
    int32_t depth_height_ = 0;

    RGBAColor clear_color_ = kRGBABlack;

    bool color_written_ = false;
};

extern GpuDevice gpu_device;
