#include "../../r_backend.h"

#include "epi.h"
#include "epi_bam.h"
#include "g_game.h"
#include "gpu_device.h"
#include "gpu_images.h"
#include "gpu_immediate.h"
#include "gpu_pipeline.h"
#include "gpu_shaders.h"
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

static constexpr float kGpuDegreesToRadians = HMM_DegToRad;

class GpuRenderBackend : public RenderBackend
{
  protected:
    void SetupMatrices2D(bool flip)
    {
        gpu_immediate.Viewport(0, 0, current_screen_width, current_screen_height);

        gpu_immediate.MatrixModeProjection();
        gpu_immediate.LoadIdentity();

        if (flip)
            gpu_immediate.Orthographic((float)current_screen_width, 0.0f, 0.0f, (float)current_screen_height, -1.0f,
                                       1.0f);
        else
            gpu_immediate.Orthographic(0.0f, (float)current_screen_width, 0.0f, (float)current_screen_height, -1.0f,
                                       1.0f);

        gpu_immediate.MatrixModeModelView();
        gpu_immediate.LoadIdentity();
    }

    void SetupWorldMatrices2D()
    {
        gpu_immediate.Viewport(view_window_x, view_window_y, view_window_width, view_window_height);

        gpu_immediate.MatrixModeProjection();
        gpu_immediate.LoadIdentity();

        if (fliplevels.d_)
            gpu_immediate.Orthographic((float)view_window_width, (float)view_window_x, (float)view_window_y,
                                       (float)view_window_height, -1.0f, 1.0f);
        else
            gpu_immediate.Orthographic((float)view_window_x, (float)view_window_width, (float)view_window_y,
                                       (float)view_window_height, -1.0f, 1.0f);

        gpu_immediate.MatrixModeModelView();
        gpu_immediate.LoadIdentity();
    }

    void SetupMatrices3D()
    {
        gpu_immediate.Viewport(view_window_x, view_window_y, view_window_width, view_window_height);

        gpu_immediate.MatrixModeProjection();
        gpu_immediate.LoadIdentity();

        if (fliplevels.d_)
            gpu_immediate.Frustum(view_x_slope * renderer_near_clip.f_, -view_x_slope * renderer_near_clip.f_,
                                  -view_y_slope * renderer_near_clip.f_, view_y_slope * renderer_near_clip.f_,
                                  renderer_near_clip.f_, renderer_far_clip.f_);
        else
            gpu_immediate.Frustum(-view_x_slope * renderer_near_clip.f_, view_x_slope * renderer_near_clip.f_,
                                  -view_y_slope * renderer_near_clip.f_, view_y_slope * renderer_near_clip.f_,
                                  renderer_near_clip.f_, renderer_far_clip.f_);

        gpu_immediate.MatrixModeModelView();
        gpu_immediate.LoadIdentity();
        gpu_immediate.Rotate(270.0f * kGpuDegreesToRadians - (float)epi::RadiansFromBAM(view_vertical_angle), 1.0f,
                             0.0f, 0.0f);
        gpu_immediate.Rotate(90.0f * kGpuDegreesToRadians - (float)epi::RadiansFromBAM(view_angle), 0.0f, 0.0f, 1.0f);

        if (!epi::AlmostEquals(view_rotation, 0.0f))
            gpu_immediate.Rotate(view_rotation * kGpuDegreesToRadians, view_forward.X, view_forward.Y, view_forward.Z);

        gpu_immediate.Translate(-view_x, -view_y, -view_z);
    }

  public:
    void Init()
    {
        LogPrint("SDL_GPU: Initialising...\n");

        max_texture_size_ = 4096;

        LogPrint("SDL_GPU: Max Texture Size: %d\n", max_texture_size_);

        if (!CreateWorldShaders(gpu_device.Handle()))
            FatalError("SDL_GPU: failed to create the world shaders.\n");

        if (!InitPipelines(gpu_device.Handle(), gpu_device.SwapchainFormat(), gpu_device.DepthFormat()))
            FatalError("SDL_GPU: failed to initialise the pipeline cache.\n");

        if (!gpu_immediate.Init(gpu_device.Handle()))
            FatalError("SDL_GPU: failed to initialise the immediate recorder.\n");

        EPI_CLEAR_MEMORY(world_state_, WorldState, kRenderWorldMax);

        RenderBackend::Init();
    }

