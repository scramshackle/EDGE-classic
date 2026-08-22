
#include "r_mirror.h"

#include "epi_doomdefs.h"
#include "r_backend.h"
#include "i_system.h"
#include "r_image.h"
#include "r_lightgrid.h"
#include "r_state.h"
#include "r_render.h"
#include "r_sky.h"
#include "r_units.h"

extern bool solid_mode;

MirrorViewState mirror_view;

void ResetMirrorView(void)
{
    mirror_view.mirror         = nullptr;
    mirror_view.depth          = 0;
    mirror_view.reflective     = false;
    mirror_view.xy_scale       = 1.0f;
    mirror_view.z_scale        = 1.0f;
    mirror_view.sprite_right   = {{view_sine, -view_cosine}};
    mirror_view.sprite_forward = {{view_cosine, view_sine}};
    mirror_view.view_position  = {{view_x, view_y, view_z}};
    mirror_view.view_plane     = view_forward;
}

void InstallMirrorNearPlane(const DrawMirror *mir)
{
    if (!mir)
    {
        render_backend->SetObliqueNearPlane(false, HMM_V4(0.0f, 0.0f, 0.0f, 0.0f));
        return;
    }

    render_backend->SetObliqueNearPlane(true, mir->near_plane);
}

static void DrawMirrorAperturePolygon(const DrawMirror *mir, bool push_to_far_plane)
{
    Line *ld = mir->seg->linedef;

    float x1 = mir->seg->vertex_1->X;
    float y1 = mir->seg->vertex_1->Y;
    float z1 = ld->front_sector->interpolated_floor_height;

    float x2 = mir->seg->vertex_2->X;
    float y2 = mir->seg->vertex_2->Y;
    float z2 = ld->front_sector->interpolated_ceiling_height;

    RendererVertex quad[4];
    EPI_CLEAR_MEMORY(quad, RendererVertex, 4);

    quad[0].position = {{x1, y1, z1}};
    quad[1].position = {{x1, y1, z2}};
    quad[2].position = {{x2, y2, z2}};
    quad[3].position = {{x2, y2, z1}};

    if (push_to_far_plane)
    {
        float limit = renderer_far_clip.f_ * 0.9f;

        for (int32_t v = 0; v < 4; v++)
        {
            HMM_Vec3 ray = HMM_SubV3(quad[v].position, mirror_view.view_position);

            float distance = HMM_LenV3(ray);

            if (distance > 0.001f && distance < limit)
                quad[v].position = HMM_AddV3(mirror_view.view_position, HMM_MulV3F(ray, limit / distance));
        }
    }

    for (int32_t v = 0; v < 4; v++)
        quad[v].rgba = kRGBABlack;

    render_state->SetPipeline(0);
    render_state->SetVertexArrays(quad, 4);
    render_state->DrawVertexArray(GL_QUADS, 0, 4);
}

static void BeginApertureState(void)
{
    render_state->ColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    render_state->Disable(GL_BLEND);
    render_state->Disable(GL_CULL_FACE);
    render_state->Disable(GL_ALPHA_TEST);
    render_state->Enable(GL_DEPTH_TEST);
    render_state->Enable(GL_STENCIL_TEST);

    render_state->ActiveTexture(GL_TEXTURE1);
    render_state->Disable(GL_TEXTURE_2D);
    render_state->ActiveTexture(GL_TEXTURE0);
    render_state->Disable(GL_TEXTURE_2D);
}

static void FinishApertureState(void)
{
    render_state->ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    render_state->DepthFunction(GL_LEQUAL);
    render_state->DepthMask(true);
    render_state->StencilWriteMask(0);
}

static void EnterMirrorAperture(const DrawMirror *mir, int32_t depth)
{
    BeginApertureState();

    render_state->StencilWriteMask(0xFF);
    render_state->StencilFunction(GL_EQUAL, depth - 1, 0xFF);
    render_state->StencilOperation(GL_KEEP, GL_KEEP, GL_INCR);
    render_state->DepthFunction(GL_LEQUAL);
    render_state->DepthMask(false);

    DrawMirrorAperturePolygon(mir, false);

    render_state->StencilWriteMask(0);
    render_state->StencilFunction(GL_EQUAL, depth, 0xFF);
    render_state->StencilOperation(GL_KEEP, GL_KEEP, GL_KEEP);
    render_state->DepthFunction(GL_ALWAYS);
    render_state->DepthMask(true);

    DrawMirrorAperturePolygon(mir, true);

    FinishApertureState();
}

