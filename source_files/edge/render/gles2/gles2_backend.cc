#include "edge_profiling.h"
#include "../../r_backend.h"

#include "epi.h"
#include "epi_bam.h"
#include "g_game.h"
#include "gles2_immediate.h"
#include "gles2_program.h"
#include "i_defs_gl.h"
#include "r_colormap.h"
#include "r_draw.h"
#include "r_gldefs.h"
#include "r_image.h"
#include "r_misc.h"
#include "r_modes.h"
#include "r_state.h"

void SetupSkyMatrices(void);

static constexpr float kGles2DegreesToRadians = HMM_DegToRad;

static inline const char *SafeStr(const void *s)
{
    return s ? (const char *)s : "";
}

class Gles2RenderBackend : public RenderBackend
{
  protected:
    void SetupMatrices2D(bool flip)
    {
        gles2_immediate.Viewport(0, 0, current_screen_width, current_screen_height);

        gles2_immediate.MatrixModeProjection();
        gles2_immediate.LoadIdentity();

        if (flip)
            gles2_immediate.Orthographic((float)current_screen_width, 0.0f, 0.0f, (float)current_screen_height, -1.0f,
                                         1.0f);
        else
            gles2_immediate.Orthographic(0.0f, (float)current_screen_width, 0.0f, (float)current_screen_height, -1.0f,
                                         1.0f);

        gles2_immediate.MatrixModeModelView();
        gles2_immediate.LoadIdentity();
    }

    void SetupWorldMatrices2D()
    {
        gles2_immediate.Viewport(view_window_x, view_window_y, view_window_width, view_window_height);

        gles2_immediate.MatrixModeProjection();
        gles2_immediate.LoadIdentity();

        if (fliplevels.d_)
            gles2_immediate.Orthographic((float)view_window_width, (float)view_window_x, (float)view_window_y,
                                         (float)view_window_height, -1.0f, 1.0f);
        else
            gles2_immediate.Orthographic((float)view_window_x, (float)view_window_width, (float)view_window_y,
                                         (float)view_window_height, -1.0f, 1.0f);

        gles2_immediate.MatrixModeModelView();
        gles2_immediate.LoadIdentity();
    }

    void SetupMatrices3D()
    {
        gles2_immediate.Viewport(view_window_x, view_window_y, view_window_width, view_window_height);

        gles2_immediate.MatrixModeProjection();
        gles2_immediate.LoadIdentity();

        if (fliplevels.d_)
            gles2_immediate.Frustum(view_x_slope * renderer_near_clip.f_, -view_x_slope * renderer_near_clip.f_,
                                    -view_y_slope * renderer_near_clip.f_, view_y_slope * renderer_near_clip.f_,
                                    renderer_near_clip.f_, renderer_far_clip.f_);
        else
            gles2_immediate.Frustum(-view_x_slope * renderer_near_clip.f_, view_x_slope * renderer_near_clip.f_,
                                    -view_y_slope * renderer_near_clip.f_, view_y_slope * renderer_near_clip.f_,
                                    renderer_near_clip.f_, renderer_far_clip.f_);

        gles2_immediate.MatrixModeModelView();
        gles2_immediate.LoadIdentity();
        gles2_immediate.Rotate(270.0f * kGles2DegreesToRadians - (float)epi::RadiansFromBAM(view_vertical_angle), 1.0f,
                               0.0f, 0.0f);
        gles2_immediate.Rotate(90.0f * kGles2DegreesToRadians - (float)epi::RadiansFromBAM(view_angle), 0.0f, 0.0f,
                               1.0f);

        if (!epi::AlmostEquals(view_rotation, 0.0f))
            gles2_immediate.Rotate(view_rotation * kGles2DegreesToRadians, view_forward.X, view_forward.Y,
                                   view_forward.Z);

        gles2_immediate.Translate(-view_x, -view_y, -view_z);
    }

