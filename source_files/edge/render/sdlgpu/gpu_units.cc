#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "con_var.h"
#include "epi.h"
#include "epi_math.h"
#include "gpu_images.h"
#include "gpu_immediate.h"
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

    float line_width;

    RGBAColor fog_color   = kRGBANoValue;
    float     fog_density = 0;

    bool        sky_pass_enabled = false;
    bool        light_depth_enabled = false;

    uint32_t static_buffer = 0;
    int      static_first  = 0;
    SkyPassInfo sky_pass;

    bool            scissor_enabled = false;
    RendererScissor scissor;

    bool              light_pass_enabled = false;
    RendererLightPass light_pass;
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

uint32_t CreateStaticVertexBuffer(const RendererVertex *vertices, int count)
{
    return gpu_immediate.CreateStaticBuffer(vertices, count);
}

void DeleteStaticVertexBuffer(uint32_t handle)
{
    gpu_immediate.DeleteStaticBuffer(handle);
}

void AddStaticRenderUnit(uint32_t handle, GLuint shape, int first, int count, GLuint env1, GLuint tex1, GLuint env2,
                         GLuint tex2, int pass, BlendingMode blending, RGBAColor fog_color, float fog_density)
{
    if (!handle || count <= 0)
        return;

    if (render_backend->RenderUnitsLocked())
        FatalError("AddStaticRenderUnit - Render units are locked");

    if (current_render_unit >= kMaximumLocalUnits)
        RenderCurrentUnits();

    RendererUnit *unit = local_units + current_render_unit;

    if (env1 == kTextureEnvironmentDisable)
        tex1 = 0;
    if (env2 == kTextureEnvironmentDisable)
        tex2 = 0;

    unit->shape               = shape;
    unit->environment_mode[0] = env1;
    unit->environment_mode[1] = env2;
    unit->texture[0]          = tex1;
    unit->texture[1]          = tex2;
    unit->pass                = pass;
    unit->blending            = blending;
    unit->first               = current_render_vert;
    unit->count               = count;
    unit->fog_color           = fog_color;
    unit->fog_density         = fog_density;
    unit->sky_pass_enabled    = false;
    unit->light_depth_enabled = true;
    unit->scissor_enabled     = false;
    unit->light_pass_enabled  = false;
    unit->static_buffer       = handle;
    unit->static_first        = first;

    current_render_unit++;
}

RendererVertex *BeginRenderUnit(GLuint shape, int max_vert, GLuint env1, GLuint tex1, GLuint env2, GLuint tex2,
                                int pass, BlendingMode blending, RGBAColor fog_color, float fog_density,
                                const SkyPassInfo *sky_pass, const RendererScissor *scissor,
                                const RendererLightPass *light_pass, bool light_depth)
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

    unit->pass       = pass;
    unit->blending   = blending;
    unit->first      = current_render_vert;
    unit->line_width = (shape == GL_LINES) ? render_state->GetLineWidth() : 1.0f;

    unit->sky_pass_enabled    = (sky_pass != nullptr);
    unit->light_depth_enabled = light_depth;
    unit->static_buffer       = 0;
    unit->static_first        = 0;

    if (sky_pass)
        unit->sky_pass = *sky_pass;

    unit->fog_color   = fog_color;
    unit->fog_density = fog_density;

    unit->scissor_enabled = (scissor != nullptr);

    if (scissor)
        unit->scissor = *scissor;

    unit->light_pass_enabled = (light_pass != nullptr);

    if (light_pass)
        unit->light_pass = *light_pass;

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

static void BindUnitTextures(const RendererUnit *unit)
{
    if (!unit->texture[0] || unit->environment_mode[0] == kTextureEnvironmentDisable)
    {
        gpu_immediate.DisableTexture();
        return;
    }

    const GpuImage *image0 = GetGpuImage(unit->texture[0]);

    if (!unit->texture[1] || unit->environment_mode[1] == kTextureEnvironmentDisable)
    {
        gpu_immediate.SetTexture(image0 ? image0->texture : nullptr, image0 ? image0->sampler : nullptr);
        return;
    }

    const GpuImage *image1 = GetGpuImage(unit->texture[1]);

    gpu_immediate.SetMultiTexture(image0 ? image0->texture : nullptr, image0 ? image0->sampler : nullptr,
                                  image1 ? image1->texture : nullptr, image1 ? image1->sampler : nullptr);
}

static void BindLightPassTextures(const RendererUnit *unit)
{
    const GpuImage *surface_image = GetGpuImage(unit->texture[0]);
    const GpuImage *light_image   = GetGpuImage(unit->texture[1]);

    gpu_immediate.SetMultiTexture(
        surface_image ? surface_image->texture : nullptr, surface_image ? surface_image->sampler : nullptr,
        light_image ? light_image->texture : nullptr, light_image ? light_image->sampler : nullptr);
}

