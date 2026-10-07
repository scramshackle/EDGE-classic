#include "r_polygon.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

#include "earcut.hpp"

#include "con_var.h"
#include "epi.h"
#include "epi_math.h"
#include "i_system.h"
#include "r_misc.h"
#include "r_state.h"

struct PolygonEdge
{
    int start;
    int end;
};

static std::vector<SectorPolygon> sector_polygons;
static bool                       sector_polygons_built = false;

static uint64_t polygon_build_microseconds = 0;
static int      polygon_status_counts[kSectorPolygonStatusTotal];
static int      polygon_sectors_traced      = 0;
static int      polygon_total_loops         = 0;
static int      polygon_total_holes         = 0;
static int      polygon_total_triangles     = 0;
static int      polygon_total_points        = 0;
static int      polygon_winding_disagree    = 0;
static int      polygon_area_mismatch_loops = 0;
static double   polygon_area_triangles      = 0.0;
static double   polygon_area_loops          = 0.0;
static int      polygon_deep_water_sectors  = 0;
static int      polygon_oversized_sectors   = 0;
static int      polygon_gap_sectors         = 0;
static int      polygon_gap_edges           = 0;

static std::vector<int>              polygon_self_reference_owned;
static std::vector<uint8_t>          polygon_self_reference_only;
static std::vector<float>            polygon_self_reference_probe;
static std::vector<std::vector<int>> polygon_self_reference_ring;

static std::vector<SectorPolygonContainment> polygon_self_reference_containers;

static int polygon_self_reference_loops    = 0;
static int polygon_self_reference_attached = 0;
static int polygon_self_reference_covered  = 0;
static int polygon_self_reference_orphan   = 0;
static int polygon_self_reference_open     = 0;


static std::vector<int> grid_starts;
static std::vector<int> grid_sectors;

static float grid_origin_x = 0.0f;
static float grid_origin_y = 0.0f;
static float grid_cell     = 128.0f;
static int   grid_width    = 0;
static int   grid_height   = 0;

struct PolygonCellEdge
{
    float x1;
    float y1;
    float x2;
    float y2;
};

enum PolygonCellMode
{
    kPolygonCellTest = 0,
    kPolygonCellInside,
    kPolygonCellFallback
};

struct PolygonCellEntry
{
    int     sector;
    int     edge_first;
    int     edge_count;
    float   reference_x;
    float   reference_y;
    uint8_t reference_inside;
    uint8_t mode;
};

struct PolygonCellHit
{
    int      cell;
    int      sector;
    uint32_t vertex_a;
    uint32_t vertex_b;
};

static std::vector<int>              cell_entry_starts;
static std::vector<PolygonCellEntry> cell_entries;
static std::vector<PolygonCellEdge>  cell_edges;

static constexpr double kPolygonCellMargin    = 1.0;
static constexpr double kPolygonCellTolerance = 0.05;
static constexpr double kPolygonCellClearance = 0.5;

static std::vector<int> vertex_point_map;
static std::vector<int> vertex_point_stamp;
static int              vertex_point_serial = 0;

static const char *polygon_status_names[kSectorPolygonStatusTotal] = {
    "ok",     "no boundary edges",       "open loop", "degenerate loops", "triangulation incomplete",
    "open self-reference"};

const char *SectorPolygonStatusName(int status)
{
    if (status < 0 || status >= kSectorPolygonStatusTotal)
        return "unknown";

    return polygon_status_names[status];
}

static bool PolygonEdgeLess(const PolygonEdge &a, const PolygonEdge &b)
{
    return a.start < b.start;
}

static int PolygonEdgeLowerBound(const std::vector<PolygonEdge> &edges, int start)
{
    int low  = 0;
    int high = (int)edges.size();

    while (low < high)
    {
        int mid = (low + high) / 2;

        if (edges[mid].start < start)
            low = mid + 1;
        else
            high = mid;
    }

    return low;
}

static float PolygonPseudoAngle(float dx, float dy)
{
    float span = fabsf(dx) + fabsf(dy);

    if (span <= 0.0f)
        return 0.0f;

    float value = dy / span;

    if (dx < 0.0f)
        return 2.0f - value;

    if (dy < 0.0f)
        return 4.0f + value;

    return value;
}

static double PolygonLoopArea(const std::vector<int> &loop)
{
    double total = 0.0;
    size_t count = loop.size();

    for (size_t i = 0; i < count; i++)
    {
        const Vertex *a = level_vertexes + loop[i];
        const Vertex *b = level_vertexes + loop[(i + 1) % count];

        total += (double)a->X * (double)b->Y - (double)b->X * (double)a->Y;
    }

    return total * 0.5;
}

static bool PolygonPointInLoop(const std::vector<int> &loop, float px, float py)
{
    bool   inside = false;
    size_t count  = loop.size();

    for (size_t i = 0; i < count; i++)
    {
        const Vertex *a = level_vertexes + loop[i];
        const Vertex *b = level_vertexes + loop[(i + 1) % count];

        if ((a->Y > py) != (b->Y > py))
        {
            float cross_x = a->X + (py - a->Y) / (b->Y - a->Y) * (b->X - a->X);

            if (cross_x > px)
                inside = !inside;
        }
    }

    return inside;
}


static void PolygonLoopProbe(const std::vector<int> &loop, double area, float *probe_x, float *probe_y)
{
    size_t count       = loop.size();
    size_t best        = 0;
    float  best_length = -1.0f;

    for (size_t i = 0; i < count; i++)
    {
        const Vertex *a = level_vertexes + loop[i];
        const Vertex *b = level_vertexes + loop[(i + 1) % count];

        float dx     = b->X - a->X;
        float dy     = b->Y - a->Y;
        float length = dx * dx + dy * dy;

        if (length > best_length)
        {
            best_length = length;
            best        = i;
        }
    }

    const Vertex *a = level_vertexes + loop[best];
    const Vertex *b = level_vertexes + loop[(best + 1) % count];

    float dx     = b->X - a->X;
    float dy     = b->Y - a->Y;
    float length = sqrtf(dx * dx + dy * dy);

    if (length <= 0.0f)
    {
        *probe_x = a->X;
        *probe_y = a->Y;

        return;
    }

    float side = (area < 0.0) ? 1.0f : -1.0f;

    *probe_x = (a->X + b->X) * 0.5f + side * 0.01f * dy / length;
    *probe_y = (a->Y + b->Y) * 0.5f - side * 0.01f * dx / length;
}

