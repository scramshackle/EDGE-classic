#include <math.h>

#include "con_var.h"
#include "dm_state.h"
#include "epi.h"
#include "epi_bam.h"
#include "g_game.h"
#include "gpu_immediate.h"
#include "i_defs_gl.h"
#include "m_math.h"
#include "n_network.h"
#include "p_tick.h"
#include "r_colormap.h"
#include "r_gldefs.h"
#include "r_image.h"
#include "r_misc.h"
#include "r_modes.h"
#include "r_sky.h"
#include "r_state.h"
#include "r_units.h"

extern ConsoleVariable fliplevels;
extern SkyStretch      current_sky_stretch;

static constexpr float kGpuDegreesToRadians = HMM_DegToRad;

void SetupSkyMatrices(void)
{
    if (custom_skybox)
    {
        gpu_immediate.MatrixModeProjection();
        gpu_immediate.PushMatrix();
        gpu_immediate.LoadIdentity();

        if (fliplevels.d_)
            gpu_immediate.Frustum(-view_x_slope * renderer_near_clip.f_, view_x_slope * renderer_near_clip.f_,
                                  -view_y_slope * renderer_near_clip.f_, view_y_slope * renderer_near_clip.f_,
                                  renderer_near_clip.f_, renderer_far_clip.f_);
        else
            gpu_immediate.Frustum(view_x_slope * renderer_near_clip.f_, -view_x_slope * renderer_near_clip.f_,
                                  -view_y_slope * renderer_near_clip.f_, view_y_slope * renderer_near_clip.f_,
                                  renderer_near_clip.f_, renderer_far_clip.f_);

        gpu_immediate.MatrixModeModelView();
        gpu_immediate.PushMatrix();
        gpu_immediate.LoadIdentity();

        if (!epi::AlmostEquals(view_rotation, 0.0f))
            gpu_immediate.Rotate(view_rotation * kGpuDegreesToRadians, 0.0f, 0.0f, 1.0f);

        gpu_immediate.Rotate(270.0f * kGpuDegreesToRadians - (float)epi::RadiansFromBAM(view_vertical_angle), 1.0f,
                             0.0f, 0.0f);
        gpu_immediate.Rotate((float)epi::RadiansFromBAM(view_angle), 0.0f, 0.0f, 1.0f);
    }
    else
    {
        gpu_immediate.MatrixModeProjection();
        gpu_immediate.PushMatrix();
        gpu_immediate.LoadIdentity();

        if (fliplevels.d_)
            gpu_immediate.Frustum(view_x_slope * renderer_near_clip.f_, -view_x_slope * renderer_near_clip.f_,
                                  -view_y_slope * renderer_near_clip.f_, view_y_slope * renderer_near_clip.f_,
                                  renderer_near_clip.f_, renderer_far_clip.f_ * 4.0f);
        else
            gpu_immediate.Frustum(-view_x_slope * renderer_near_clip.f_, view_x_slope * renderer_near_clip.f_,
                                  -view_y_slope * renderer_near_clip.f_, view_y_slope * renderer_near_clip.f_,
                                  renderer_near_clip.f_, renderer_far_clip.f_ * 4.0f);

        gpu_immediate.MatrixModeModelView();
        gpu_immediate.PushMatrix();
        gpu_immediate.LoadIdentity();

        if (!epi::AlmostEquals(view_rotation, 0.0f))
            gpu_immediate.Rotate(-view_rotation * kGpuDegreesToRadians, 0.0f, 0.0f, 1.0f);

        gpu_immediate.Rotate(270.0f * kGpuDegreesToRadians - (float)epi::RadiansFromBAM(view_vertical_angle), 1.0f,
                             0.0f, 0.0f);

        BAMAngle rot = view_angle;

        if (sky_ref)
        {
            if (!epi::AlmostEquals(sky_ref->old_offset.X, sky_ref->offset.X) && !console_active && !paused &&
                !menu_active && !time_stop_active && !erraticism_active)
                rot += epi::BAMFromDegrees(HMM_Lerp(sky_ref->old_offset.X, fractional_tic, sky_ref->offset.X) /
                                           sky_image->ScaledWidth());
            else
                rot += epi::BAMFromDegrees(sky_ref->offset.X / sky_image->ScaledWidth());
        }

        gpu_immediate.Rotate(-(float)epi::RadiansFromBAM(rot), 0.0f, 0.0f, 1.0f);

        if (current_sky_stretch == kSkyStretchStretch)
            gpu_immediate.Translate(0.0f, 0.0f, (renderer_far_clip.f_ * 2 * 0.15f));
        else
            gpu_immediate.Translate(0.0f, 0.0f, -(renderer_far_clip.f_ * 2 * 0.15f));
    }
}

void RendererRevertSkyMatrices(void)
{
    gpu_immediate.MatrixModeProjection();
    gpu_immediate.PopMatrix();

    gpu_immediate.MatrixModeModelView();
    gpu_immediate.PopMatrix();
}