static void LeaveMirrorAperture(const DrawMirror *mir, int32_t depth)
{
    BeginApertureState();

    render_state->StencilWriteMask(0xFF);
    render_state->StencilFunction(GL_EQUAL, depth, 0xFF);
    render_state->StencilOperation(GL_KEEP, GL_KEEP, GL_DECR);
    render_state->DepthFunction(GL_ALWAYS);
    render_state->DepthMask(true);

    DrawMirrorAperturePolygon(mir, false);

    FinishApertureState();

    if (depth > 1)
    {
        render_state->StencilFunction(GL_EQUAL, depth - 1, 0xFF);
        render_state->StencilOperation(GL_KEEP, GL_KEEP, GL_KEEP);
    }
    else
    {
        render_state->Disable(GL_STENCIL_TEST);
        render_state->StencilWriteMask(0xFF);
    }
}

static void DrawMirrorPolygon(DrawMirror *mir)
{
    float alpha = 0.15 + 0.10 * mirror_view.depth;

    Line *ld = mir->seg->linedef;
    EPI_ASSERT(ld);
    RGBAColor unit_col;

    if (ld->special)
    {
        uint8_t col_r = epi::GetRGBARed(ld->special->fx_color_);
        uint8_t col_g = epi::GetRGBAGreen(ld->special->fx_color_);
        uint8_t col_b = epi::GetRGBABlue(ld->special->fx_color_);

        // looks better with reduced color in multiple reflections
        float reduce = 1.0f / (1 + 1.5 * mirror_view.depth);

        unit_col = epi::MakeRGBA((uint8_t)(reduce * col_r), (uint8_t)(reduce * col_g), (uint8_t)(reduce * col_b),
                                 (uint8_t)(alpha * 255.0f));
    }
    else
        unit_col = epi::MakeRGBA(255, 0, 0, (uint8_t)(alpha * 255.0f));

    float x1 = mir->seg->vertex_1->X;
    float y1 = mir->seg->vertex_1->Y;
    float z1 = ld->front_sector->interpolated_floor_height;

    float x2 = mir->seg->vertex_2->X;
    float y2 = mir->seg->vertex_2->Y;
    float z2 = ld->front_sector->interpolated_ceiling_height;

    RendererVertex *glvert = BeginRenderUnit(GL_QUADS, 4, GL_MODULATE, 0, (GLuint)kTextureEnvironmentDisable, 0, 0,
                                             alpha < 0.99f ? kBlendingAlpha : kBlendingNone);

    glvert->rgba       = unit_col;
    glvert++->position = {{x1, y1, z1}};
    glvert->rgba       = unit_col;
    glvert++->position = {{x1, y1, z2}};
    glvert->rgba       = unit_col;
    glvert++->position = {{x2, y2, z2}};
    glvert->rgba       = unit_col;
    glvert->position   = {{x2, y2, z1}};

    EndRenderUnit(4);
}

