
#pragma once

#include "con_var.h"
#include "dm_defs.h"
#include "epi.h"
#include "epi_color.h"
#include "epi_math.h"
#include "epi_vector.h"

extern ConsoleVariable fliplevels;
extern ConsoleVariable render_scale;

struct PassInfo
{
    int32_t width_;
    int32_t height_;
};
constexpr int32_t kRenderWorldMax = 8;

enum RenderLayer
{
    kRenderLayerHUD = 0,
    kRenderLayerSky,
    kRenderLayerSolid,
    kRenderLayerTransparent, // Transparent - additive renders on this layer
    kRenderLayerViewport, // Weapon sprites and 2D effects that use viewport instead of full screen space like the HUD
    kRenderLayerMax,
    kRenderLayerInvalid
};

enum OitPass
{
    kOitPassNone = 0,
    kOitPassAccumulate,
    kOitPassRevealage,
    kOitPassMasked,
    kOitPassAdditive
};

enum ClipVolumeDepthRange
{
    kClipVolumeZeroToW = 0,
    kClipVolumeNegativeWToW
};

inline epi::Mat4 ObliqueNearPlaneProjection(const epi::Mat4 &projection, const epi::Vec4 &eye_plane,
                                            ClipVolumeDepthRange range)
{
    if (epi::AlmostEquals(projection.elements[0][0], 0.0f) || epi::AlmostEquals(projection.elements[1][1], 0.0f) ||
        epi::AlmostEquals(projection.elements[3][2], 0.0f))
        return projection;

    float sign_x = (eye_plane.x > 0.0f) ? 1.0f : ((eye_plane.x < 0.0f) ? -1.0f : 0.0f);
    float sign_y = (eye_plane.y > 0.0f) ? 1.0f : ((eye_plane.y < 0.0f) ? -1.0f : 0.0f);

    epi::Vec4 corner;

    corner.x = (sign_x + projection.elements[2][0]) / projection.elements[0][0];
    corner.y = (sign_y + projection.elements[2][1]) / projection.elements[1][1];
    corner.z = -1.0f;
    corner.w = (1.0f + projection.elements[2][2]) / projection.elements[3][2];

    float denominator = epi::DotProduct(eye_plane, corner);

    if (epi::AlmostEquals(denominator, 0.0f))
        return projection;

    float near_bias = (range == kClipVolumeNegativeWToW) ? 1.0f : 0.0f;
    float scale     = (1.0f + near_bias) / denominator;

    epi::Mat4 result = projection;

    result.elements[0][2] = eye_plane.x * scale;
    result.elements[1][2] = eye_plane.y * scale;
    result.elements[2][2] = eye_plane.z * scale + near_bias;
    result.elements[3][2] = eye_plane.w * scale;

    return result;
}

inline epi::Vec4 EyeSpacePlane(const epi::Mat4 &model_view, const epi::Vec4 &plane)
{
    epi::Mat4 inverse = epi::InverseMatrix(model_view);

    epi::Vec4 result;

    result.x = epi::DotProduct(epi::MatrixColumn(inverse, 0), plane);
    result.y = epi::DotProduct(epi::MatrixColumn(inverse, 1), plane);
    result.z = epi::DotProduct(epi::MatrixColumn(inverse, 2), plane);
    result.w = epi::DotProduct(epi::MatrixColumn(inverse, 3), plane);

    return result;
}

struct LightGrid;

struct FrameFinishedCallback
{
    void (*function)(void *context);
    void *context;
};

struct FrameStats
{
    uint32_t num_apply_pipeline_;
    uint32_t num_apply_bindings_;
    uint32_t num_apply_uniforms_;
    uint32_t num_draw_;
    uint32_t num_update_buffer_;
    uint32_t num_update_image_;

    uint32_t size_apply_uniforms_;
    uint32_t size_update_buffer_;
    uint32_t size_append_buffer_;
};

class RenderBackend
{
  public:
    virtual void SetClearColor(RGBAColor color) = 0;

    virtual void StartFrame(int32_t width, int32_t height) = 0;

    virtual void SwapBuffers() = 0;

    virtual void FinishFrame() = 0;