static void PolygonAppendRing(std::vector<std::vector<std::array<float, 2>>> &rings, std::vector<int> &ring_vertices,
                              const std::vector<int> &loop)
{
    std::vector<std::array<float, 2>> ring;

    ring.reserve(loop.size());

    for (size_t i = 0; i < loop.size(); i++)
    {
        const Vertex *point = level_vertexes + loop[i];

        std::array<float, 2> entry = {{point->X, point->Y}};

        ring.push_back(entry);

        ring_vertices.push_back(loop[i]);
    }

    rings.push_back(ring);
}

static int PolygonMapPoint(SectorPolygon *poly, int vertex_index)
{
    if (vertex_point_stamp[vertex_index] == vertex_point_serial)
        return vertex_point_map[vertex_index];

    vertex_point_stamp[vertex_index] = vertex_point_serial;
    vertex_point_map[vertex_index]   = (int)poly->points.size();

    poly->points.push_back(level_vertexes + vertex_index);

    return vertex_point_map[vertex_index];
}

struct PolygonIncidence
{
    int vertex;
    int edge;
};

static bool PolygonIncidenceLess(const PolygonIncidence &a, const PolygonIncidence &b)
{
    return a.vertex < b.vertex;
}

static int PolygonIncidenceLowerBound(const std::vector<PolygonIncidence> &list, int vertex)
{
    int low  = 0;
    int high = (int)list.size();

    while (low < high)
    {
        int mid = (low + high) / 2;

        if (list[mid].vertex < vertex)
            low = mid + 1;
        else
            high = mid;
    }

    return low;
}

static void PolygonStoreLoop(SectorPolygon *poly, const std::vector<int> &loop)
{
    if (poly->loop_starts.empty())
        poly->loop_starts.push_back(0);

    for (size_t i = 0; i < loop.size(); i++)
        poly->loop_points.push_back((uint32_t)loop[i]);

    poly->loop_starts.push_back((uint32_t)poly->loop_points.size());
}

static int PolygonStoredLoops(const SectorPolygon *poly)
{
    return poly->loop_starts.empty() ? 0 : (int)poly->loop_starts.size() - 1;
}

static void PolygonReadLoop(const SectorPolygon *poly, int index, std::vector<int> *loop)
{
    loop->clear();

    for (uint32_t i = poly->loop_starts[index]; i < poly->loop_starts[index + 1]; i++)
        loop->push_back((int)poly->loop_points[i]);
}

static void PolygonRefreshBounds(SectorPolygon *poly)
{
    poly->bounds[0] = poly->bounds[1] = FLT_MAX;
    poly->bounds[2] = poly->bounds[3] = -FLT_MAX;

    for (size_t i = 0; i < poly->loop_points.size(); i++)
    {
        const Vertex *point = level_vertexes + poly->loop_points[i];

        if (point->X < poly->bounds[0])
            poly->bounds[0] = point->X;

        if (point->Y < poly->bounds[1])
            poly->bounds[1] = point->Y;

        if (point->X > poly->bounds[2])
            poly->bounds[2] = point->X;

        if (point->Y > poly->bounds[3])
            poly->bounds[3] = point->Y;
    }
}

static bool PolygonSectorContains(const SectorPolygon *poly, float px, float py)
{
    if (px < poly->bounds[0] || px > poly->bounds[2] || py < poly->bounds[1] || py > poly->bounds[3])
        return false;

    bool inside = false;

    for (size_t i = 0; i + 1 < poly->loop_starts.size(); i++)
    {
        uint32_t begin = poly->loop_starts[i];
        uint32_t count = poly->loop_starts[i + 1] - begin;

        for (uint32_t k = 0; k < count; k++)
        {
            const Vertex *a = level_vertexes + poly->loop_points[begin + k];
            const Vertex *b = level_vertexes + poly->loop_points[begin + (k + 1) % count];

            if ((a->Y > py) != (b->Y > py))
            {
                float cross_x = a->X + (py - a->Y) / (b->Y - a->Y) * (b->X - a->X);

                if (cross_x > px)
                    inside = !inside;
            }
        }
    }

    return inside;
}

static void DestroySectorGrid(void)
{
    grid_starts.clear();
    grid_sectors.clear();

    cell_entry_starts.clear();
    cell_entries.clear();
    cell_edges.clear();

    grid_width  = 0;
    grid_height = 0;
}

static void BuildSectorGrid(void)
{
    DestroySectorGrid();

    float min_x = FLT_MAX;
    float min_y = FLT_MAX;
    float max_x = -FLT_MAX;
    float max_y = -FLT_MAX;

    for (size_t i = 0; i < sector_polygons.size(); i++)
    {
        const SectorPolygon *poly = &sector_polygons[i];

        if (poly->loop_points.empty())
            continue;

        min_x = HMM_MIN(min_x, poly->bounds[0]);
        min_y = HMM_MIN(min_y, poly->bounds[1]);
        max_x = HMM_MAX(max_x, poly->bounds[2]);
        max_y = HMM_MAX(max_y, poly->bounds[3]);
    }

    if (min_x > max_x || min_y > max_y)
        return;

    grid_cell = 128.0f;

    while ((max_x - min_x) / grid_cell > 1024.0f || (max_y - min_y) / grid_cell > 1024.0f)
        grid_cell *= 2.0f;

    grid_origin_x = min_x;
    grid_origin_y = min_y;

    grid_width  = (int)((max_x - min_x) / grid_cell) + 1;
    grid_height = (int)((max_y - min_y) / grid_cell) + 1;

    size_t cells = (size_t)grid_width * (size_t)grid_height;

    grid_starts.assign(cells + 1, 0);

    for (size_t i = 0; i < sector_polygons.size(); i++)
    {
        const SectorPolygon *poly = &sector_polygons[i];

        if (poly->loop_points.empty())
            continue;

        int x0 = (int)((poly->bounds[0] - grid_origin_x) / grid_cell);
        int y0 = (int)((poly->bounds[1] - grid_origin_y) / grid_cell);
        int x1 = (int)((poly->bounds[2] - grid_origin_x) / grid_cell);
        int y1 = (int)((poly->bounds[3] - grid_origin_y) / grid_cell);

        for (int y = y0; y <= y1; y++)
        {
            for (int x = x0; x <= x1; x++)
                grid_starts[(size_t)y * grid_width + x + 1]++;
        }
    }

    for (size_t i = 1; i < grid_starts.size(); i++)
        grid_starts[i] += grid_starts[i - 1];

    grid_sectors.assign((size_t)grid_starts.back(), 0);

    std::vector<int> cursor(grid_starts.begin(), grid_starts.end() - 1);

    for (size_t i = 0; i < sector_polygons.size(); i++)
    {
        const SectorPolygon *poly = &sector_polygons[i];

        if (poly->loop_points.empty())
            continue;

        int x0 = (int)((poly->bounds[0] - grid_origin_x) / grid_cell);
        int y0 = (int)((poly->bounds[1] - grid_origin_y) / grid_cell);
        int x1 = (int)((poly->bounds[2] - grid_origin_x) / grid_cell);
        int y1 = (int)((poly->bounds[3] - grid_origin_y) / grid_cell);

        for (int y = y0; y <= y1; y++)
        {
            for (int x = x0; x <= x1; x++)
                grid_sectors[(size_t)cursor[(size_t)y * grid_width + x]++] = (int)i;
        }
    }
}

