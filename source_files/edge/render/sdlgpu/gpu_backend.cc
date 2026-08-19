#include "edge_profiling.h"
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
#include "i_system.h"
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

        if (world_model_matrix_total_ > 0)
            gpu_immediate.MultiplyMatrix(world_model_matrix_);

        if (oblique_near_plane_active_)
        {
            HMM_Vec4 eye_plane = EyeSpacePlane(gpu_immediate.ModelViewMatrix(), oblique_near_plane_);

            gpu_immediate.MatrixModeProjection();
            gpu_immediate.LoadMatrix(ObliqueNearPlaneProjection(gpu_immediate.ProjectionMatrix(), eye_plane, kClipVolumeZeroToW));
            gpu_immediate.MatrixModeModelView();
        }
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

    HMM_Mat4 WorldViewProjection()
    {
        return gpu_immediate.ProjectionMatrix() * gpu_immediate.ModelViewMatrix();
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

        UpdateRenderTargetSize();

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

        EDGE_FrameMark;
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
        DestroyModelShaders(gpu_device.Handle());
        DestroyLightShaders(gpu_device.Handle());

        gpu_device.Shutdown();
    }

    void SetClearColor(RGBAColor color)
    {
        clear_color_ = color;
    }

    void GetPassInfo(PassInfo &info)
    {
        info.width_  = current_screen_width;
        info.height_ = current_screen_height;
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

        if (!gpu_device.EnsureWorldTextures(render_target_width_, render_target_height_))
            FatalError("GpuRenderBackend: world render target creation failed\n");

        render_target_active_ = true;

        gpu_immediate.BeginWorldTarget();
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

        if (render_target_active_)
        {
            GpuResolveArguments resolve;

            resolve.source_x      = ScaleToRenderTargetX(view_window_x);
            resolve.source_y      = ScaleToRenderTargetY(view_window_y);
            resolve.source_width  = ScaleToRenderTargetX(view_window_width);
            resolve.source_height = ScaleToRenderTargetY(view_window_height);

            resolve.destination_x      = view_window_x;
            resolve.destination_y      = view_window_y;
            resolve.destination_width  = view_window_width;
            resolve.destination_height = view_window_height;

            resolve.smooth = image_smoothing > 0;

            render_target_active_ = false;

            gpu_immediate.ResolveWorldTarget(resolve);
        }

        SetRenderLayer(kRenderLayerHUD);
    }

    bool OitSinglePass()
    {
        return true;
    }

    void BeginOitPass()
    {
        gpu_immediate.BeginOitTarget();
    }

    void SetOitPass(int32_t mode)
    {
        oit_mode_ = mode;

        gpu_immediate.SetOitPipeline(mode == kOitPassAccumulate);
    }

    void FinishOitPass()
    {
        gpu_immediate.SetOitPipeline(false);

        gpu_immediate.EndOitTarget();

        CompositeOit();

        oit_mode_ = 0;
    }

    void CompositeOit()
    {
        const GpuImage *accumulation = GetGpuImage(kGpuImageOitAccumulation);
        const GpuImage *revealage    = GetGpuImage(kGpuImageOitRevealage);

        if (!accumulation || !revealage)
            return;

        float target_width  = (float)gpu_device.WorldWidth();
        float target_height = (float)gpu_device.WorldHeight();

        if (target_width < 1.0f || target_height < 1.0f)
            return;

        float view_x      = (float)ScaleToRenderTargetX(view_window_x);
        float view_y      = (float)ScaleToRenderTargetY(view_window_y);
        float view_width  = (float)ScaleToRenderTargetX(view_window_width);
        float view_height = (float)ScaleToRenderTargetY(view_window_height);

        float u0 = view_x / target_width;
        float u1 = (view_x + view_width) / target_width;

        float v_top    = (target_height - view_y - view_height) / target_height;
        float v_bottom = (target_height - view_y) / target_height;

        gpu_immediate.SetPipelineState(kGpuPipelineBlend, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        gpu_immediate.MatrixModeProjection();
        gpu_immediate.PushMatrix();
        gpu_immediate.LoadIdentity();

        gpu_immediate.MatrixModeModelView();
        gpu_immediate.PushMatrix();
        gpu_immediate.LoadIdentity();

        gpu_immediate.SetSkyPass(nullptr);
        gpu_immediate.SetLightDepth(false);
        gpu_immediate.SetSkipRGB(false);
        gpu_immediate.SetLineMode(false);
        gpu_immediate.SetViewTint(1.0f, 1.0f, 1.0f);
        gpu_immediate.SetTextureOffset({{0.0f, 0.0f}});
        gpu_immediate.SetLiquid({{0.0f, 0.0f}});
        gpu_immediate.SetOitComposite(true);

        gpu_immediate.SetMultiTexture(accumulation->texture, accumulation->sampler, revealage->texture,
                                      revealage->sampler);

        RendererVertex quad[4];

        EPI_CLEAR_MEMORY(quad, RendererVertex, 4);

        for (int32_t i = 0; i < 4; i++)
            quad[i].rgba = kRGBAWhite;

        quad[0].position               = {{-1.0f, -1.0f, 0.0f}};
        quad[0].texture_coordinates[0] = {{u0, v_bottom}};

        quad[1].position               = {{1.0f, -1.0f, 0.0f}};
        quad[1].texture_coordinates[0] = {{u1, v_bottom}};

        quad[2].position               = {{1.0f, 1.0f, 0.0f}};
        quad[2].texture_coordinates[0] = {{u1, v_top}};

        quad[3].position               = {{-1.0f, 1.0f, 0.0f}};
        quad[3].texture_coordinates[0] = {{u0, v_top}};

        gpu_immediate.Draw(GL_QUADS, quad, 4);

        gpu_immediate.SetOitComposite(false);

        gpu_immediate.MatrixModeModelView();
        gpu_immediate.PopMatrix();

        gpu_immediate.MatrixModeProjection();
        gpu_immediate.PopMatrix();

        gpu_immediate.MatrixModeModelView();
    }

    void PushModelMatrix(const HMM_Mat4 &matrix)
    {
        EPI_ASSERT(world_model_matrix_total_ < kMaximumWorldModelMatrices);

        world_model_matrix_stack_[world_model_matrix_total_++] = world_model_matrix_;

        world_model_matrix_ = HMM_MulM4(world_model_matrix_, matrix);

        SetupMatrices3D();
    }

    void PopModelMatrix()
    {
        EPI_ASSERT(world_model_matrix_total_ > 0);

        world_model_matrix_ = world_model_matrix_stack_[--world_model_matrix_total_];

        SetupMatrices3D();
    }

    void SetObliqueNearPlane(bool enabled, const HMM_Vec4 &plane)
    {
        oblique_near_plane_active_ = enabled;
        oblique_near_plane_        = plane;

        SetupMatrices3D();
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

    static constexpr int32_t kMaximumWorldModelMatrices = 8;

    HMM_Mat4 world_model_matrix_                                   = HMM_M4D(1.0f);

    bool     oblique_near_plane_active_ = false;
    HMM_Vec4 oblique_near_plane_        = {};
    HMM_Mat4 world_model_matrix_stack_[kMaximumWorldModelMatrices] = {};
    int32_t  world_model_matrix_total_                             = 0;

    RenderLayer render_layer_ = kRenderLayerInvalid;

    RGBAColor clear_color_ = kRGBABlack;

    WorldState world_state_[kRenderWorldMax];

    int32_t world_state_index_ = kGpuWorldStateInvalid;
};

static GpuRenderBackend gpu_render_backend;
RenderBackend          *render_backend = &gpu_render_backend;