    void CaptureScreen(int32_t width, int32_t height, int32_t stride, uint8_t *dest)
    {
        if (!gpu_device.ReadColorTarget(width, height, stride, dest))
            memset(dest, 0, (size_t)stride * (size_t)height);
    }

    void StartFrame(int32_t width, int32_t height)
    {
        frame_number_++;

        FlushDeletedGpuImages(gpu_device.Handle());

        if (vsync.CheckModified())
            gpu_device.SetVerticalSync(vsync.d_);

        gpu_device.AcquireFrame(width, height);

        gpu_device.SetClearColor(clear_color_);

        render_state->Reset();

        gpu_immediate.BeginFrame();

        EPI_CLEAR_MEMORY(world_state_, WorldState, kRenderWorldMax);

        world_state_index_ = kGpuWorldStateInvalid;

        SetRenderLayer(kRenderLayerHUD);
    }

    void SwapBuffers()
    {
    }

    void FinishFrame()
    {
        gpu_immediate.Replay();

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
        gpu_immediate.Shutdown(gpu_device.Handle());

        ShutdownGpuImages(gpu_device.Handle());

        ShutdownPipelines(gpu_device.Handle());

        DestroyWorldShaders(gpu_device.Handle());

        gpu_device.Shutdown();
    }

    void SetClearColor(RGBAColor color)
    {
        clear_color_ = color;
    }

    void GetPassInfo(PassInfo &info)
    {
        info.width_  = gpu_device.TargetWidth() ? gpu_device.TargetWidth() : current_screen_width;
        info.height_ = gpu_device.TargetHeight() ? gpu_device.TargetHeight() : current_screen_height;
    }

    void BeginWorldRender()
    {
        int32_t i = 0;

        for (; i < kRenderWorldMax; i++)
        {
            if (world_state_[i].active_)
            {
                FatalError("GpuRenderBackend: BeginWorldRender called with active world");
            }

            if (!world_state_[i].used_)
            {
                break;
            }
        }

        if (i == kRenderWorldMax)
        {
            FatalError("GpuRenderBackend: BeginWorldRender max worlds exceeded");
        }

        world_state_[i].active_ = true;
        world_state_[i].used_   = true;

        world_state_index_ = i;
    }

    void FinishWorldRender()
    {
        world_state_index_ = kGpuWorldStateInvalid;

        int32_t i = 0;

        for (; i < kRenderWorldMax; i++)
        {
            if (world_state_[i].active_)
            {
                world_state_[i].active_ = false;
                break;
            }
        }

        if (i == kRenderWorldMax)
        {
            FatalError("GpuRenderBackend: FinishWorldRender called with no active world render");
        }

        SetRenderLayer(kRenderLayerHUD);
    }

    void SetupMatrices(RenderLayer layer)
    {
        if (layer == kRenderLayerHUD)
        {
            SetupMatrices2D(false);
        }
        else if (layer == kRenderLayerViewport)
        {
            SetupWorldMatrices2D();
        }
        else
        {
            SetupMatrices3D();
        }
    }

    void SetRenderLayer(RenderLayer layer, bool clear_depth = false)
    {
        render_layer_ = layer;

        SetupMatrices(layer);

        if (clear_depth)
            gpu_immediate.ClearDepth();
    }

    RenderLayer GetRenderLayer()
    {
        return render_layer_;
    }

    void GetFrameStats(FrameStats &stats)
    {
        EPI_CLEAR_MEMORY(&stats, FrameStats, 1);

        stats.num_apply_pipeline_ = gpu_immediate.PipelineBindCount();
        stats.num_apply_bindings_ = gpu_immediate.BindingCount();
        stats.num_apply_uniforms_ = gpu_immediate.UniformPushCount();
        stats.num_draw_           = gpu_immediate.DrawCount();
        stats.num_update_buffer_  = gpu_immediate.UploadedBytes() ? 1 : 0;

        stats.size_apply_uniforms_ = (uint32_t)gpu_immediate.UniformBytes();
        stats.size_update_buffer_  = (uint32_t)gpu_immediate.UploadedBytes();
    }

  private:
    struct WorldState
    {
        bool active_;
        bool used_;
    };

    static constexpr int32_t kGpuWorldStateInvalid = -1;

    RenderLayer render_layer_ = kRenderLayerInvalid;

    RGBAColor clear_color_ = kRGBABlack;

    WorldState world_state_[kRenderWorldMax];

    int32_t world_state_index_ = kGpuWorldStateInvalid;
};

static GpuRenderBackend gpu_render_backend;
RenderBackend          *render_backend = &gpu_render_backend;