static bool PolygonCellHitLess(const PolygonCellHit &a, const PolygonCellHit &b)
{
    if (a.cell != b.cell)
        return a.cell < b.cell;

    return a.sector < b.sector;
}

static bool SegmentTouchesRectangle(double ax, double ay, double bx, double by, double x0, double y0, double x1,
                                    double y1)
{
    if (HMM_MAX(ax, bx) < x0 || HMM_MIN(ax, bx) > x1 || HMM_MAX(ay, by) < y0 || HMM_MIN(ay, by) > y1)
        return false;

    double dx = bx - ax;
    double dy = by - ay;

    double c0 = dx * (y0 - ay) - dy * (x0 - ax);
    double c1 = dx * (y0 - ay) - dy * (x1 - ax);
    double c2 = dx * (y1 - ay) - dy * (x0 - ax);
    double c3 = dx * (y1 - ay) - dy * (x1 - ax);

    if (c0 > 0.0 && c1 > 0.0 && c2 > 0.0 && c3 > 0.0)
        return false;

    if (c0 < 0.0 && c1 < 0.0 && c2 < 0.0 && c3 < 0.0)
        return false;

    return true;
}

static double PointSegmentDistance(double px, double py, double ax, double ay, double bx, double by)
{
    double dx     = bx - ax;
    double dy     = by - ay;
    double length = dx * dx + dy * dy;
    double t      = (length > 0.0) ? ((px - ax) * dx + (py - ay) * dy) / length : 0.0;

    t = HMM_MAX(0.0, HMM_MIN(1.0, t));

    double nx = ax + t * dx - px;
    double ny = ay + t * dy - py;

    return sqrt(nx * nx + ny * ny);
}

