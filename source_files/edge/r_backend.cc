
#include "r_backend.h"

#include "g_game.h"
#include "i_defs_gl.h"
#include "r_colormap.h"
#include "r_draw.h"
#include "r_gldefs.h"
#include "r_image.h"
#include "r_misc.h"
#include "r_modes.h"

EDGE_DEFINE_CONSOLE_VARIABLE(renderer_near_clip, "1", kConsoleVariableFlagArchive)
EDGE_DEFINE_CONSOLE_VARIABLE(renderer_far_clip, "64000", kConsoleVariableFlagArchive)
EDGE_DEFINE_CONSOLE_VARIABLE(draw_culling, "0", kConsoleVariableFlagArchive)
EDGE_DEFINE_CONSOLE_VARIABLE_CLAMPED(draw_culling_distance, "3000", kConsoleVariableFlagArchive, 1000.0f, 16000.0f)
EDGE_DEFINE_CONSOLE_VARIABLE(cull_fog_color, "0", kConsoleVariableFlagArchive)
EDGE_DEFINE_CONSOLE_VARIABLE(fliplevels, "0", kConsoleVariableFlagNone)
EDGE_DEFINE_CONSOLE_VARIABLE_CLAMPED(render_scale, "1.0", kConsoleVariableFlagArchive, 0.25f, 1.0f)

bool RenderBackend::UpdateRenderTargetSize()
{
    float scale = render_scale.f_;

    if (scale > 1.0f)
        scale = 1.0f;
    if (scale < 0.25f)
        scale = 0.25f;

    int32_t width  = (int32_t)((float)current_screen_width * scale + 0.5f);
    int32_t height = (int32_t)((float)current_screen_height * scale + 0.5f);

    if (width < 1)
        width = 1;
    if (height < 1)
        height = 1;

    if (width > current_screen_width)
        width = current_screen_width;
    if (height > current_screen_height)
        height = current_screen_height;

    bool changed = (width != render_target_width_) || (height != render_target_height_);

    render_target_width_  = width;
    render_target_height_ = height;

    render_target_scale_x_ = (float)width / (float)current_screen_width;
    render_target_scale_y_ = (float)height / (float)current_screen_height;

    render_target_scaled_ = (width != current_screen_width) || (height != current_screen_height);

    return changed;
}

void RenderBackend::SoftInit(void)
{
    render_state->Disable(GL_BLEND);
    render_state->Disable(GL_LIGHTING);
    render_state->Disable(GL_COLOR_MATERIAL);
    render_state->Disable(GL_CULL_FACE);
    render_state->Disable(GL_DEPTH_TEST);
    render_state->Disable(GL_SCISSOR_TEST);
    render_state->Disable(GL_STENCIL_TEST);


    render_state->Disable(GL_POLYGON_SMOOTH);

    render_state->Enable(GL_NORMALIZE);

    render_state->DepthFunction(GL_LEQUAL);
    render_state->AlphaFunction(GL_GREATER, 0);

    render_state->FrontFace(GL_CW);
    render_state->CullFace(GL_BACK);
    render_state->Disable(GL_CULL_FACE);

    render_state->Hint(GL_FOG_HINT, GL_NICEST);
    render_state->Hint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);

    LockRenderUnits(false);
}

void RenderBackend::Init()
{
    SoftInit();

    AllocateDrawStructs();

    SetupMatrices2D(false);
}