static void DrawLightPassUnit(const RendererUnit *unit)
{
    const RendererLightPass &light_pass = unit->light_pass;

    BindLightPassTextures(unit);

    GpuLightVertexParameters vertex_parameters;
    EPI_CLEAR_MEMORY(&vertex_parameters, GpuLightVertexParameters, 1);

    vertex_parameters.mvp = render_backend->WorldViewProjection();

    GpuLightFragmentParameters fragment_parameters;
    EPI_CLEAR_MEMORY(&fragment_parameters, GpuLightFragmentParameters, 1);

    for (int e = 0; e < 4; e++)
        fragment_parameters.surface_normal[e] = light_pass.surface_normal[e];

    for (int i = 0; i < kMaximumLightsPerPass; i++)
    {
        for (int e = 0; e < 4; e++)
        {
            fragment_parameters.light_position_radius[i][e] = light_pass.position_radius[i * 4 + e];
            fragment_parameters.light_color[i][e]           = light_pass.color[i * 4 + e];
        }
    }

    fragment_parameters.light_count       = light_pass.count;
    fragment_parameters.normal_horizontal = light_pass.normal_is_horizontal ? 1.0f : 0.0f;
    fragment_parameters.surface_mode      = light_pass.surface_mode;
    fragment_parameters.alpha             = light_pass.alpha;
    fragment_parameters.alpha_test        = light_pass.alpha_test;

    gpu_immediate.RecordLightDraw(unit->shape, local_verts + unit->first, unit->count, vertex_parameters,
                                  fragment_parameters);
}

static void DrawLineUnit(const RendererUnit *unit)
{
    gpu_immediate.DisableTexture();
    gpu_immediate.SetLineMode(true);

    const RendererVertex *source = local_verts + unit->first;

    int32_t line_total = unit->count / 2;

    if (line_total <= 0)
    {
        gpu_immediate.SetLineMode(false);
        return;
    }

    RendererVertex *destination = gpu_immediate.ReserveVertices(line_total * 4);

    HMM_Vec2 aa_radius = {{2.0f, 2.0f}};

    float line_width       = HMM_MAX(1.0f, unit->line_width) + aa_radius.X;
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

        out[0].position                   = {{a1.X, a1.Y, source_v0->position.Z}};
        out[0].texture_coordinates[0]     = {{line_width, -factor * line_length}};

        out[1].position                   = {{a0.X, a0.Y, source_v0->position.Z}};
        out[1].texture_coordinates[0]     = {{-line_width, -factor * line_length}};

        out[2].position                   = {{b0.X, b0.Y, source_v1->position.Z}};
        out[2].texture_coordinates[0]     = {{-line_width, factor * line_length}};

        out[3].position                   = {{b1.X, b1.Y, source_v1->position.Z}};
        out[3].texture_coordinates[0]     = {{line_width, factor * line_length}};
    }

    gpu_immediate.RecordDraw(GL_QUADS, line_total * 4);

    gpu_immediate.SetLineMode(false);
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

    bool    ambient_scissor_enabled = render_state->ScissorTestEnabled();
    GLint   ambient_scissor_x       = 0;
    GLint   ambient_scissor_y       = 0;
    GLsizei ambient_scissor_width   = 0;
    GLsizei ambient_scissor_height  = 0;

    render_state->GetScissor(ambient_scissor_x, ambient_scissor_y, ambient_scissor_width, ambient_scissor_height);

    bool scissor_overridden = false;

    for (int j = 0; j < current_render_unit; j++)
    {
        RendererUnit *unit = local_unit_map[j];

        EPI_ASSERT(unit->count > 0);

        if (unit->scissor_enabled)
        {
            render_state->Enable(GL_SCISSOR_TEST);
            render_state->Scissor(unit->scissor.x, unit->scissor.y, unit->scissor.width, unit->scissor.height);

            scissor_overridden = true;
        }
        else if (scissor_overridden)
        {
            render_state->Enable(GL_SCISSOR_TEST, ambient_scissor_enabled);
            render_state->Scissor(ambient_scissor_x, ambient_scissor_y, ambient_scissor_width, ambient_scissor_height);

            scissor_overridden = false;
        }

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
            render_state->AlphaFunction(GL_GREATER, 0.01);
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

        render_state->SetPipeline(0);

        if (unit->light_pass_enabled)
        {
            DrawLightPassUnit(unit);
            continue;
        }

        if (!unit->static_buffer && (!unit->texture[0] || unit->environment_mode[0] == kTextureEnvironmentDisable) &&
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
        }

        gpu_immediate.SetSkipRGB(unit->environment_mode[0] == (GLuint)kTextureEnvironmentSkipRGB);

        gpu_immediate.SetSkyPass(unit->sky_pass_enabled ? &unit->sky_pass : nullptr);
        gpu_immediate.SetLightDepth(unit->light_depth_enabled);

        if (unit->shape == GL_LINES)
        {
            DrawLineUnit(unit);
            continue;
        }

        BindUnitTextures(unit);

        if (unit->static_buffer)
            gpu_immediate.DrawStatic(unit->static_buffer, unit->static_first, unit->count);
        else
            gpu_immediate.Draw(unit->shape, local_verts + unit->first, unit->count);
    }

    if (scissor_overridden)
    {
        render_state->Enable(GL_SCISSOR_TEST, ambient_scissor_enabled);
        render_state->Scissor(ambient_scissor_x, ambient_scissor_y, ambient_scissor_width, ambient_scissor_height);
    }

    gpu_immediate.SetSkipRGB(false);
    gpu_immediate.SetSkyPass(nullptr);

    current_render_vert = current_render_unit = 0;
}