static void BuildPolygonCellEdges(void)
{
    cell_entry_starts.clear();
    cell_entries.clear();
    cell_edges.clear();

    if (grid_width <= 0 || grid_height <= 0)
        return;

    std::vector<PolygonCellHit> hits;

    for (size_t i = 0; i < sector_polygons.size(); i++)
    {
        const SectorPolygon *poly = &sector_polygons[i];

        for (size_t loop = 0; loop + 1 < poly->loop_starts.size(); loop++)
        {
            uint32_t begin = poly->loop_starts[loop];
            uint32_t count = poly->loop_starts[loop + 1] - begin;

            for (uint32_t k = 0; k < count; k++)
            {
                uint32_t vertex_a = poly->loop_points[begin + k];
                uint32_t vertex_b = poly->loop_points[begin + (k + 1) % count];

                const Vertex *a = level_vertexes + vertex_a;
                const Vertex *b = level_vertexes + vertex_b;

                if (epi::AlmostEquals(a->X, b->X) && epi::AlmostEquals(a->Y, b->Y))
                    continue;

                double low_x  = HMM_MIN(a->X, b->X) - kPolygonCellMargin;
                double low_y  = HMM_MIN(a->Y, b->Y) - kPolygonCellMargin;
                double high_x = HMM_MAX(a->X, b->X) + kPolygonCellMargin;
                double high_y = HMM_MAX(a->Y, b->Y) + kPolygonCellMargin;

                int x0 = HMM_MAX(0, (int)floor((low_x - grid_origin_x) / grid_cell));
                int y0 = HMM_MAX(0, (int)floor((low_y - grid_origin_y) / grid_cell));
                int x1 = HMM_MIN(grid_width - 1, (int)floor((high_x - grid_origin_x) / grid_cell));
                int y1 = HMM_MIN(grid_height - 1, (int)floor((high_y - grid_origin_y) / grid_cell));

                for (int y = y0; y <= y1; y++)
                {
                    for (int x = x0; x <= x1; x++)
                    {
                        double cell_x0 = grid_origin_x + x * (double)grid_cell - kPolygonCellMargin;
                        double cell_y0 = grid_origin_y + y * (double)grid_cell - kPolygonCellMargin;
                        double cell_x1 = cell_x0 + grid_cell + 2.0 * kPolygonCellMargin;
                        double cell_y1 = cell_y0 + grid_cell + 2.0 * kPolygonCellMargin;

                        if (!SegmentTouchesRectangle(a->X, a->Y, b->X, b->Y, cell_x0, cell_y0, cell_x1, cell_y1))
                            continue;

                        hits.push_back(PolygonCellHit{y * grid_width + x, (int)i, vertex_a, vertex_b});
                    }
                }
            }
        }
    }

    std::sort(hits.begin(), hits.end(), PolygonCellHitLess);

    static const float reference_fractions[9][2] = {{0.5f, 0.5f},   {0.25f, 0.25f}, {0.75f, 0.25f},
                                                    {0.25f, 0.75f}, {0.75f, 0.75f}, {0.5f, 0.25f},
                                                    {0.5f, 0.75f},  {0.25f, 0.5f},  {0.75f, 0.5f}};

    size_t cells = (size_t)grid_width * (size_t)grid_height;
    size_t h     = 0;

    cell_entry_starts.assign(cells + 1, 0);

    for (size_t cell = 0; cell < cells; cell++)
    {
        cell_entry_starts[cell] = (int)cell_entries.size();

        float cell_x = grid_origin_x + (float)(cell % (size_t)grid_width) * grid_cell;
        float cell_y = grid_origin_y + (float)(cell / (size_t)grid_width) * grid_cell;

        for (int g = grid_starts[cell]; g < grid_starts[cell + 1]; g++)
        {
            int index = grid_sectors[(size_t)g];

            while (h < hits.size() && (hits[h].cell < (int)cell || (hits[h].cell == (int)cell && hits[h].sector < index)))
                h++;

            PolygonCellEntry entry;

            entry.sector           = index;
            entry.edge_first       = (int)cell_edges.size();
            entry.edge_count       = 0;
            entry.reference_x      = cell_x + grid_cell * 0.5f;
            entry.reference_y      = cell_y + grid_cell * 0.5f;
            entry.reference_inside = 0;
            entry.mode             = kPolygonCellFallback;

            while (h < hits.size() && hits[h].cell == (int)cell && hits[h].sector == index)
            {
                const Vertex *a = level_vertexes + hits[h].vertex_a;
                const Vertex *b = level_vertexes + hits[h].vertex_b;

                cell_edges.push_back(PolygonCellEdge{a->X, a->Y, b->X, b->Y});
                entry.edge_count++;
                h++;
            }

            const SectorPolygon *poly = &sector_polygons[(size_t)index];

            if (entry.edge_count == 0)
            {
                if (!PolygonSectorContains(poly, entry.reference_x, entry.reference_y))
                    continue;

                entry.reference_inside = 1;
                entry.mode             = kPolygonCellInside;

                cell_entries.push_back(entry);
                continue;
            }

            for (int r = 0; r < 9; r++)
            {
                float rx = cell_x + grid_cell * reference_fractions[r][0];
                float ry = cell_y + grid_cell * reference_fractions[r][1];

                bool clear = true;

                for (int e = 0; e < entry.edge_count && clear; e++)
                {
                    const PolygonCellEdge &edge = cell_edges[(size_t)(entry.edge_first + e)];

                    if (PointSegmentDistance(rx, ry, edge.x1, edge.y1, edge.x2, edge.y2) < kPolygonCellClearance)
                        clear = false;
                }

                if (!clear)
                    continue;

                entry.reference_x      = rx;
                entry.reference_y      = ry;
                entry.reference_inside = PolygonSectorContains(poly, rx, ry) ? 1 : 0;
                entry.mode             = kPolygonCellTest;
                break;
            }

            cell_entries.push_back(entry);
        }
    }

    cell_entry_starts[cells] = (int)cell_entries.size();
}

static int PolygonCellCrossings(const PolygonCellEntry *entry, float px, float py)
{
    double rx  = entry->reference_x;
    double ry  = entry->reference_y;
    double rpx = px - rx;
    double rpy = py - ry;

    double rp_length = sqrt(rpx * rpx + rpy * rpy);

    if (rp_length < kPolygonCellTolerance)
        return -1;

    double low_x  = HMM_MIN(rx, (double)px) - kPolygonCellTolerance;
    double low_y  = HMM_MIN(ry, (double)py) - kPolygonCellTolerance;
    double high_x = HMM_MAX(rx, (double)px) + kPolygonCellTolerance;
    double high_y = HMM_MAX(ry, (double)py) + kPolygonCellTolerance;

    int crossings = 0;

    for (int e = 0; e < entry->edge_count; e++)
    {
        const PolygonCellEdge &edge = cell_edges[(size_t)(entry->edge_first + e)];

        if (HMM_MAX(edge.x1, edge.x2) < low_x || HMM_MIN(edge.x1, edge.x2) > high_x ||
            HMM_MAX(edge.y1, edge.y2) < low_y || HMM_MIN(edge.y1, edge.y2) > high_y)
            continue;

        double ex = (double)edge.x2 - edge.x1;
        double ey = (double)edge.y2 - edge.y1;

        double e_length = sqrt(ex * ex + ey * ey);

        double d1 = ex * (ry - edge.y1) - ey * (rx - edge.x1);
        double d2 = ex * (py - edge.y1) - ey * (px - edge.x1);
        double d3 = rpx * (edge.y1 - ry) - rpy * (edge.x1 - rx);
        double d4 = rpx * (edge.y2 - ry) - rpy * (edge.x2 - rx);

        if (fabs(d1) <= kPolygonCellTolerance * e_length || fabs(d2) <= kPolygonCellTolerance * e_length ||
            fabs(d3) <= kPolygonCellTolerance * rp_length || fabs(d4) <= kPolygonCellTolerance * rp_length)
            return -1;

        if ((d1 > 0.0) != (d2 > 0.0) && (d3 > 0.0) != (d4 > 0.0))
            crossings++;
    }

    return crossings;
}

static bool PolygonCellContains(const PolygonCellEntry *entry, const SectorPolygon *poly, float px, float py)
{
    if (px < poly->bounds[0] || px > poly->bounds[2] || py < poly->bounds[1] || py > poly->bounds[3])
        return false;

    if (entry->mode == kPolygonCellInside)
        return true;

    if (entry->mode == kPolygonCellTest)
    {
        int crossings = PolygonCellCrossings(entry, px, py);

        if (crossings >= 0)
            return ((crossings & 1) != 0) != (entry->reference_inside != 0);
    }

    return PolygonSectorContains(poly, px, py);
}

