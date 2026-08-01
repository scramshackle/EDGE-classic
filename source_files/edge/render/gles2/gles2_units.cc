#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "con_var.h"
#include "epi.h"
#include "epi_math.h"
#include "gles2_immediate.h"
#include "i_defs_gl.h"
#include "r_backend.h"
#include "r_gldefs.h"
#include "r_misc.h"
#include "r_state.h"
#include "r_units.h"

static constexpr uint16_t kMaximumLocalUnits = 1024;

extern ConsoleVariable draw_culling;
extern ConsoleVariable cull_fog_color;

struct RendererUnit
{
    GLuint shape;

    GLuint environment_mode[2];

    GLuint texture[2];

    int pass;

    BlendingMode blending;

    int first, count;

    RGBAColor fog_color   = kRGBANoValue;
    float     fog_density = 0;
};

static RendererVertex local_verts[kMaximumLocalVertices];
static RendererUnit   local_units[kMaximumLocalUnits];

static std::vector<RendererUnit *> local_unit_map;

static int current_render_vert;
static int current_render_unit;

static bool batch_sort;

RGBAColor culling_fog_color;

void StartUnitBatch(bool sort_em)
{
    if (render_backend->RenderUnitsLocked())
    {
        FatalError("StartUnitBatch - Render units are locked");
    }

    current_render_vert = current_render_unit = 0;

    batch_sort = sort_em;

    local_unit_map.resize(kMaximumLocalUnits);
}

void FinishUnitBatch(void)
{
    if (render_backend->RenderUnitsLocked())
    {
        FatalError("FinishUnitBatch - Render units are locked");
    }

    RenderCurrentUnits();
}

RendererVertex *BeginRenderUnit(GLuint shape, int max_vert, GLuint env1, GLuint tex1, GLuint env2, GLuint tex2,
                                int pass, BlendingMode blending, RGBAColor fog_color, float fog_density)
{
    if (render_backend->RenderUnitsLocked())
    {
        FatalError("BeginRenderUnit - Render units are locked");
    }

    RendererUnit *unit;

    EPI_ASSERT(max_vert > 0);
    EPI_ASSERT(pass >= 0);

    EPI_ASSERT((blending & (kBlendingCullBack | kBlendingCullFront)) != (kBlendingCullBack | kBlendingCullFront));

    if (current_render_vert + max_vert >= kMaximumLocalVertices || current_render_unit >= kMaximumLocalUnits)
    {
        RenderCurrentUnits();
    }

    unit = local_units + current_render_unit;

    if (env1 == kTextureEnvironmentDisable)
        tex1 = 0;
    if (env2 == kTextureEnvironmentDisable)
        tex2 = 0;

    unit->shape               = shape;
    unit->environment_mode[0] = env1;
    unit->environment_mode[1] = env2;
    unit->texture[0]          = tex1;
    unit->texture[1]          = tex2;

    unit->pass     = pass;
    unit->blending = blending;
    unit->first    = current_render_vert;

    unit->fog_color   = fog_color;
    unit->fog_density = fog_density;

    return local_verts + current_render_vert;
}

void EndRenderUnit(int actual_vert)
{
    if (render_backend->RenderUnitsLocked())
    {
        FatalError("EndRenderUnit - Render units are locked");
    }

    RendererUnit *unit;

    EPI_ASSERT(actual_vert >= 0);

    if (actual_vert == 0)
        return;

    unit = local_units + current_render_unit;

    unit->count = actual_vert;

    current_render_vert += actual_vert;
    current_render_unit++;

    EPI_ASSERT(current_render_vert <= kMaximumLocalVertices);
    EPI_ASSERT(current_render_unit <= kMaximumLocalUnits);
}

struct Compare_Unit_pred
{
    inline bool operator()(const RendererUnit *A, const RendererUnit *B) const
    {
        if (A->pass != B->pass)
            return A->pass < B->pass;

        if (A->texture[0] != B->texture[0])
            return A->texture[0] < B->texture[0];

        if (A->texture[1] != B->texture[1])
            return A->texture[1] < B->texture[1];

        if (A->environment_mode[0] != B->environment_mode[0])
            return A->environment_mode[0] < B->environment_mode[0];

        if (A->environment_mode[1] != B->environment_mode[1])
            return A->environment_mode[1] < B->environment_mode[1];

        return A->blending < B->blending;
    }
};

