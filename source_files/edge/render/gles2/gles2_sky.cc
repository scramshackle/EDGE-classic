#include <math.h>

#include "con_var.h"
#include "dm_state.h"
#include "epi.h"
#include "epi_bam.h"
#include "g_game.h"
#include "gles2_immediate.h"
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

static constexpr float kGles2DegreesToRadians = HMM_DegToRad;

void SetupSkyMatrices(void)
{
    if (custom_skybox)
    {
        gles2_immediate.MatrixModeProjection();
        gles2_immediate.PushMatrix();
        gles2_immediate.LoadIdentity();

        if (fliplevels.d_)
            gles2_immediate.Frustum(-view_x_slope * renderer_near_clip.f_, view_x_slope * renderer_near_clip.f_,
                                    -view_y_slope * renderer_near_clip.f_, view_y_slope * renderer_near_clip.f_,
                                    renderer_near_clip.f_, renderer_far_clip.f_);
        else
            gles2_immediate.Frustum(view_x_slope * renderer_near_clip.f_, -view_x_slope * renderer_near_clip.f_,
                                    -view_y_slope * renderer_near_clip.f_, view_y_slope * renderer_near_clip.f_,
                                    renderer_near_clip.f_, renderer_far_clip.f_);

        gles2_immediate.MatrixModeModelView();
        gles2_immediate.PushMatrix();
        gles2_immediate.LoadIdentity();

        if (!epi::AlmostEquals(view_rotation, 0.0f))
            gles2_immediate.Rotate(view_rotation * kGles2DegreesToRadians, 0.0f, 0.0f, 1.0f);

        gles2_immediate.Rotate(270.0f * kGles2DegreesToRadians - (float)epi::RadiansFromBAM(view_vertical_angle), 1.0f,
                               0.0f, 0.0f);
        gles2_immediate.Rotate((float)epi::RadiansFromBAM(view_angle), 0.0f, 0.0f, 1.0f);
    }
    else
    {
        gles2_immediate.MatrixModeProjection();
        gles2_immediate.PushMatrix();
        gles2_immediate.LoadIdentity();

        if (fliplevels.d_)
            gles2_immediate.Frustum(view_x_slope * renderer_near_clip.f_, -view_x_slope * renderer_near_clip.f_,
                                    -view_y_slope * renderer_near_clip.f_, view_y_slope * renderer_near_clip.f_,
                                    renderer_near_clip.f_, renderer_far_clip.f_ * 4.0f);
        else
            gles2_immediate.Frustum(-view_x_slope * renderer_near_clip.f_, view_x_slope * renderer_near_clip.f_,
                                    -view_y_slope * renderer_near_clip.f_, view_y_slope * renderer_near_clip.f_,
                                    renderer_near_clip.f_, renderer_far_clip.f_ * 4.0f);

        gles2_immediate.MatrixModeModelView();
        gles2_immediate.PushMatrix();
        gles2_immediate.LoadIdentity();

        if (!epi::AlmostEquals(view_rotation, 0.0f))
            gles2_immediate.Rotate(-view_rotation * kGles2DegreesToRadians, 0.0f, 0.0f, 1.0f);

        gles2_immediate.Rotate(270.0f * kGles2DegreesToRadians - (float)epi::RadiansFromBAM(view_vertical_angle), 1.0f,
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

        gles2_immediate.Rotate(-(float)epi::RadiansFromBAM(rot), 0.0f, 0.0f, 1.0f);

        if (current_sky_stretch == kSkyStretchStretch)
            gles2_immediate.Translate(0.0f, 0.0f, (renderer_far_clip.f_ * 2 * 0.15f));
        else
            gles2_immediate.Translate(0.0f, 0.0f, -(renderer_far_clip.f_ * 2 * 0.15f));
    }
}

void RendererRevertSkyMatrices(void)
{
    gles2_immediate.MatrixModeProjection();
    gles2_immediate.PopMatrix();

    gles2_immediate.MatrixModeModelView();
    gles2_immediate.PopMatrix();
}

void GetSkyInverseMatrices(HMM_Mat4 &inverse_projection, HMM_Mat4 &inverse_view)
{
    inverse_projection = HMM_InvGeneralM4(gles2_immediate.ProjectionMatrix());
    inverse_view       = HMM_InvGeneralM4(gles2_immediate.ModelViewMatrix());
}

static const GLenum kSkyCubeFaceTargets[6] = {
    GL_TEXTURE_CUBE_MAP_POSITIVE_X, GL_TEXTURE_CUBE_MAP_NEGATIVE_X, GL_TEXTURE_CUBE_MAP_POSITIVE_Y,
    GL_TEXTURE_CUBE_MAP_NEGATIVE_Y, GL_TEXTURE_CUBE_MAP_POSITIVE_Z, GL_TEXTURE_CUBE_MAP_NEGATIVE_Z};

GLuint CreateSkyCubemap(ImageData *faces[6], int face_size)
{
    GLuint cubemap = 0;

    glGenTextures(1, &cubemap);

    if (cubemap == 0)
        return 0;

    glBindTexture(GL_TEXTURE_CUBE_MAP, cubemap);

    for (int face = 0; face < 6; face++)
    {
        ImageData *data = faces[face];

        if (!data || !data->pixels_)
            continue;

        GLenum format = (data->depth_ == 4) ? GL_RGBA : GL_RGB;

        glTexImage2D(kSkyCubeFaceTargets[face], 0, format, data->width_, data->height_, 0, format, GL_UNSIGNED_BYTE,
                     data->pixels_);
    }

    EPI_UNUSED(face_size);

    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);

    return cubemap;
}

void DeleteSkyCubemap(GLuint cubemap)
{
    if (cubemap != 0)
        glDeleteTextures(1, &cubemap);
}
