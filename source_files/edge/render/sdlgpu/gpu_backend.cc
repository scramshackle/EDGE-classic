#include "../../r_backend.h"

#include "epi.h"
#include "g_game.h"
#include "gpu_device.h"
#include "i_defs_gl.h"
#include "r_colormap.h"
#include "r_draw.h"
#include "r_gldefs.h"
#include "r_image.h"
#include "r_misc.h"
#include "r_modes.h"
#include "r_state.h"

extern ConsoleVariable vsync;

void SetupSkyMatrices(void);

class GpuRenderBackend : public RenderBackend
{
  private:
    void SetupMatrices2D(bool flip)
    {
        EPI_UNUSED(flip);
    }

    void SetupWorldMatrices2D()
    {
    }

    void SetupMatrices3D()
    {
    }

  public:
    void Init()
    {
        LogPrint("SDL_GPU: Initialising...\n");

        max_texture_size_ = 4096;

        LogPrint("SDL_GPU: Max Texture Size: %d\n", max_texture_size_);

        RenderBackend::Init();
    }

    void CaptureScreen(int32_t width, int32_t height, int32_t stride, uint8_t *dest)
    {
        EPI_UNUSED(width);
        EPI_UNUSED(height);
        EPI_UNUSED(stride);
        EPI_UNUSED(dest);
    }

    void StartFrame(int32_t width, int32_t height)
    {
        frame_number_++;

        if (vsync.CheckModified())
            gpu_device.SetVerticalSync(vsync.d_);

        if (!gpu_device.AcquireFrame(width, height))
            return;

        gpu_device.BeginPass(kGpuLoadOperationClear, kGpuLoadOperationClear);
    }

    void SwapBuffers()
    {
    }

    void FinishFrame()
    {
        gpu_device.SubmitFrame();

        for (auto itr = on_frame_finished_.begin(); itr != on_frame_finished_.end(); itr++)
        {
            (*itr)();
        }

        on_frame_finished_.clear();
    }

    void Resize(int32_t width, int32_t height)
    {
        EPI_UNUSED(width);
        EPI_UNUSED(height);
    }

    void Shutdown()
    {
        gpu_device.Shutdown();
    }

    void SetClearColor(RGBAColor color)
    {
        gpu_device.SetClearColor(color);
    }

    void GetPassInfo(PassInfo &info)
    {
        info.width_  = current_screen_width;
        info.height_ = current_screen_height;
    }

    void BeginWorldRender()
    {
    }

    void FinishWorldRender()
    {
    }

    void SetRenderLayer(RenderLayer layer, bool clear_depth = false)
    {
        render_layer_ = layer;

        if (clear_depth)
            gpu_device.BeginPass(kGpuLoadOperationLoad, kGpuLoadOperationClear);
    }

    RenderLayer GetRenderLayer()
    {
        return render_layer_;
    }

    void Flush(int32_t commands, int32_t vertices)
    {
        EPI_UNUSED(commands);
        EPI_UNUSED(vertices);
    }

    void GetFrameStats(FrameStats &stats)
    {
        EPI_CLEAR_MEMORY(&stats, FrameStats, 1);
    }

    void OnContextSwitch()
    {
    }

  private:
    RenderLayer render_layer_ = kRenderLayerInvalid;
};

static GpuRenderBackend gpu_render_backend;
RenderBackend          *render_backend = &gpu_render_backend;