static int SectorPolygonAtPointScan(size_t cell, float x, float y, int exclude_sector)
{
    int    best      = -1;
    double best_area = 0.0;

    for (int i = grid_starts[cell]; i < grid_starts[cell + 1]; i++)
    {
        int index = grid_sectors[i];

        if (index == exclude_sector)
            continue;

        const SectorPolygon *poly = &sector_polygons[index];

        if (!PolygonSectorContains(poly, x, y))
            continue;

        double area = (double)(poly->bounds[2] - poly->bounds[0]) * (double)(poly->bounds[3] - poly->bounds[1]);

        if (best < 0 || area < best_area)
        {
            best      = index;
            best_area = area;
        }
    }

    return best;
}

int SectorPolygonAtPoint(float x, float y, int exclude_sector)
{
    if (grid_width <= 0)
        return -1;

    int cx = (int)((x - grid_origin_x) / grid_cell);
    int cy = (int)((y - grid_origin_y) / grid_cell);

    if (cx < 0 || cy < 0 || cx >= grid_width || cy >= grid_height)
        return -1;

    size_t cell = (size_t)cy * grid_width + cx;

    if (cell_entry_starts.empty())
        return SectorPolygonAtPointScan(cell, x, y, exclude_sector);

    int    best      = -1;
    double best_area = 0.0;

    for (int i = cell_entry_starts[cell]; i < cell_entry_starts[cell + 1]; i++)
    {
        const PolygonCellEntry *entry = &cell_entries[(size_t)i];

        if (entry->sector == exclude_sector)
            continue;

        const SectorPolygon *poly = &sector_polygons[(size_t)entry->sector];

        if (!PolygonCellContains(entry, poly, x, y))
            continue;

        double area = (double)(poly->bounds[2] - poly->bounds[0]) * (double)(poly->bounds[3] - poly->bounds[1]);

        if (best < 0 || area < best_area)
        {
            best      = entry->sector;
            best_area = area;
        }
    }

    return best;
}

static constexpr size_t kMaximumPolygonGapVertices = 512;

struct PolygonGapCandidate
{
    double distance;
    int    end_slot;
    int    start_slot;
};

static bool PolygonGapCandidateLess(const PolygonGapCandidate &a, const PolygonGapCandidate &b)
{
    return a.distance < b.distance;
}

static void PolygonCloseGaps(std::vector<PolygonEdge> &edges)
{
    std::vector<std::pair<int, int>> incidence;

    incidence.reserve(edges.size() * 2);

    for (size_t i = 0; i < edges.size(); i++)
    {
        incidence.push_back(std::make_pair(edges[i].start, 1));
        incidence.push_back(std::make_pair(edges[i].end, -1));
    }

    std::sort(incidence.begin(), incidence.end());

    std::vector<int> open_ends;
    std::vector<int> open_starts;

    for (size_t i = 0; i < incidence.size();)
    {
        int vertex  = incidence[i].first;
        int balance = 0;

        for (; i < incidence.size() && incidence[i].first == vertex; i++)
            balance += incidence[i].second;

        for (; balance > 0; balance--)
            open_starts.push_back(vertex);

        for (; balance < 0; balance++)
            open_ends.push_back(vertex);
    }

    if (open_ends.empty() || open_ends.size() != open_starts.size() ||
        open_ends.size() > kMaximumPolygonGapVertices)
        return;

    std::vector<PolygonGapCandidate> candidates;

    candidates.reserve(open_ends.size() * open_starts.size());

    for (size_t e = 0; e < open_ends.size(); e++)
    {
        const Vertex *from = level_vertexes + open_ends[e];

        for (size_t k = 0; k < open_starts.size(); k++)
        {
            if (open_starts[k] == open_ends[e])
                continue;

            const Vertex *to = level_vertexes + open_starts[k];

            double dx = (double)to->X - (double)from->X;
            double dy = (double)to->Y - (double)from->Y;

            candidates.push_back(PolygonGapCandidate{dx * dx + dy * dy, (int)e, (int)k});
        }
    }

    std::sort(candidates.begin(), candidates.end(), PolygonGapCandidateLess);

    std::vector<uint8_t> end_used(open_ends.size(), 0);
    std::vector<uint8_t> start_used(open_starts.size(), 0);

    int added = 0;

    for (size_t c = 0; c < candidates.size(); c++)
    {
        const PolygonGapCandidate &candidate = candidates[c];

        if (end_used[candidate.end_slot] || start_used[candidate.start_slot])
            continue;

        end_used[candidate.end_slot]     = 1;
        start_used[candidate.start_slot] = 1;

        PolygonEdge edge;

        edge.start = open_ends[candidate.end_slot];
        edge.end   = open_starts[candidate.start_slot];

        edges.push_back(edge);

        added++;
    }

    if (added > 0)
    {
        polygon_gap_sectors++;
        polygon_gap_edges += added;
    }
}

static bool PolygonTraceLoops(const Sector *sec, std::vector<PolygonEdge> &edges, std::vector<std::vector<int>> &loops)
{
    edges.clear();

    for (int i = 0; i < sec->line_count; i++)
    {
        const Line *line = sec->lines[i];

        if (line->front_sector == line->back_sector)
            continue;

        int start = (int)(line->vertex_1 - level_vertexes);
        int end   = (int)(line->vertex_2 - level_vertexes);

        if (start == end)
            continue;

        PolygonEdge edge;

        if (line->front_sector == sec)
        {
            edge.start = start;
            edge.end   = end;

            edges.push_back(edge);
        }

        if (line->back_sector == sec)
        {
            edge.start = end;
            edge.end   = start;

            edges.push_back(edge);
        }
    }

    if (edges.empty())
        return false;

    PolygonCloseGaps(edges);

    std::sort(edges.begin(), edges.end(), PolygonEdgeLess);

    std::vector<uint8_t> used((size_t)edges.size(), 0);

    size_t remaining = edges.size();
    size_t scan      = 0;

    while (remaining > 0)
    {
        while (scan < edges.size() && used[scan])
            scan++;

        if (scan >= edges.size())
            break;

        int first = edges[scan].start;
        int prev  = first;
        int cur   = edges[scan].end;

        used[scan] = 1;
        remaining--;

        std::vector<int> loop;

        loop.push_back(first);

        while (cur != first)
        {
            const Vertex *here = level_vertexes + cur;
            const Vertex *back = level_vertexes + prev;

            float reverse_angle = PolygonPseudoAngle(back->X - here->X, back->Y - here->Y);

            int   pick       = -1;
            float pick_delta = FLT_MAX;

            for (int k = PolygonEdgeLowerBound(edges, cur); k < (int)edges.size() && edges[k].start == cur; k++)
            {
                if (used[k])
                    continue;

                const Vertex *ahead = level_vertexes + edges[k].end;

                float delta = PolygonPseudoAngle(ahead->X - here->X, ahead->Y - here->Y) - reverse_angle;

                if (delta < 0.0f)
                    delta += 4.0f;

                if (delta < 0.000001f)
                    delta = 4.0f;

                if (delta < pick_delta)
                {
                    pick_delta = delta;
                    pick       = k;
                }
            }

            if (pick < 0)
                return false;

            used[pick] = 1;
            remaining--;

            loop.push_back(cur);

            prev = cur;
            cur  = edges[pick].end;

            if (loop.size() > edges.size() + 2)
                return false;
        }

        if (loop.size() >= 3)
            loops.push_back(loop);
    }

    return !loops.empty();
}