static void ApplyUnitEnvironment(int32_t texture_unit, GLuint environment)
{
    render_state->ActiveTexture(GL_TEXTURE0 + texture_unit);

    if (environment == (GLuint)kTextureEnvironmentSkipRGB)
    {
        render_state->TextureEnvironmentMode(GL_COMBINE);
        render_state->TextureEnvironmentCombineRGB(GL_REPLACE);
        render_state->TextureEnvironmentSource0RGB(GL_PREVIOUS);
    }
    else
    {
        render_state->TextureEnvironmentMode(GL_MODULATE);
        render_state->TextureEnvironmentCombineRGB(GL_MODULATE);
        render_state->TextureEnvironmentSource0RGB(GL_TEXTURE);
    }
}

static void BindUnitTextures(const RendererUnit *unit)
{
    for (int32_t t = 1; t >= 0; t--)
    {
        bool active = unit->texture[t] != 0 && unit->environment_mode[t] != kTextureEnvironmentDisable;

        render_state->ActiveTexture(GL_TEXTURE0 + t);

        if (active)
        {
            render_state->Enable(GL_TEXTURE_2D);
            render_state->BindTexture(unit->texture[t]);
        }
        else
        {
            render_state->Disable(GL_TEXTURE_2D);
            render_state->BindTexture(0);
        }

        ApplyUnitEnvironment(t, unit->environment_mode[t]);
    }

    render_state->ActiveTexture(GL_TEXTURE0);
}

static void ApplyUnitClamping(const RendererUnit *unit, GLint &old_clamp_s, GLint &old_clamp_t)
{
    old_clamp_s = kDummyClamp;
    old_clamp_t = kDummyClamp;

    if (unit->texture[0] == 0)
        return;

    render_state->ActiveTexture(GL_TEXTURE0);

    if (unit->blending & kBlendingRepeatX)
    {
        auto existing = texture_clamp_s.find(unit->texture[0]);

        if (existing == texture_clamp_s.end())
        {
            render_state->TextureWrapS(GL_REPEAT);
        }
        else if (existing->second != GL_REPEAT)
        {
            old_clamp_s = existing->second;
            render_state->TextureWrapS(GL_REPEAT);
        }
    }

    if (unit->blending & (kBlendingClampY | kBlendingRepeatY))
    {
        GLint wanted = (unit->blending & kBlendingClampY) ? GL_CLAMP_TO_EDGE : GL_REPEAT;

        auto existing = texture_clamp_t.find(unit->texture[0]);

        if (existing == texture_clamp_t.end())
        {
            render_state->TextureWrapT(wanted);
        }
        else if (existing->second != wanted)
        {
            old_clamp_t = existing->second;
            render_state->TextureWrapT(wanted);
        }
    }
}

static void DrawLineUnit(const RendererUnit *unit)
{
    render_state->ActiveTexture(GL_TEXTURE1);
    render_state->Disable(GL_TEXTURE_2D);
    render_state->ActiveTexture(GL_TEXTURE0);
    render_state->Disable(GL_TEXTURE_2D);

    gles2_immediate.SetLineMode(true);

    const RendererVertex *source = local_verts + unit->first;

    int32_t line_total = unit->count / 2;

    if (line_total <= 0)
    {
        gles2_immediate.SetLineMode(false);
        return;
    }

    RendererVertex *destination = gles2_immediate.ReserveVertices(line_total * 4);

    HMM_Vec2 aa_radius = {{2.0f, 2.0f}};

    float line_width       = HMM_MAX(1.0f, render_state->GetLineWidth()) + aa_radius.X;
    float extension_length = aa_radius.Y;

    for (int32_t i = 0; i < line_total; i++)
    {
        const RendererVertex *source_v0 = source + i * 2;
        const RendererVertex *source_v1 = source_v0 + 1;

        HMM_Vec2 v0 = {{source_v0->position.X, source_v0->position.Y}};
        HMM_Vec2 v1 = {{source_v1->position.X, source_v1->position.Y}};

        HMM_Vec2 line_vector = HMM_SubV2(v1, v0);
        float    line_length = HMM_LenV2(line_vector) + 2.0f * extension_length;
        HMM_Vec2 direction   = HMM_NormV2(line_vector);
        HMM_Vec2 normal      = {{-direction.Y * line_width * 0.5f, direction.X * line_width * 0.5f}};

        HMM_Vec2 extension = HMM_MulV2({{extension_length, extension_length}}, direction);

        HMM_Vec2 a1 = {{v0.X - normal.X - extension.X, v0.Y - normal.Y - extension.Y}};
        HMM_Vec2 a0 = {{v0.X + normal.X - extension.X, v0.Y + normal.Y - extension.Y}};

        HMM_Vec2 b1 = {{v1.X - normal.X + extension.X, v1.Y - normal.Y + extension.Y}};
        HMM_Vec2 b0 = {{v1.X + normal.X + extension.X, v1.Y + normal.Y + extension.Y}};

        float factor = 0.5f;

        RendererVertex *out = destination + i * 4;

        for (int32_t k = 0; k < 4; k++)
        {
            out[k].rgba                     = source_v0->rgba;
            out[k].texture_coordinates[1].X = line_width;
            out[k].texture_coordinates[1].Y = factor * line_length;
        }

        out[0].position               = {{a1.X, a1.Y, source_v0->position.Z}};
        out[0].texture_coordinates[0] = {{line_width, -factor * line_length}};

        out[1].position               = {{a0.X, a0.Y, source_v0->position.Z}};
        out[1].texture_coordinates[0] = {{-line_width, -factor * line_length}};

        out[2].position               = {{b0.X, b0.Y, source_v1->position.Z}};
        out[2].texture_coordinates[0] = {{-line_width, factor * line_length}};

        out[3].position               = {{b1.X, b1.Y, source_v1->position.Z}};
        out[3].texture_coordinates[0] = {{line_width, -factor * line_length}};
    }

    render_state->SetPipeline(0);

    Gles2ApplyRenderState();

    gles2_immediate.RecordDraw(GL_QUADS, line_total * 4);

    gles2_immediate.SetLineMode(false);
}

