#include "r_units.h"

#include <math.h>

#include "epi.h"
#include "epi_math.h"
#include "r_backend.h"
#include "r_misc.h"

static constexpr float kOitBoundsMinimumW = 0.0001f;

static bool      oit_bounds_tracking = false;
static bool      oit_bounds_full     = false;
static bool      oit_bounds_empty    = true;
static float     oit_bounds_low_x    = 0.0f;
static float     oit_bounds_low_y    = 0.0f;
static float     oit_bounds_high_x   = 0.0f;
static float     oit_bounds_high_y   = 0.0f;
static epi::Mat4 oit_bounds_view_projection;

void BeginOitBounds(bool enabled)
{
    oit_bounds_tracking = enabled;
    oit_bounds_full     = !enabled;
    oit_bounds_empty    = true;

    if (enabled)
        oit_bounds_view_projection = render_backend->WorldViewProjection();
}

void EndOitBounds(void)
{
    oit_bounds_tracking = false;

    render_backend->ClearOitBounds();
}

bool OitBoundsTracking(void)
{
    return oit_bounds_tracking && !oit_bounds_full;
}

bool UnitAccumulatesOit(BlendingMode blending, int pass)
{
    return (blending & kBlendingAlpha) && !(blending & kBlendingAdd) && pass == 0;
}

void OitBoundsAddFull(void)
{
    oit_bounds_full = true;
}

static void OitBoundsAddPoint(float x, float y, float z)
{
    epi::Vec4 clip = epi::TransformVector(oit_bounds_view_projection, epi::Vec4{x, y, z, 1.0f});

    if (clip.w <= kOitBoundsMinimumW)
    {
        oit_bounds_full = true;
        return;
    }

    float screen_x = (float)view_window_x + ((clip.x / clip.w) * 0.5f + 0.5f) * (float)view_window_width;
    float screen_y = (float)view_window_y + ((clip.y / clip.w) * 0.5f + 0.5f) * (float)view_window_height;

    if (oit_bounds_empty)
    {
        oit_bounds_low_x = oit_bounds_high_x = screen_x;
        oit_bounds_low_y = oit_bounds_high_y = screen_y;
        oit_bounds_empty                     = false;
        return;
    }

    oit_bounds_low_x  = epi::Min(oit_bounds_low_x, screen_x);
    oit_bounds_low_y  = epi::Min(oit_bounds_low_y, screen_y);
    oit_bounds_high_x = epi::Max(oit_bounds_high_x, screen_x);
    oit_bounds_high_y = epi::Max(oit_bounds_high_y, screen_y);
}

void OitBoundsAddVertices(std::span<const RendererVertex> vertices)
{
    if (!OitBoundsTracking())
        return;

    for (size_t i = 0; i < vertices.size() && !oit_bounds_full; i++)
        OitBoundsAddPoint(vertices[i].position.x, vertices[i].position.y, vertices[i].position.z);
}

void OitBoundsAddBox(const float low[3], const float high[3])
{
    if (!OitBoundsTracking())
        return;

    for (int corner = 0; corner < 8 && !oit_bounds_full; corner++)
    {
        OitBoundsAddPoint((corner & 1) ? high[0] : low[0], (corner & 2) ? high[1] : low[1],
                          (corner & 4) ? high[2] : low[2]);
    }
}

void OitBoundsAddSprites(std::span<const SpriteInstance> instances, const epi::Vec4 &sprite_view)
{
    if (!OitBoundsTracking())
        return;

    float right_length   = sqrtf(sprite_view.x * sprite_view.x + sprite_view.y * sprite_view.y);
    float forward_length = sqrtf(sprite_view.z * sprite_view.z + sprite_view.w * sprite_view.w);

    for (size_t i = 0; i < instances.size() && !oit_bounds_full; i++)
    {
        const SpriteInstance &instance = instances[i];

        float along  = epi::Max(fabsf(instance.extent[0]), fabsf(instance.extent[1]));
        float radius = along * right_length + fabsf(instance.extent[2]) * 0.5f * forward_length;

        float low[3]  = {instance.origin[0] - radius, instance.origin[1] - radius,
                         epi::Min(instance.origin[2], instance.origin[3])};
        float high[3] = {instance.origin[0] + radius, instance.origin[1] + radius,
                         epi::Max(instance.origin[2], instance.origin[3])};

        OitBoundsAddBox(low, high);
    }
}

void PublishOitBounds(void)
{
    if (oit_bounds_full)
    {
        render_backend->ClearOitBounds();
        return;
    }

    if (oit_bounds_empty)
    {
        render_backend->SetOitBounds(view_window_x, view_window_y, 0, 0);
        return;
    }

    int32_t low_x  = epi::Max((int32_t)floorf(oit_bounds_low_x) - 1, (int32_t)view_window_x);
    int32_t low_y  = epi::Max((int32_t)floorf(oit_bounds_low_y) - 1, (int32_t)view_window_y);
    int32_t high_x = epi::Min((int32_t)ceilf(oit_bounds_high_x) + 1, (int32_t)(view_window_x + view_window_width));
    int32_t high_y = epi::Min((int32_t)ceilf(oit_bounds_high_y) + 1, (int32_t)(view_window_y + view_window_height));

    render_backend->SetOitBounds(low_x, low_y, epi::Max(high_x - low_x, 0), epi::Max(high_y - low_y, 0));
}