    void OnFrameFinished(void (*function)(void *context), void *context)
    {
        FrameFinishedCallback callback;
        callback.function = function;
        callback.context  = context;
        on_frame_finished_.push_back(callback);
    }

    virtual void BeginWorldRender() = 0;

    virtual void FinishWorldRender() = 0;

    virtual void SetRenderLayer(RenderLayer layer, bool clear_depth = false) = 0;

    virtual void PushModelMatrix(const epi::Mat4 &matrix) = 0;

    virtual void PopModelMatrix() = 0;

    virtual void SetObliqueNearPlane(bool enabled, const epi::Vec4 &plane) = 0;

    virtual bool OitSinglePass()
    {
        return false;
    }

    virtual void BeginOitPass()
    {
    }

    virtual void SetOitPass(int32_t mode)
    {
        EPI_UNUSED(mode);
    }

    virtual void FinishOitPass()
    {
    }

    int32_t OitMode() const
    {
        return oit_mode_;
    }

    virtual RenderLayer GetRenderLayer() = 0;

    void LockRenderUnits(bool locked)
    {
        units_locked_ = locked;
    }

    bool RenderUnitsLocked()
    {
        return units_locked_;
    }

    virtual void Resize(int32_t width, int32_t height) = 0;

    virtual void Shutdown() = 0;

    virtual void SoftInit();

    virtual void Init();

    virtual void GetPassInfo(PassInfo &info) = 0;

    virtual epi::Mat4 WorldViewProjection() = 0;

    virtual epi::Mat4 WorldModelView() = 0;

    virtual void UploadLightGrid(const LightGrid *grid) = 0;

    virtual void UploadColorLookup(int slot, const uint8_t *pixels) = 0;

    virtual void CaptureScreen(int32_t width, int32_t height, int32_t stride, uint8_t *dest) = 0;

    virtual void GetFrameStats(FrameStats &stats) = 0;

    int64_t GetFrameNumber()
    {
        return frame_number_;
    }


    int32_t GetMaxTextureSize() const
    {
        return max_texture_size_;
    }

    bool SupportsFullNonPowerOfTwoTextures() const
    {
        return full_non_power_of_two_textures_;
    }

    int32_t RenderTargetWidth() const
    {
        return render_target_width_;
    }

    int32_t RenderTargetHeight() const
    {
        return render_target_height_;
    }

    float RenderTargetScaleX() const
    {
        return render_target_scale_x_;
    }

    float RenderTargetScaleY() const
    {
        return render_target_scale_y_;
    }

    bool RenderTargetScaled() const
    {
        return render_target_scaled_;
    }

    bool RenderTargetActive() const
    {
        return render_target_active_;
    }

    int32_t ScaleToRenderTargetX(int32_t value) const
    {
        return render_target_active_ ? (int32_t)((float)value * render_target_scale_x_ + 0.5f) : value;
    }

    int32_t ScaleToRenderTargetY(int32_t value) const
    {
        return render_target_active_ ? (int32_t)((float)value * render_target_scale_y_ + 0.5f) : value;
    }

    float ActiveScaleX() const
    {
        return render_target_active_ ? render_target_scale_x_ : 1.0f;
    }

    float ActiveScaleY() const
    {
        return render_target_active_ ? render_target_scale_y_ : 1.0f;
    }

    // Setup the GL matrices for drawing 2D stuff.
    virtual void SetupMatrices2D(bool flip) = 0;

  protected:
    bool UpdateRenderTargetSize();

    int32_t max_texture_size_ = 0;
    int32_t oit_mode_         = 0;
    int64_t frame_number_;
    bool    units_locked_ = false;

    bool full_non_power_of_two_textures_ = true;

    int32_t render_target_width_   = 0;
    int32_t render_target_height_  = 0;
    float   render_target_scale_x_ = 1.0f;
    float   render_target_scale_y_ = 1.0f;
    bool    render_target_scaled_  = false;
    bool    render_target_active_  = false;

    std::vector<FrameFinishedCallback> on_frame_finished_;

    // Setup the GL matrices for drawing 2D stuff within the "world" rendered by
    // HUDRenderWorld
    virtual void SetupWorldMatrices2D() = 0;

    // Setup the GL matrices for drawing 3D stuff.
    virtual void SetupMatrices3D() = 0;
};

extern RenderBackend *render_backend;