static bool PolygonTraceSelfReference(const Sector *sec, std::vector<std::vector<int>> &loops)
{
    std::vector<PolygonEdge> edges;

    for (int i = 0; i < sec->line_count; i++)
    {
        const Line *line = sec->lines[i];

        if (line->front_sector != sec || line->back_sector != sec)
            continue;

        int start = (int)(line->vertex_1 - level_vertexes);
        int end   = (int)(line->vertex_2 - level_vertexes);

        if (start == end)
            continue;

        PolygonEdge edge;

        edge.start = start;
        edge.end   = end;

        edges.push_back(edge);
    }

    if (edges.empty())
        return true;

    std::vector<uint8_t> dropped((size_t)edges.size(), 0);

    for (;;)
    {
        std::vector<int> degree;

        for (size_t i = 0; i < edges.size(); i++)
        {
            if (dropped[i])
                continue;

            if ((int)degree.size() <= edges[i].start)
                degree.resize((size_t)edges[i].start + 1, 0);

            if ((int)degree.size() <= edges[i].end)
                degree.resize((size_t)edges[i].end + 1, 0);

            degree[edges[i].start]++;
            degree[edges[i].end]++;
        }

        bool cut = false;

        for (size_t i = 0; i < edges.size(); i++)
        {
            if (dropped[i])
                continue;

            if (degree[edges[i].start] < 2 || degree[edges[i].end] < 2)
            {
                dropped[i] = 1;
                cut        = true;
            }
        }

        if (!cut)
            break;
    }

    std::vector<PolygonEdge> kept;

    for (size_t i = 0; i < edges.size(); i++)
    {
        if (!dropped[i])
            kept.push_back(edges[i]);
    }

    edges.swap(kept);

    if (edges.empty())
        return true;

    std::vector<PolygonIncidence> incident;

    incident.reserve(edges.size() * 2);

    for (size_t i = 0; i < edges.size(); i++)
    {
        PolygonIncidence entry;

        entry.edge   = (int)i;
        entry.vertex = edges[i].start;

        incident.push_back(entry);

        entry.vertex = edges[i].end;

        incident.push_back(entry);
    }

    std::sort(incident.begin(), incident.end(), PolygonIncidenceLess);

    std::vector<uint8_t> used((size_t)edges.size(), 0);

    for (size_t seed = 0; seed < edges.size(); seed++)
    {
        if (used[seed])
            continue;

        used[seed] = 1;

        int first = edges[seed].start;
        int cur   = edges[seed].end;

        std::vector<int> loop;

        loop.push_back(first);

        while (cur != first)
        {
            int pick = -1;

            for (int k = PolygonIncidenceLowerBound(incident, cur);
                 k < (int)incident.size() && incident[k].vertex == cur; k++)
            {
                if (used[incident[k].edge])
                    continue;

                pick = incident[k].edge;
                break;
            }

            if (pick < 0)
                return false;

            used[pick] = 1;

            loop.push_back(cur);

            cur = (edges[pick].start == cur) ? edges[pick].end : edges[pick].start;

            if (loop.size() > edges.size() + 2)
                return false;
        }

        if (loop.size() < 3)
            continue;

        if (PolygonLoopArea(loop) > 0.0)
            std::reverse(loop.begin(), loop.end());

        loops.push_back(loop);
    }

    return true;
}

static void PolygonTraceSector(int sector_index)
{
    Sector        *sec  = level_sectors + sector_index;
    SectorPolygon *poly = &sector_polygons[sector_index];

    poly->points.clear();
    poly->indices.clear();
    poly->loop_points.clear();
    poly->loop_starts.clear();
    poly->loop_count = 0;
    poly->hole_count = 0;
    poly->status     = kSectorPolygonOk;

    PolygonRefreshBounds(poly);

    if (sec->line_count == 0)
    {
        poly->status = kSectorPolygonNoEdges;
        return;
    }

    std::vector<PolygonEdge>      edges;
    std::vector<std::vector<int>> loops;

    if (!PolygonTraceLoops(sec, edges, loops))
    {
        poly->status =
            edges.empty() ? kSectorPolygonNoEdges : (loops.empty() ? kSectorPolygonDegenerate : kSectorPolygonOpenLoop);
        return;
    }

    for (size_t i = 0; i < loops.size(); i++)
        PolygonStoreLoop(poly, loops[i]);

    PolygonRefreshBounds(poly);
}

static void PolygonAttachSelfReferences(void)
{
    std::vector<std::vector<int>> loops;

    for (int i = 0; i < total_level_sectors; i++)
    {
        loops.clear();

        bool self_only = sector_polygons[i].loop_points.empty();

        if (!PolygonTraceSelfReference(level_sectors + i, loops))
        {
            polygon_self_reference_open++;

            if (sector_polygons[i].status == kSectorPolygonOk && sector_polygons[i].loop_points.empty())
                sector_polygons[i].status = kSectorPolygonOpenSelfReference;

            continue;
        }

        for (size_t k = 0; k < loops.size(); k++)
        {
            polygon_self_reference_loops++;

            float probe_x = 0.0f;
            float probe_y = 0.0f;

            PolygonLoopProbe(loops[k], PolygonLoopArea(loops[k]), &probe_x, &probe_y);

            if (PolygonSectorContains(&sector_polygons[i], probe_x, probe_y))
            {
                polygon_self_reference_covered++;
                continue;
            }

            PolygonStoreLoop(&sector_polygons[i], loops[k]);
            PolygonRefreshBounds(&sector_polygons[i]);

            if (sector_polygons[i].status == kSectorPolygonNoEdges)
                sector_polygons[i].status = kSectorPolygonOk;

            polygon_self_reference_owned.push_back(i);
            polygon_self_reference_only.push_back(self_only ? 1 : 0);
            polygon_self_reference_probe.push_back(probe_x);
            polygon_self_reference_probe.push_back(probe_y);
            polygon_self_reference_ring.push_back(loops[k]);
        }
    }
}