static void DrawPortalPolygon(DrawMirror *mir)
{
    Line *ld = mir->seg->linedef;
    EPI_ASSERT(ld);

    const MapSurface *surf = &mir->seg->sidedef->middle;

    if (!surf->image || !ld->special || !(ld->special->portal_effect_ & kPortalEffectTypeStandard))
    {
        DrawMirrorPolygon(mir);
        return;
    }

    // set texture
    GLuint tex_id = ImageCache(surf->image);

    // set colour & alpha
    float alpha = ld->special->translucency_ * surf->translucency;

    RGBAColor unit_col = ld->special->fx_color_;
    epi::SetRGBAAlpha(unit_col, alpha);

    // get polygon coordinates
    float x1 = mir->seg->vertex_1->X;
    float y1 = mir->seg->vertex_1->Y;
    float z1 = ld->front_sector->interpolated_floor_height;

    float x2 = mir->seg->vertex_2->X;
    float y2 = mir->seg->vertex_2->Y;
    float z2 = ld->front_sector->interpolated_ceiling_height;

    // get texture coordinates
    float total_w = surf->image->ScaledWidth();
    float total_h = surf->image->ScaledHeight();

    float tx1 = mir->seg->offset;
    float tx2 = tx1 + mir->seg->length;

    float ty1 = 0;
    float ty2 = (z2 - z1);

    tx1 = tx1 * surf->x_matrix.X / total_w;
    tx2 = tx2 * surf->x_matrix.X / total_w;

    ty1 = ty1 * surf->y_matrix.Y / total_h;
    ty2 = ty2 * surf->y_matrix.Y / total_h;

    RendererVertex *glvert = BeginRenderUnit(GL_QUADS, 4, GL_MODULATE, tex_id, (GLuint)kTextureEnvironmentDisable, 0,
                                             0, alpha < 0.99f ? kBlendingAlpha : kBlendingNone);

    glvert->rgba                     = unit_col;
    glvert->position                 = {{x1, y1, z1}};
    glvert++->texture_coordinates[0] = {{tx1, ty1}};
    glvert->rgba                     = unit_col;
    glvert->position                 = {{x1, y1, z2}};
    glvert++->texture_coordinates[0] = {{tx1, ty2}};
    glvert->rgba                     = unit_col;
    glvert->position                 = {{x2, y2, z2}};
    glvert++->texture_coordinates[0] = {{tx2, ty2}};
    glvert->rgba                     = unit_col;
    glvert->position                 = {{x2, y2, z1}};
    glvert->texture_coordinates[0]   = {{tx2, ty1}};

    EndRenderUnit(4);
}

void RenderMirror(DrawMirror *mir)
{
    // mark the line on the automap
    if (!(mir->seg->linedef->flags & kLineFlagMapped))
        newly_seen_lines.emplace(mir->seg->linedef);

    FinishUnitBatch();

    MirrorViewState saved = mirror_view;

    int32_t depth = saved.depth + 1;

    EnterMirrorAperture(mir, depth);

    mirror_view.mirror         = mir;
    mirror_view.depth          = depth;
    mirror_view.reflective     = mir->reflective;
    mirror_view.xy_scale       = mir->xy_scale;
    mirror_view.z_scale        = mir->z_scale;
    mirror_view.sprite_right   = mir->sprite_right;
    mirror_view.sprite_forward = mir->sprite_forward;
    mirror_view.view_position  = mir->view_position;
    mirror_view.view_plane     = mir->view_plane;

    InstallMirrorNearPlane(mir);

    render_state->Enable(GL_STENCIL_TEST);
    render_state->StencilFunction(GL_EQUAL, depth, 0xFF);
    render_state->StencilOperation(GL_KEEP, GL_KEEP, GL_KEEP);
    render_state->StencilWriteMask(0);

    render_backend->PushModelMatrix(mir->local_matrix);

    BuildLightGrid();

    {
        uint64_t upload_mark = GetMicroseconds();
        render_backend->UploadLightGrid(CurrentLightGrid());
        ec_frame_stats.light_grid_upload_us += GetMicroseconds() - upload_mark;
    }

    FinishSkyForMirror(mir);

    RenderSubList(mir->draw_subsectors, mir->draw_things, true);

    render_backend->PopModelMatrix();

    BuildLightGrid();

    {
        uint64_t upload_mark = GetMicroseconds();
        render_backend->UploadLightGrid(CurrentLightGrid());
        ec_frame_stats.light_grid_upload_us += GetMicroseconds() - upload_mark;
    }

    mirror_view = saved;

    InstallMirrorNearPlane(mirror_view.mirror);

    LeaveMirrorAperture(mir, depth);

    StartUnitBatch(false);

    if (mir->is_portal)
        DrawPortalPolygon(mir);
    else
        DrawMirrorPolygon(mir);

    FinishUnitBatch();

    solid_mode = true;
    StartUnitBatch(solid_mode);
}