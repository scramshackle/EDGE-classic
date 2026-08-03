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

    void BeginPass(GpuLoadOperation color_load, GpuLoadOperation depth_load, GpuLoadOperation stencil_load);

    void EndPass();

    SDL_GPUCommandBuffer *BeginUpload();

    void EndUpload(SDL_GPUCommandBuffer *upload_buffer);

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

    bool HasStencil() const
    {
        return has_stencil_;
    }

    bool FrameAcquired() const
    {
        return command_buffer_ != nullptr && swapchain_texture_ != nullptr;
    }

    int32_t TargetWidth() const
    {
        return target_width_;
    }

    int32_t TargetHeight() const
    {
        return target_height_;
    }

    bool ReadColorTarget(int32_t width, int32_t height, int32_t stride, uint8_t *dest);

    bool ReadColorRegion(int32_t x, int32_t y, int32_t width, int32_t height, int32_t stride, uint8_t *dest);

  private:
    bool CreateFrameTextures(int32_t width, int32_t height);

    SDL_Window           *window_            = nullptr;
    SDL_GPUDevice        *device_            = nullptr;
    SDL_GPUCommandBuffer *command_buffer_    = nullptr;
    SDL_GPURenderPass    *render_pass_       = nullptr;
    SDL_GPUTexture       *swapchain_texture_ = nullptr;
    SDL_GPUTexture       *color_texture_     = nullptr;
    SDL_GPUTexture       *depth_texture_     = nullptr;

    SDL_GPUTransferBuffer *download_buffer_          = nullptr;
    uint32_t               download_buffer_capacity_ = 0;

    SDL_GPUTextureFormat swapchain_format_ = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUTextureFormat depth_format_     = SDL_GPU_TEXTUREFORMAT_INVALID;

    int32_t target_width_  = 0;
    int32_t target_height_ = 0;

    RGBAColor clear_color_ = kRGBABlack;

    bool color_written_ = false;
    bool has_stencil_   = false;
};

extern GpuDevice gpu_device;