  public:
    void Init()
    {
        LogPrint("OpenGL: Initialising...\n");

        Gles2LoadEntryPoints();

        LogPrint("OpenGL: Version: %s\n", SafeStr(glGetString(GL_VERSION)));
        LogPrint("OpenGL: Renderer: %s\n", SafeStr(glGetString(GL_RENDERER)));
        LogPrint("OpenGL: Vendor: %s\n", SafeStr(glGetString(GL_VENDOR)));
        LogPrint("OpenGL: Shading Language: %s\n", SafeStr(glGetString(GL_SHADING_LANGUAGE_VERSION)));

        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size_);

        LogPrint("OpenGL: Max Texture Size: %d\n", max_texture_size_);

        Gles2DetectStencilBuffer();

        LogPrint("OpenGL: Stencil Buffer: %s\n", render_state->HasStencilBuffer() ? "yes" : "no");

        int32_t varying_vectors = Gles2MaxVaryingVectors();

        if (varying_vectors < 3)
            FatalError("OpenGL: the world shader needs 3 varying vectors, the driver offers %d\n", varying_vectors);

        if (!gles2_movie_program.Init())
            FatalError("Gles2Backend: movie program initialisation failed\n");

        if (!gles2_program.Init())
            FatalError("OpenGL: failed to create the world shader program.\n");

        if (!gles2_model_program.Init())
            FatalError("OpenGL: failed to create the model shader program.\n");

        if (!gles2_immediate.Init())
            FatalError("OpenGL: failed to initialise the immediate renderer.\n");

        gles2_program.Use();

        RenderBackend::Init();
    }

    void CaptureScreen(int32_t width, int32_t height, int32_t stride, uint8_t *dest)
    {
        render_state->Flush();
        render_state->PixelStorei(GL_UNPACK_ALIGNMENT, 1);

        for (int32_t y = 0; y < height; y++)
        {
            render_state->ReadPixels(0, y, width, 1, GL_RGBA, GL_UNSIGNED_BYTE, dest);
            dest += stride;
        }
    }

    void StartFrame(int32_t width, int32_t height)
    {
        EPI_UNUSED(width);
        EPI_UNUSED(height);

        frame_number_++;

        render_state->Reset();

        gles2_program.Use();
        gles2_program.ResetStatistics();

        gles2_immediate.BeginFrame();
        gles2_immediate.ResetStatistics();

        render_state->ClearColor(clear_color_);

        SetRenderLayer(kRenderLayerHUD);
    }

    void SwapBuffers()
    {
    }

    void FinishFrame()
    {
        gles2_immediate.InvalidateBatch();

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
        gles2_immediate.Shutdown();

        gles2_program.Shutdown();
        gles2_model_program.Shutdown();
        gles2_movie_program.Shutdown();
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
    }

    void FinishWorldRender()
    {
        SetRenderLayer(kRenderLayerHUD);
    }

    void SetRenderLayer(RenderLayer layer, bool clear_depth = false)
    {
        render_layer_ = layer;

        if (layer == kRenderLayerHUD)
        {
            SetupMatrices2D(false);
        }
        else if (layer == kRenderLayerSky)
        {
            SetupSkyMatrices();
        }
        else if (layer == kRenderLayerViewport)
        {
            SetupWorldMatrices2D();
        }
        else
        {
            SetupMatrices3D();
        }

        if (clear_depth)
        {
            gles2_immediate.ClearDepth();
        }
    }

    RenderLayer GetRenderLayer()
    {
        return render_layer_;
    }

    void GetFrameStats(FrameStats &stats)
    {
        EPI_CLEAR_MEMORY(&stats, FrameStats, 1);

        stats.num_draw_           = gles2_immediate.DrawCount();
        stats.num_apply_uniforms_ = gles2_program.UniformUpdateCount();
        stats.num_update_buffer_  = gles2_immediate.UploadCount();

        stats.size_update_buffer_ = (uint32_t)gles2_immediate.UploadedBytes();
    }

  private:
    RenderLayer render_layer_ = kRenderLayerInvalid;

    RGBAColor clear_color_ = kRGBABlack;
};

static Gles2RenderBackend gles2_render_backend;
RenderBackend            *render_backend = &gles2_render_backend;
