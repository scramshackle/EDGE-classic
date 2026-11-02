#include "r_translucent.h"

#include <math.h>

#include <algorithm>
#include <unordered_map>
#include <vector>

#include "epi.h"
#include "epi_math.h"
#include "r_backend.h"

static constexpr float kTranslucentSideEpsilon  = 0.25f;
static constexpr float kTranslucentPlaneEpsilon = 1.0f;
static constexpr float kTranslucentNearW        = 1.0f;

struct TranslucentItem
{
    epi::Vec3 normal;
    float     plane_distance;
    float     eye_side;
    epi::Vec3 center;
    float     radius;
    bool      planar;
    int       point_first;
    int       point_count;
    float     rect[4];
    float     distance;
    int       kind;
    int       payload;
    int       sequence;
    int       group;
};

static std::vector<TranslucentItem> translucent_items;
static std::vector<epi::Vec4>       translucent_clip;
static std::vector<epi::Vec3>       translucent_points;
static std::vector<int>             translucent_sweep;
struct TranslucentSweepEntry
{
    float high_x;
    float low_y;
    float high_y;
    int   item;
};

static std::vector<TranslucentSweepEntry> translucent_active;
static std::vector<int>                   translucent_rank;
static std::vector<int>                   translucent_by_rank;
static std::vector<int>                   translucent_edge_from;
static std::vector<int>                   translucent_edge_to;
static std::vector<int>                   translucent_edge_first;
static std::vector<int>                   translucent_edge_list;
static std::vector<int>                   translucent_in_degree;
static std::vector<bool>                  translucent_emitted;
static std::vector<int>                   translucent_order;
static std::vector<int>                   translucent_payloads;
static std::vector<int>                   translucent_ready;
static std::vector<int>                   translucent_fill;
static std::vector<int>                   translucent_wave;
static std::vector<int>                   translucent_wave_ends;

static std::unordered_map<uint64_t, int> translucent_group_index;
static std::vector<std::vector<int>>     translucent_group_ready;

static epi::Mat4 translucent_view_projection;
static epi::Vec3 translucent_eye;

static float Determinant3(float a, float b, float c, float d, float e, float f, float g, float h, float i)
{
    return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
}

static void SolveEyePosition(void)
{
    const float(*m)[4] = translucent_view_projection.elements;

    int columns[3] = {0, 1, 3};

    float a[3][3];
    float r[3];

    for (int row = 0; row < 3; row++)
    {
        int c     = columns[row];
        a[row][0] = m[0][c];
        a[row][1] = m[1][c];
        a[row][2] = m[2][c];
        r[row]    = -m[3][c];
    }

    float det = Determinant3(a[0][0], a[0][1], a[0][2], a[1][0], a[1][1], a[1][2], a[2][0], a[2][1], a[2][2]);

    if (fabsf(det) < 1e-12f)
    {
        translucent_eye = {0.0f, 0.0f, 0.0f};
        return;
    }

    float det_x = Determinant3(r[0], a[0][1], a[0][2], r[1], a[1][1], a[1][2], r[2], a[2][1], a[2][2]);
    float det_y = Determinant3(a[0][0], r[0], a[0][2], a[1][0], r[1], a[1][2], a[2][0], r[2], a[2][2]);
    float det_z = Determinant3(a[0][0], a[0][1], r[0], a[1][0], a[1][1], r[1], a[2][0], a[2][1], r[2]);

    translucent_eye = {det_x / det, det_y / det, det_z / det};
}

void TranslucentBegin(void)
{
    translucent_items.clear();
    translucent_points.clear();
    translucent_group_index.clear();

    translucent_view_projection = render_backend->WorldViewProjection();

    SolveEyePosition();
}