void RenderCurrentUnits(void)
{
    if (render_backend->RenderUnitsLocked())
    {
        FatalError("RenderCurrentUnits - Render units are locked");
    }

    if (current_render_unit == 0)
        return;

    for (int i = 0; i < current_render_unit; i++)
        local_unit_map[i] = &local_units[i];

    if (batch_sort)
    {
        std::sort(local_unit_map.begin(), local_unit_map.begin() + current_render_unit, Compare_Unit_pred());
    }

    RenderLayer render_layer = render_backend->GetRenderLayer();

    ec_frame_stats.draw_render_units += current_render_unit;

    bool no_fog = (render_layer == kRenderLayerHUD) || (render_layer == kRenderLayerViewport);

    bool culling = draw_culling.d_ && !no_fog;

    if (culling)
    {
        RGBAColor fogColor;
        switch (cull_fog_color.d_)
        {
        case 0:
            fogColor = culling_fog_color;
            break;
        case 1:
            fogColor = kRGBASilver;
            break;
        case 2:
            fogColor = MakeRGBAConstant(0x404040FF);
            break;
        case 3:
            fogColor = kRGBABlack;
            break;
        default:
            fogColor = culling_fog_color;
            break;
        }

        render_backend->SetClearColor(fogColor);
        render_state->FogMode(GL_LINEAR);
        render_state->FogColor(fogColor);
        render_state->FogStart(renderer_far_clip.f_ - 750.0f);
        render_state->FogEnd(renderer_far_clip.f_ - 250.0f);
        render_state->Enable(GL_FOG);
    }
    else
        render_state->Disable(GL_FOG);

    render_state->SetVertexArrays(local_verts);

    gles2_immediate.UploadBatch(local_verts, current_render_vert);

    for (int j = 0; j < current_render_unit; j++)
    {
        RendererUnit *unit = local_unit_map[j];

        EPI_ASSERT(unit->count > 0);

        if (!culling && unit->fog_color != kRGBANoValue && !(unit->blending & kBlendingNoFog) && !no_fog)
        {
            float density = unit->fog_density;
            render_state->FogMode(GL_EXP);
            render_state->ClearColor(unit->fog_color);
            render_state->FogColor(unit->fog_color);
            render_state->FogDensity(std::log1p(density));
            if (!epi::AlmostEquals(density, 0.0f))
                render_state->Enable(GL_FOG);
            else
                render_state->Disable(GL_FOG);
        }
        else if (!culling || (unit->blending & kBlendingNoFog))
            render_state->Disable(GL_FOG);

        if (unit->blending & kBlendingAdd)
        {
            render_state->Enable(GL_BLEND);
            render_state->BlendFunction(GL_SRC_ALPHA, GL_ONE);
        }
        else if (unit->blending & kBlendingAlpha)
        {
            render_state->Enable(GL_BLEND);
            render_state->BlendFunction(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
        else if (unit->blending & kBlendingInvert)
        {
            render_state->Enable(GL_BLEND);
            render_state->BlendFunction(GL_ONE_MINUS_DST_COLOR, GL_ZERO);
        }
        else if (unit->blending & kBlendingNegativeGamma)
        {
            render_state->Enable(GL_BLEND);
            render_state->BlendFunction(GL_ZERO, GL_SRC_COLOR);
        }
        else if (unit->blending & kBlendingPositiveGamma)
        {
            render_state->Enable(GL_BLEND);
            render_state->BlendFunction(GL_DST_COLOR, GL_ONE);
        }
        else
            render_state->Disable(GL_BLEND);

        if (unit->blending & (kBlendingCullBack | kBlendingCullFront))
        {
            render_state->Enable(GL_CULL_FACE);
            render_state->CullFace((unit->blending & kBlendingCullFront) ? GL_FRONT : GL_BACK);
        }
        else
            render_state->Disable(GL_CULL_FACE);

        render_state->DepthMask((unit->blending & kBlendingNoZBuffer) ? false : true);

        if (unit->blending & kBlendingLess)
        {
            render_state->Enable(GL_ALPHA_TEST);
        }
        else if (unit->blending & kBlendingMasked)
        {
            render_state->Enable(GL_ALPHA_TEST);
            render_state->AlphaFunction(GL_GREATER, 0.01f);
        }
        else if (unit->blending & kBlendingGEqual)
        {
            render_state->Enable(GL_ALPHA_TEST);
            render_state->AlphaFunction(GL_GEQUAL, 1.0f - (epi::GetRGBAAlpha(local_verts[unit->first].rgba) / 255.0f));
        }
        else
            render_state->Disable(GL_ALPHA_TEST);

        if (unit->blending & kBlendingLess)
        {
            float a = epi::GetRGBAAlpha(local_verts[unit->first].rgba) / 255.0f;
            render_state->AlphaFunction(GL_GREATER, a * 0.66f);
        }

        if (draw_culling.d_ && !(unit->blending & kBlendingNoFog) &&
            (render_layer == kRenderLayerSolid || render_layer == kRenderLayerTransparent))
        {
            if (unit->pass > 0)
            {
                render_state->Disable(GL_FOG);
            }
            else
            {
                render_state->Enable(GL_FOG);
            }
        }

        render_state->PolygonOffset(0, -unit->pass);

        if ((!unit->texture[0] || unit->environment_mode[0] == kTextureEnvironmentDisable) &&
            (unit->texture[1] && unit->environment_mode[1] != kTextureEnvironmentDisable))
        {
            unit->texture[0]          = unit->texture[1];
            unit->environment_mode[0] = unit->environment_mode[1];

            unit->texture[1]          = 0;
            unit->environment_mode[1] = kTextureEnvironmentDisable;

            RendererVertex *v = local_verts + unit->first;

            for (int k = 0; k < unit->count; k++, v++)
            {
                v->texture_coordinates[0].X = v->texture_coordinates[1].X;
                v->texture_coordinates[0].Y = v->texture_coordinates[1].Y;
            }

            gles2_immediate.InvalidateBatch();
        }

        if (unit->shape == GL_LINES)
        {
            DrawLineUnit(unit);
            continue;
        }

        BindUnitTextures(unit);

        GLint old_clamp_s = kDummyClamp;
        GLint old_clamp_t = kDummyClamp;

        ApplyUnitClamping(unit, old_clamp_s, old_clamp_t);

        render_state->SetPipeline(0);

        render_state->DrawVertexArray(unit->shape, unit->first, unit->count);

        if (old_clamp_s != kDummyClamp)
        {
            render_state->ActiveTexture(GL_TEXTURE0);
            render_state->TextureWrapS(old_clamp_s);
        }

        if (old_clamp_t != kDummyClamp)
        {
            render_state->ActiveTexture(GL_TEXTURE0);
            render_state->TextureWrapT(old_clamp_t);
        }
    }

    for (int t = 1; t >= 0; t--)
    {
        render_state->ActiveTexture(GL_TEXTURE0 + t);
        render_state->Disable(GL_TEXTURE_2D);

        ApplyUnitEnvironment(t, GL_MODULATE);
    }

    render_state->ActiveTexture(GL_TEXTURE0);

    gles2_immediate.InvalidateBatch();

    current_render_vert = current_render_unit = 0;
}