static void PolygonSubtractSelfReferences(void)
{
    for (size_t i = 0; i < polygon_self_reference_owned.size(); i++)
    {
        int owner = polygon_self_reference_owned[i];

        float probe_x = polygon_self_reference_probe[i * 2];
        float probe_y = polygon_self_reference_probe[i * 2 + 1];

        int container = SectorPolygonAtPoint(probe_x, probe_y, owner);

        if (container < 0)
        {
            polygon_self_reference_orphan++;
            continue;
        }

        if (polygon_self_reference_only[i] && !level_sectors[owner].deep_water_reference)
            level_sectors[owner].deep_water_reference = level_sectors + container;

        std::vector<int> ring = polygon_self_reference_ring[i];

        std::reverse(ring.begin(), ring.end());

        PolygonStoreLoop(&sector_polygons[container], ring);

        polygon_self_reference_containers.push_back(SectorPolygonContainment{owner, container});

        polygon_self_reference_attached++;
    }
}

static void PolygonFinishSector(int sector_index)
{
    SectorPolygon *poly = &sector_polygons[sector_index];

    if (poly->status != kSectorPolygonOk)
        return;

    size_t loop_count = (size_t)PolygonStoredLoops(poly);

    if (loop_count == 0)
    {
        poly->status = kSectorPolygonNoEdges;
        return;
    }

    poly->loop_count = (int)loop_count;

    std::vector<std::vector<int>> loops((size_t)loop_count);

    for (size_t i = 0; i < loop_count; i++)
        PolygonReadLoop(poly, (int)i, &loops[i]);

    std::vector<double> areas((size_t)loop_count, 0.0);
    std::vector<int>    depths((size_t)loop_count, 0);
    std::vector<int>    parents((size_t)loop_count, -1);

    for (size_t i = 0; i < loop_count; i++)
        areas[i] = PolygonLoopArea(loops[i]);

    for (size_t i = 0; i < loop_count; i++)
    {
        float probe_x = 0.0f;
        float probe_y = 0.0f;

        PolygonLoopProbe(loops[i], areas[i], &probe_x, &probe_y);

        for (size_t j = 0; j < loop_count; j++)
        {
            if (i == j)
                continue;

            if (!PolygonPointInLoop(loops[j], probe_x, probe_y))
                continue;

            depths[i]++;

            if (parents[i] < 0 || fabs(areas[(size_t)parents[i]]) > fabs(areas[j]))
                parents[i] = (int)j;
        }
    }

    size_t outer_loops = 0;

    for (size_t i = 0; i < loop_count; i++)
    {
        if ((depths[i] & 1) == 0)
            outer_loops++;
    }

    if (outer_loops == 0)
    {
        size_t widest = 0;

        for (size_t i = 1; i < loop_count; i++)
        {
            if (fabs(areas[i]) > fabs(areas[widest]))
                widest = i;
        }

        depths[widest]  = 0;
        parents[widest] = -1;
    }

    for (size_t i = 0; i < loop_count; i++)
    {
        bool hole_by_depth   = (depths[i] & 1) != 0;
        bool hole_by_winding = areas[i] > 0.0;

        if (hole_by_depth != hole_by_winding)
            polygon_winding_disagree++;

        if (hole_by_depth)
            poly->hole_count++;
    }

    vertex_point_serial++;

    std::vector<int> triangles;

    std::vector<std::vector<std::array<float, 2>>> rings;
    std::vector<int>                               ring_vertices;

    for (size_t i = 0; i < loop_count; i++)
    {
        if ((depths[i] & 1) != 0)
            continue;

        rings.clear();
        ring_vertices.clear();

        PolygonAppendRing(rings, ring_vertices, loops[i]);

        for (size_t j = 0; j < loop_count; j++)
        {
            if ((depths[j] & 1) == 0 || parents[j] != (int)i)
                continue;

            PolygonAppendRing(rings, ring_vertices, loops[j]);
        }

        std::vector<uint32_t> result = mapbox::earcut<uint32_t>(rings);

        if (result.empty())
        {
            poly->status = kSectorPolygonIncomplete;
            continue;
        }

        for (size_t k = 0; k + 2 < result.size(); k += 3)
        {
            triangles.push_back(ring_vertices[result[k]]);
            triangles.push_back(ring_vertices[result[k + 1]]);
            triangles.push_back(ring_vertices[result[k + 2]]);
        }
    }

    if (triangles.empty())
        poly->status = kSectorPolygonDegenerate;

    for (size_t i = 0; i + 2 < triangles.size(); i += 3)
    {
        const Vertex *a = level_vertexes + triangles[i];
        const Vertex *b = level_vertexes + triangles[i + 1];
        const Vertex *c = level_vertexes + triangles[i + 2];

        double doubled = (double)(b->X - a->X) * (double)(c->Y - a->Y) - (double)(c->X - a->X) * (double)(b->Y - a->Y);

        if (fabs(doubled) < 0.0001)
            continue;

        poly->indices.push_back((uint32_t)PolygonMapPoint(poly, triangles[i]));

        if (doubled < 0.0)
        {
            poly->indices.push_back((uint32_t)PolygonMapPoint(poly, triangles[i + 2]));
            poly->indices.push_back((uint32_t)PolygonMapPoint(poly, triangles[i + 1]));
        }
        else
        {
            poly->indices.push_back((uint32_t)PolygonMapPoint(poly, triangles[i + 1]));
            poly->indices.push_back((uint32_t)PolygonMapPoint(poly, triangles[i + 2]));
        }

        polygon_area_triangles += fabs(doubled) * 0.5;
    }

    for (size_t i = 0; i < loop_count; i++)
        polygon_area_loops -= areas[i];
}