static epi::Vec3 Cross(const epi::Vec3 &a, const epi::Vec3 &b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

static float Dot(const epi::Vec3 &a, const epi::Vec3 &b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static epi::Vec3 Subtract(const epi::Vec3 &a, const epi::Vec3 &b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

static int GroupIndex(TranslucentKind kind, uint64_t group)
{
    uint64_t key = (group * 0x9E3779B97F4A7C15ULL) ^ (uint64_t)kind;

    std::unordered_map<uint64_t, int>::iterator found = translucent_group_index.find(key);

    if (found != translucent_group_index.end())
        return found->second;

    int index = (int)translucent_group_index.size();

    translucent_group_index[key] = index;

    return index;
}

static bool AddItem(TranslucentKind kind, int payload, uint64_t group, int point_first, int point_count)
{
    TranslucentItem item;

    item.normal         = {0.0f, 0.0f, 0.0f};
    item.plane_distance = 0.0f;
    item.eye_side       = 0.0f;
    item.planar         = false;
    item.point_first    = point_first;
    item.point_count    = point_count;
    item.kind           = kind;
    item.payload        = payload;
    item.sequence       = (int)translucent_items.size();
    item.group          = 0;

    float low_x  = 1.0f;
    float low_y  = 1.0f;
    float high_x = -1.0f;
    float high_y = -1.0f;

    epi::Vec3 center = {0.0f, 0.0f, 0.0f};

    translucent_clip.resize((size_t)point_count);

    int front = 0;

    for (int i = 0; i < point_count; i++)
    {
        const epi::Vec3 &point = translucent_points[(size_t)(point_first + i)];

        center.x += point.x;
        center.y += point.y;
        center.z += point.z;

        epi::Vec4 clip = epi::TransformVector(translucent_view_projection, epi::Vec4{point.x, point.y, point.z, 1.0f});

        translucent_clip[(size_t)i] = clip;

        if (clip.w > kTranslucentNearW)
        {
            front++;

            float x = clip.x / clip.w;
            float y = clip.y / clip.w;

            low_x  = epi::Min(low_x, x);
            low_y  = epi::Min(low_y, y);
            high_x = epi::Max(high_x, x);
            high_y = epi::Max(high_y, y);
        }
    }

    if (front == 0)
        return false;

    if (front < point_count)
    {
        for (int i = 0; i < point_count; i++)
        {
            const epi::Vec4 &near_point = translucent_clip[(size_t)i];

            if (near_point.w <= kTranslucentNearW)
                continue;

            for (int j = 0; j < point_count; j++)
            {
                const epi::Vec4 &far_point = translucent_clip[(size_t)j];

                if (far_point.w > kTranslucentNearW)
                    continue;

                float t = (near_point.w - kTranslucentNearW) / (near_point.w - far_point.w);

                float clip_x = near_point.x + (far_point.x - near_point.x) * t;
                float clip_y = near_point.y + (far_point.y - near_point.y) * t;

                float x = clip_x / kTranslucentNearW;
                float y = clip_y / kTranslucentNearW;

                low_x  = epi::Min(low_x, x);
                low_y  = epi::Min(low_y, y);
                high_x = epi::Max(high_x, x);
                high_y = epi::Max(high_y, y);
            }
        }
    }

    if (low_x > 1.0f || low_y > 1.0f || high_x < -1.0f || high_y < -1.0f)
        return false;

    item.rect[0] = epi::Clamp(low_x, -1.0f, 1.0f);
    item.rect[1] = epi::Clamp(low_y, -1.0f, 1.0f);
    item.rect[2] = epi::Clamp(high_x, -1.0f, 1.0f);
    item.rect[3] = epi::Clamp(high_y, -1.0f, 1.0f);

    float inverse = 1.0f / (float)point_count;

    center = {center.x * inverse, center.y * inverse, center.z * inverse};

    float radius = 0.0f;

    for (int i = 0; i < point_count; i++)
    {
        epi::Vec3 delta = Subtract(translucent_points[(size_t)(point_first + i)], center);

        radius = epi::Max(radius, Dot(delta, delta));
    }

    item.center = center;
    item.radius = sqrtf(radius);

    epi::Vec3 offset = Subtract(center, translucent_eye);

    item.distance = Dot(offset, offset);
    item.group    = GroupIndex(kind, group);

    translucent_items.push_back(item);

    return true;
}

void TranslucentAddPoints(TranslucentKind kind, int payload, uint64_t group, std::span<const epi::Vec3> points)
{
    if (points.empty())
        return;

    int first = (int)translucent_points.size();

    translucent_points.insert(translucent_points.end(), points.begin(), points.end());

    if (!AddItem(kind, payload, group, first, (int)points.size()))
        translucent_points.resize((size_t)first);
}

void TranslucentAddPlanarBox(TranslucentKind kind, int payload, uint64_t group, const epi::Vec3 &normal,
                             float plane_distance, const float low[3], const float high[3])
{
    int first = (int)translucent_points.size();

    for (int corner = 0; corner < 8; corner++)
    {
        translucent_points.push_back(
            {(corner & 1) ? high[0] : low[0], (corner & 2) ? high[1] : low[1], (corner & 4) ? high[2] : low[2]});
    }

    if (!AddItem(kind, payload, group, first, 8))
    {
        translucent_points.resize((size_t)first);
        return;
    }

    TranslucentItem &item = translucent_items.back();

    item.normal         = normal;
    item.plane_distance = plane_distance;
    item.eye_side       = Dot(normal, translucent_eye) + plane_distance;
    item.planar         = true;
}

void TranslucentAddPolygon(TranslucentKind kind, int payload, uint64_t group, GLuint shape,
                           std::span<const RendererVertex> vertices)
{
    if (vertices.empty())
        return;

    int first = (int)translucent_points.size();

    for (size_t i = 0; i < vertices.size(); i++)
        translucent_points.push_back(vertices[i].position);

    if (!AddItem(kind, payload, group, first, (int)vertices.size()))
    {
        translucent_points.resize((size_t)first);
        return;
    }

    TranslucentItem &item = translucent_items.back();

    const epi::Vec3 *points = translucent_points.data() + first;
    int              count  = (int)vertices.size();

    epi::Vec3 best        = {0.0f, 0.0f, 0.0f};
    float     best_length = 0.0f;

    if (shape == GL_TRIANGLES)
    {
        for (int i = 0; i + 2 < count; i += 3)
        {
            epi::Vec3 normal = Cross(Subtract(points[i + 1], points[i]), Subtract(points[i + 2], points[i]));
            float     length = Dot(normal, normal);

            if (length > best_length)
            {
                best        = normal;
                best_length = length;
            }
        }
    }
    else
    {
        for (int i = 1; i + 1 < count; i++)
        {
            epi::Vec3 normal = Cross(Subtract(points[i], points[0]), Subtract(points[i + 1], points[0]));
            float     length = Dot(normal, normal);

            if (length > best_length)
            {
                best        = normal;
                best_length = length;
            }
        }
    }

    if (best_length < 1e-8f)
        return;

    float inverse = 1.0f / sqrtf(best_length);

    epi::Vec3 normal = {best.x * inverse, best.y * inverse, best.z * inverse};
    float     plane  = -Dot(normal, points[0]);

    for (int i = 0; i < count; i++)
    {
        if (fabsf(Dot(normal, points[i]) + plane) > kTranslucentPlaneEpsilon)
            return;
    }

    item.normal         = normal;
    item.plane_distance = plane;
    item.eye_side       = Dot(normal, translucent_eye) + plane;
    item.planar         = true;
}

static int SideOfPlane(const TranslucentItem &plane, const TranslucentItem &other)
{
    float eye_side = plane.eye_side;

    if (fabsf(eye_side) < kTranslucentSideEpsilon)
        return 0;

    float sign = (eye_side > 0.0f) ? 1.0f : -1.0f;

    float center_side = (Dot(plane.normal, other.center) + plane.plane_distance) * sign;

    if (center_side - other.radius > -kTranslucentSideEpsilon)
        return (center_side + other.radius < kTranslucentSideEpsilon) ? 0 : -1;

    if (center_side + other.radius < kTranslucentSideEpsilon)
        return 1;

    bool all_front = true;
    bool all_back  = true;

    for (int i = 0; i < other.point_count; i++)
    {
        const epi::Vec3 &point = translucent_points[(size_t)(other.point_first + i)];

        float side = (Dot(plane.normal, point) + plane.plane_distance) * sign;

        if (side < -kTranslucentSideEpsilon)
            all_front = false;

        if (side > kTranslucentSideEpsilon)
            all_back = false;

        if (!all_front && !all_back)
            return 0;
    }

    if (all_front && all_back)
        return 0;

    return all_front ? -1 : 1;
}

static bool Coplanar(const TranslucentItem &a, const TranslucentItem &b)
{
    if (!a.planar || !b.planar)
        return false;

    float alignment = Dot(a.normal, b.normal);

    if (fabsf(alignment) < 0.999f)
        return false;

    float distance = (alignment > 0.0f) ? b.plane_distance : -b.plane_distance;

    return fabsf(a.plane_distance - distance) < kTranslucentSideEpsilon;
}

static int OrderPair(const TranslucentItem &a, const TranslucentItem &b)
{
    if (Coplanar(a, b))
        return 0;

    if (a.planar)
    {
        int side = SideOfPlane(a, b);

        if (side != 0)
            return side;
    }

    if (b.planar)
    {
        int side = SideOfPlane(b, a);

        if (side != 0)
            return -side;
    }

    return translucent_rank[(size_t)a.sequence] < translucent_rank[(size_t)b.sequence] ? -1 : 1;
}

static bool CompareRectLeft(int a, int b)
{
    return translucent_items[(size_t)a].rect[0] < translucent_items[(size_t)b].rect[0];
}

static bool CompareRank(int a, int b)
{
    const TranslucentItem &left  = translucent_items[(size_t)a];
    const TranslucentItem &right = translucent_items[(size_t)b];

    if (!epi::AlmostEquals(left.distance, right.distance))
        return left.distance > right.distance;

    return left.sequence < right.sequence;
}

static void BuildConstraints(void)
{
    translucent_edge_from.clear();
    translucent_edge_to.clear();

    int total = (int)translucent_items.size();

    translucent_sweep.resize((size_t)total);

    for (int i = 0; i < total; i++)
        translucent_sweep[(size_t)i] = i;

    std::sort(translucent_sweep.begin(), translucent_sweep.end(), CompareRectLeft);

    translucent_active.clear();

    for (int s = 0; s < total; s++)
    {
        int                    current = translucent_sweep[(size_t)s];
        const TranslucentItem &a       = translucent_items[(size_t)current];

        float low_x  = a.rect[0];
        float low_y  = a.rect[1];
        float high_y = a.rect[3];

        size_t keep = 0;

        for (size_t k = 0; k < translucent_active.size(); k++)
        {
            const TranslucentSweepEntry &entry = translucent_active[k];

            if (entry.high_x < low_x)
                continue;

            translucent_active[keep++] = entry;

            if (entry.low_y > high_y || low_y > entry.high_y)
                continue;

            int                    other = entry.item;
            const TranslucentItem &b     = translucent_items[(size_t)other];

            int order = OrderPair(a, b);

            if (order == 0)
                continue;

            translucent_edge_from.push_back(order < 0 ? current : other);
            translucent_edge_to.push_back(order < 0 ? other : current);
        }

        translucent_active.resize(keep);
        translucent_active.push_back({a.rect[2], a.rect[1], a.rect[3], current});
    }
}

struct RankGreater
{
    bool operator()(int a, int b) const
    {
        return translucent_rank[(size_t)a] > translucent_rank[(size_t)b];
    }
};

static void MarkReady(int item)
{
    translucent_ready.push_back(item);
    std::push_heap(translucent_ready.begin(), translucent_ready.end(), RankGreater());

    translucent_group_ready[(size_t)translucent_items[(size_t)item].group].push_back(item);
}

static void SortItems(void)
{
    int total = (int)translucent_items.size();

    translucent_by_rank.resize((size_t)total);

    for (int i = 0; i < total; i++)
        translucent_by_rank[(size_t)i] = i;

    std::sort(translucent_by_rank.begin(), translucent_by_rank.end(), CompareRank);

    translucent_rank.resize((size_t)total);

    for (int r = 0; r < total; r++)
        translucent_rank[(size_t)translucent_by_rank[(size_t)r]] = r;

    BuildConstraints();

    int edges = (int)translucent_edge_from.size();

    translucent_edge_first.assign((size_t)total + 1, 0);
    translucent_in_degree.assign((size_t)total, 0);

    for (int e = 0; e < edges; e++)
    {
        translucent_edge_first[(size_t)translucent_edge_from[(size_t)e] + 1]++;
        translucent_in_degree[(size_t)translucent_edge_to[(size_t)e]]++;
    }

    for (int i = 0; i < total; i++)
        translucent_edge_first[(size_t)i + 1] += translucent_edge_first[(size_t)i];

    translucent_edge_list.resize((size_t)edges);

    translucent_fill.assign(translucent_edge_first.begin(), translucent_edge_first.end() - 1);

    for (int e = 0; e < edges; e++)
        translucent_edge_list[(size_t)translucent_fill[(size_t)translucent_edge_from[(size_t)e]]++] =
            translucent_edge_to[(size_t)e];

    translucent_emitted.assign((size_t)total, false);
    translucent_order.clear();
    translucent_wave_ends.clear();
    translucent_ready.clear();

    int groups = (int)translucent_group_index.size();

    if ((int)translucent_group_ready.size() < groups)
        translucent_group_ready.resize((size_t)groups);

    for (int g = 0; g < groups; g++)
        translucent_group_ready[(size_t)g].clear();

    for (int i = 0; i < total; i++)
    {
        if (translucent_in_degree[(size_t)i] == 0)
            MarkReady(i);
    }

    int scan  = 0;
    int group = -1;

    while ((int)translucent_order.size() < total)
    {
        if (group < 0 || translucent_group_ready[(size_t)group].empty())
        {
            group = -1;

            while (!translucent_ready.empty())
            {
                std::pop_heap(translucent_ready.begin(), translucent_ready.end(), RankGreater());

                int candidate = translucent_ready.back();
                translucent_ready.pop_back();

                if (!translucent_emitted[(size_t)candidate])
                {
                    group = translucent_items[(size_t)candidate].group;
                    break;
                }
            }
        }

        translucent_wave.clear();

        if (group >= 0)
        {
            std::vector<int> &bucket = translucent_group_ready[(size_t)group];

            for (size_t k = 0; k < bucket.size(); k++)
            {
                if (!translucent_emitted[(size_t)bucket[k]])
                    translucent_wave.push_back(bucket[k]);
            }

            bucket.clear();
        }

        if (translucent_wave.empty())
        {
            while (translucent_emitted[(size_t)translucent_by_rank[(size_t)scan]])
                scan++;

            translucent_wave.push_back(translucent_by_rank[(size_t)scan]);

            group = -1;
        }

        std::sort(translucent_wave.begin(), translucent_wave.end());

        for (size_t w = 0; w < translucent_wave.size(); w++)
        {
            int next = translucent_wave[w];

            translucent_emitted[(size_t)next] = true;
            translucent_order.push_back(next);
        }

        translucent_wave_ends.push_back((int)translucent_order.size());

        for (size_t w = 0; w < translucent_wave.size(); w++)
        {
            int next = translucent_wave[w];

            for (int e = translucent_edge_first[(size_t)next]; e < translucent_edge_first[(size_t)next + 1]; e++)
            {
                int target = translucent_edge_list[(size_t)e];

                if (translucent_emitted[(size_t)target])
                    continue;

                translucent_in_degree[(size_t)target]--;

                if (translucent_in_degree[(size_t)target] == 0)
                    MarkReady(target);
            }
        }
    }
}

void EmitTranslucentUnits(std::span<const int> payloads)
{
    for (size_t i = 0; i < payloads.size(); i++)
        ReplayRetainedUnit(payloads[i]);
}

void TranslucentDraw(void)
{
    if (translucent_items.empty())
        return;

    SortItems();

    size_t index = 0;

    for (size_t w = 0; w < translucent_wave_ends.size(); w++)
    {
        size_t end = (size_t)translucent_wave_ends[w];

        int kind = translucent_items[(size_t)translucent_order[index]].kind;

        translucent_payloads.clear();

        for (; index < end; index++)
            translucent_payloads.push_back(translucent_items[(size_t)translucent_order[index]].payload);

        switch (kind)
        {
        case kTranslucentUnit:
            EmitTranslucentUnits(translucent_payloads);
            break;
        case kTranslucentStatic:
            EmitTranslucentStatic(translucent_payloads);
            break;
        case kTranslucentSprite:
            EmitTranslucentSprites(translucent_payloads);
            break;
        default:
            EmitTranslucentModels(translucent_payloads);
            break;
        }
    }

    translucent_items.clear();
    translucent_points.clear();
}