void DestroySectorPolygons(void)
{
    sector_polygons.clear();
    vertex_point_map.clear();
    vertex_point_stamp.clear();

    polygon_self_reference_owned.clear();
    polygon_self_reference_only.clear();
    polygon_self_reference_probe.clear();
    polygon_self_reference_ring.clear();
    polygon_self_reference_containers.clear();

    DestroySectorGrid();

    vertex_point_serial   = 0;
    sector_polygons_built = false;
}

void BuildSectorPolygons(void)
{
    DestroySectorPolygons();

    if (total_level_sectors <= 0 || total_level_vertexes <= 0)
        return;

    uint64_t mark = GetMicroseconds();

    sector_polygons.resize((size_t)total_level_sectors);

    vertex_point_map.assign((size_t)total_level_vertexes, 0);
    vertex_point_stamp.assign((size_t)total_level_vertexes, 0);

    vertex_point_serial = 0;

    for (int i = 0; i < kSectorPolygonStatusTotal; i++)
        polygon_status_counts[i] = 0;

    polygon_sectors_traced          = 0;
    polygon_total_loops             = 0;
    polygon_total_holes             = 0;
    polygon_total_triangles         = 0;
    polygon_total_points            = 0;
    polygon_winding_disagree        = 0;
    polygon_area_mismatch_loops     = 0;
    polygon_area_triangles          = 0.0;
    polygon_area_loops              = 0.0;
    polygon_deep_water_sectors      = 0;
    polygon_oversized_sectors       = 0;
    polygon_gap_sectors             = 0;
    polygon_gap_edges               = 0;
    polygon_self_reference_loops    = 0;
    polygon_self_reference_attached = 0;
    polygon_self_reference_covered  = 0;
    polygon_self_reference_orphan   = 0;
    polygon_self_reference_open     = 0;

    for (int i = 0; i < total_level_sectors; i++)
    {
        level_sectors[i].deep_water_reference = nullptr;

        PolygonTraceSector(i);
    }

    PolygonAttachSelfReferences();

    BuildSectorGrid();

    PolygonSubtractSelfReferences();

    for (int i = 0; i < total_level_sectors; i++)
    {
        double before_triangles = polygon_area_triangles;
        double before_loops     = polygon_area_loops;

        PolygonFinishSector(i);

        const SectorPolygon *poly = &sector_polygons[i];

        polygon_status_counts[poly->status]++;

        if (poly->status != kSectorPolygonOk)
            continue;

        polygon_sectors_traced++;

        if (poly->indices.size() > kMaximumSectorPolygonVertices)
            polygon_oversized_sectors++;

        if (level_sectors[i].deep_water_reference)
            polygon_deep_water_sectors++;

        polygon_total_loops     += poly->loop_count;
        polygon_total_holes     += poly->hole_count;
        polygon_total_triangles += (int)poly->indices.size() / 3;
        polygon_total_points    += (int)poly->points.size();

        double sector_triangles = polygon_area_triangles - before_triangles;
        double sector_loops     = polygon_area_loops - before_loops;

        double scale = (sector_loops > 1.0) ? sector_loops : 1.0;

        if (fabs(sector_triangles - sector_loops) / scale > 0.001)
            polygon_area_mismatch_loops++;
    }

    polygon_self_reference_ring.clear();
    polygon_self_reference_probe.clear();
    polygon_self_reference_owned.clear();
    polygon_self_reference_only.clear();

    BuildPolygonCellEdges();

    polygon_build_microseconds = GetMicroseconds() - mark;

    sector_polygons_built = true;

    SectorPolygonReport();
}

bool SectorPolygonsBuilt(void)
{
    return sector_polygons_built;
}

const std::vector<SectorPolygonContainment> &SectorPolygonSelfReferenceContainers(void)
{
    return polygon_self_reference_containers;
}

bool SectorPolygonsEnabled(void)
{
    return sector_polygons_built;
}

const SectorPolygon *SectorPolygonForSector(int sector_index)
{
    if (!sector_polygons_built || sector_index < 0 || sector_index >= (int)sector_polygons.size())
        return nullptr;

    return &sector_polygons[sector_index];
}

void SectorPolygonReport(void)
{
    if (!sector_polygons_built)
    {
        LogPrint("Sector polygons: not built.\n");
        return;
    }

    LogPrint("Sector polygons: %d of %d sectors traced in %llu us\n", polygon_sectors_traced, total_level_sectors,
             (unsigned long long)polygon_build_microseconds);

    LogPrint("      %-32s %6d loops (%d holes), %d triangles, %d points\n", "geometry", polygon_total_loops,
             polygon_total_holes, polygon_total_triangles, polygon_total_points);

    for (int i = 0; i < kSectorPolygonStatusTotal; i++)
    {
        if (i == kSectorPolygonOk || polygon_status_counts[i] == 0)
            continue;

        LogPrint("      %-32s %6d\n", SectorPolygonStatusName(i), polygon_status_counts[i]);
    }

    LogPrint("      %-32s triangles %.1f, loops %.1f\n", "area", polygon_area_triangles, polygon_area_loops);

    LogPrint("      %-32s %6d vs loops, %d winding/containment disagreements\n", "sectors off by >0.1%",
             polygon_area_mismatch_loops, polygon_winding_disagree);

    LogPrint("      %-32s %6d deep-water, %d over the vertex cap\n", "special sectors",
             polygon_deep_water_sectors, polygon_oversized_sectors);

    LogPrint("      %-32s %6d sectors, %d edges added\n", "open boundaries closed", polygon_gap_sectors,
             polygon_gap_edges);

    LogPrint("      %-32s %6d loops, %d subtracted from a container, %d already covered, %d orphan, %d open\n",
             "self-referencing sectors", polygon_self_reference_loops, polygon_self_reference_attached,
             polygon_self_reference_covered, polygon_self_reference_orphan, polygon_self_reference_open);
}
