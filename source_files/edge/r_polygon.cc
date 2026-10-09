#include "r_polygon.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <algorithm>
#include <utility>
#include <vector>

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

class PolygonEarClipper
{
  public:
    void Triangulate(const std::vector<float> &coords, const std::vector<uint32_t> &ring_ends,
                     std::vector<uint32_t> *indices);
    void Release(void);

  private:
    struct Node
    {
        double   x;
        double   y;
        Node    *prev;
        Node    *next;
        int32_t  z;
        uint32_t index;
        bool     steiner;
        Node    *prev_z;
        Node    *next_z;
    };

    struct EarTriangle
    {
        const Node *a;
        const Node *b;
        const Node *c;
        double      min_x;
        double      min_y;
        double      max_x;
        double      max_y;
    };

    static constexpr int32_t kEdgesPerBlock = 16;
    static constexpr size_t  kNodeBlockSize = 512;

    Node   *NewNode(uint32_t index, double x, double y);
    Node   *InsertNode(uint32_t index, double x, double y, Node *last);
    void    RemoveNode(Node *p);
    Node   *LinkRing(const std::vector<float> &coords, size_t start, size_t end, bool clockwise);
    Node   *FilterPoints(Node *start, Node *end);
    void    ClipEars(Node *ear);
    bool    IsEar(const Node *ear) const;
    bool    IsEarHashed(const Node *ear) const;
    Node   *CureLocalIntersections(Node *start);
    void    SplitAndClip(Node *start);
    Node   *EliminateHoles(const std::vector<float> &coords, const std::vector<uint32_t> &ring_ends, Node *outer_node);
    Node   *EliminateHole(Node *hole, Node *outer_node);
    Node   *FindHoleBridge(Node *hole, Node *outer_node);
    void    BuildBlockIndex(size_t max_nodes, size_t hole_count);
    void    IndexSegment(Node *head, Node *stop);
    void    GrowBlock(const Node *head, const Node *tail);
    Node   *LiveBlockHead(size_t block);
    Node   *LiveBlockStop(size_t block);
    void    IndexCurve(Node *start);
    void    SortLinked(Node *list);
    int32_t ZOrder(double x, double y) const;
    Node   *SplitPolygon(Node *a, Node *b);

    static EarTriangle MakeEarTriangle(const Node *ear);
    static bool        NodeBlocksEar(const EarTriangle &triangle, const Node *p);
    static bool        HoleQueueLess(const Node *a, const Node *b);
    static bool        NodeZLess(const Node *a, const Node *b);
    static Node       *GetLeftmost(Node *start);
    static bool PointInTriangle(double ax, double ay, double bx, double by, double cx, double cy, double px, double py);
    static bool IsValidDiagonal(const Node *a, const Node *b);
    static double SignedArea(const Node *p, const Node *q, const Node *r);
    static bool   PointsEqual(const Node *p1, const Node *p2);
    static bool   SegmentsIntersect(const Node *p1, const Node *q1, const Node *p2, const Node *q2,
                                    bool include_boundary);
    static bool   OnSegment(const Node *p, const Node *q, const Node *r);
    static bool   IntersectsPolygon(const Node *a, const Node *b);
    static bool   LocallyInside(const Node *a, const Node *b);
    static bool   MiddleInside(const Node *a, const Node *b);
    static bool   SectorContainsSector(const Node *m, const Node *p);

    std::vector<uint32_t>         *indices_ = nullptr;
    std::vector<std::vector<Node>> node_blocks_;
    size_t                         node_block_index_ = 0;
    size_t                         node_block_used_  = 0;
    std::vector<Node *>            hole_queue_;
    std::vector<Node *>            sort_buffer_;
    std::vector<double>            block_bounds_;
    std::vector<Node *>            block_heads_;
    std::vector<Node *>            block_stops_;
    size_t                         block_count_  = 0;
    size_t                         vertex_count_ = 0;
    double                         min_x_        = 0.0;
    double                         min_y_        = 0.0;
    double                         inverse_size_ = 0.0;
    bool                           hashing_      = false;
    bool                           filtered_out_ = false;
    bool                           index_active_ = false;
};

void PolygonEarClipper::Triangulate(const std::vector<float> &coords, const std::vector<uint32_t> &ring_ends,
                                    std::vector<uint32_t> *indices)
{
    indices_ = indices;
    indices_->clear();

    vertex_count_     = 0;
    node_block_index_ = 0;
    node_block_used_  = 0;

    if (ring_ends.empty())
        return;

    int    threshold  = 80;
    size_t ring_start = 0;

    for (size_t i = 0; threshold >= 0 && i < ring_ends.size(); i++)
    {
        threshold -= (int)(ring_ends[i] - ring_start);
        ring_start = ring_ends[i];
    }

    indices_->reserve((size_t)ring_ends.back() + ring_ends[0]);

    Node *outer_node = LinkRing(coords, 0, ring_ends[0], true);

    if (outer_node == nullptr || outer_node->prev == outer_node->next)
        return;

    if (ring_ends.size() > 1)
        outer_node = EliminateHoles(coords, ring_ends, outer_node);

    hashing_ = threshold < 0;

    if (hashing_)
    {
        Node  *p     = outer_node->next;
        double max_x = outer_node->x;
        double max_y = outer_node->y;

        min_x_ = outer_node->x;
        min_y_ = outer_node->y;

        do
        {
            min_x_ = std::min(min_x_, p->x);
            min_y_ = std::min(min_y_, p->y);
            max_x  = std::max(max_x, p->x);
            max_y  = std::max(max_y, p->y);
            p      = p->next;
        } while (p != outer_node);

        inverse_size_ = std::max(max_x - min_x_, max_y - min_y_);
        inverse_size_ = epi::AlmostEquals(inverse_size_, 0.0) ? 0.0 : 32767.0 / inverse_size_;
    }

    ClipEars(outer_node);

    hole_queue_.clear();
}

void PolygonEarClipper::Release(void)
{
    node_blocks_.clear();
    node_blocks_.shrink_to_fit();
    hole_queue_.clear();
    hole_queue_.shrink_to_fit();
    sort_buffer_.clear();
    sort_buffer_.shrink_to_fit();
    block_bounds_.clear();
    block_bounds_.shrink_to_fit();
    block_heads_.clear();
    block_heads_.shrink_to_fit();
    block_stops_.clear();
    block_stops_.shrink_to_fit();

    indices_          = nullptr;
    node_block_index_ = 0;
    node_block_used_  = 0;
    block_count_      = 0;
}

PolygonEarClipper::Node *PolygonEarClipper::NewNode(uint32_t index, double x, double y)
{
    if (node_block_used_ == kNodeBlockSize)
    {
        node_block_index_++;
        node_block_used_ = 0;
    }

    if (node_block_index_ == node_blocks_.size())
        node_blocks_.emplace_back(kNodeBlockSize);

    Node *node = &node_blocks_[node_block_index_][node_block_used_++];

    node->x       = x;
    node->y       = y;
    node->prev    = nullptr;
    node->next    = nullptr;
    node->z       = 0;
    node->index   = index;
    node->steiner = false;
    node->prev_z  = nullptr;
    node->next_z  = nullptr;

    return node;
}

PolygonEarClipper::Node *PolygonEarClipper::InsertNode(uint32_t index, double x, double y, Node *last)
{
    Node *p = NewNode(index, x, y);

    if (last == nullptr)
    {
        p->prev = p;
        p->next = p;
    }
    else
    {
        p->next          = last->next;
        p->prev          = last;
        last->next->prev = p;
        last->next       = p;
    }

    return p;
}

void PolygonEarClipper::RemoveNode(Node *p)
{
    p->next->prev = p->prev;
    p->prev->next = p->next;

    if (p->prev_z != nullptr)
        p->prev_z->next_z = p->next_z;

    if (p->next_z != nullptr)
        p->next_z->prev_z = p->prev_z;

    if (index_active_)
        GrowBlock(p->prev, p->next);
}

PolygonEarClipper::Node *PolygonEarClipper::LinkRing(const std::vector<float> &coords, size_t start, size_t end,
                                                     bool clockwise)
{
    size_t length = end - start;
    double sum    = 0.0;

    for (size_t i = 0, j = length > 0 ? length - 1 : 0; i < length; j = i++)
    {
        double x1 = coords[(start + i) * 2];
        double y1 = coords[(start + i) * 2 + 1];
        double x2 = coords[(start + j) * 2];
        double y2 = coords[(start + j) * 2 + 1];

        sum += (x2 - x1) * (y1 + y2);
    }

    Node *last = nullptr;

    if (clockwise == (sum > 0.0))
    {
        for (size_t i = start; i < end; i++)
            last = InsertNode((uint32_t)i, coords[i * 2], coords[i * 2 + 1], last);
    }
    else
    {
        for (size_t i = end; i-- > start;)
            last = InsertNode((uint32_t)i, coords[i * 2], coords[i * 2 + 1], last);
    }

    if (last != nullptr && PointsEqual(last, last->next))
    {
        RemoveNode(last);
        last = last->next;
    }

    vertex_count_ += length;

    return last;
}

PolygonEarClipper::Node *PolygonEarClipper::FilterPoints(Node *start, Node *end)
{
    if (start == nullptr)
        return start;

    bool full = end == nullptr;

    if (full)
        end = start;

    Node *p = start;
    bool  again;

    do
    {
        again = false;

        if (p != p->next && !p->steiner &&
            (PointsEqual(p, p->next) || epi::AlmostEquals(SignedArea(p->prev, p, p->next), 0.0)))
        {
            if (full || p == end)
                end = p->prev;

            filtered_out_ = true;
            RemoveNode(p);
            p     = p->prev;
            again = true;
        }
        else if (full || p != end)
        {
            p     = p->next;
            again = !full;
        }
    } while (again || p != end);

    return end;
}

void PolygonEarClipper::ClipEars(Node *ear)
{
    if (ear == nullptr)
        return;

    if (hashing_)
        IndexCurve(ear);

    Node *stop  = ear;
    bool  cured = false;

    while (ear->prev != ear->next)
    {
        Node *prev = ear->prev;
        Node *next = ear->next;

        if (SignedArea(prev, ear, next) < 0.0 && (hashing_ ? IsEarHashed(ear) : IsEar(ear)))
        {
            indices_->push_back(prev->index);
            indices_->push_back(ear->index);
            indices_->push_back(next->index);

            RemoveNode(ear);

            ear  = next;
            stop = next;

            continue;
        }

        ear = next;

        if (ear == stop)
        {
            filtered_out_ = false;
            ear           = FilterPoints(ear, nullptr);

            if (filtered_out_)
            {
                stop = ear;
                continue;
            }

            if (!cured)
            {
                ear   = CureLocalIntersections(ear);
                stop  = ear;
                cured = true;
                continue;
            }

            SplitAndClip(ear);
            break;
        }
    }
}

PolygonEarClipper::EarTriangle PolygonEarClipper::MakeEarTriangle(const Node *ear)
{
    EarTriangle triangle;

    triangle.a     = ear->prev;
    triangle.b     = ear;
    triangle.c     = ear->next;
    triangle.min_x = std::min(triangle.a->x, std::min(triangle.b->x, triangle.c->x));
    triangle.min_y = std::min(triangle.a->y, std::min(triangle.b->y, triangle.c->y));
    triangle.max_x = std::max(triangle.a->x, std::max(triangle.b->x, triangle.c->x));
    triangle.max_y = std::max(triangle.a->y, std::max(triangle.b->y, triangle.c->y));

    return triangle;
}

bool PolygonEarClipper::NodeBlocksEar(const EarTriangle &triangle, const Node *p)
{
    if (p->x < triangle.min_x || p->x > triangle.max_x || p->y < triangle.min_y || p->y > triangle.max_y)
        return false;

    if (PointsEqual(triangle.a, p))
        return false;

    return PointInTriangle(triangle.a->x, triangle.a->y, triangle.b->x, triangle.b->y, triangle.c->x, triangle.c->y,
                           p->x, p->y) &&
           SignedArea(p->prev, p, p->next) >= 0.0;
}

bool PolygonEarClipper::IsEar(const Node *ear) const
{
    EarTriangle triangle = MakeEarTriangle(ear);

    for (const Node *p = ear->next->next; p != ear->prev; p = p->next)
    {
        if (NodeBlocksEar(triangle, p))
            return false;
    }

    return true;
}

bool PolygonEarClipper::IsEarHashed(const Node *ear) const
{
    EarTriangle triangle = MakeEarTriangle(ear);

    int32_t min_z = ZOrder(triangle.min_x, triangle.min_y);
    int32_t max_z = ZOrder(triangle.max_x, triangle.max_y);

    for (const Node *p = ear->next_z; p != nullptr && p->z <= max_z; p = p->next_z)
    {
        if (p != ear->next && NodeBlocksEar(triangle, p))
            return false;
    }

    for (const Node *p = ear->prev_z; p != nullptr && p->z >= min_z; p = p->prev_z)
    {
        if (p != ear->next && NodeBlocksEar(triangle, p))
            return false;
    }

    return true;
}

PolygonEarClipper::Node *PolygonEarClipper::CureLocalIntersections(Node *start)
{
    Node *p     = start;
    bool  cured = false;

    do
    {
        Node *a = p->prev;
        Node *b = p->next->next;

        if (SegmentsIntersect(a, p, p->next, b, false) && LocallyInside(a, b) && LocallyInside(b, a))
        {
            indices_->push_back(a->index);
            indices_->push_back(p->index);
            indices_->push_back(b->index);

            RemoveNode(p);
            RemoveNode(p->next);

            p     = b;
            start = b;
            cured = true;
        }

        p = p->next;
    } while (p != start);

    return cured ? FilterPoints(p, nullptr) : p;
}

void PolygonEarClipper::SplitAndClip(Node *start)
{
    Node *a = start;

    do
    {
        for (Node *b = a->next->next; b != a->prev; b = b->next)
        {
            if (a->index != b->index && IsValidDiagonal(a, b))
            {
                Node *c = SplitPolygon(a, b);

                a = FilterPoints(a, a->next);
                c = FilterPoints(c, c->next);

                ClipEars(a);
                ClipEars(c);
                return;
            }
        }

        a = a->next;
    } while (a != start);
}

bool PolygonEarClipper::HoleQueueLess(const Node *a, const Node *b)
{
    if (!epi::AlmostEquals(a->x, b->x))
        return a->x < b->x;

    if (!epi::AlmostEquals(a->y, b->y))
        return a->y < b->y;

    double a_dx = a->next->x - a->x;
    double a_dy = a->next->y - a->y;
    double b_dx = b->next->x - b->x;
    double b_dy = b->next->y - b->y;

    bool a_degenerate = epi::AlmostEquals(a_dx, 0.0) && epi::AlmostEquals(a_dy, 0.0);
    bool b_degenerate = epi::AlmostEquals(b_dx, 0.0) && epi::AlmostEquals(b_dy, 0.0);

    if (a_degenerate != b_degenerate)
        return a_degenerate;

    return a_dy * b_dx < b_dy * a_dx;
}

PolygonEarClipper::Node *PolygonEarClipper::EliminateHoles(const std::vector<float>    &coords,
                                                           const std::vector<uint32_t> &ring_ends, Node *outer_node)
{
    hole_queue_.clear();

    for (size_t i = 1; i < ring_ends.size(); i++)
    {
        Node *list = LinkRing(coords, ring_ends[i - 1], ring_ends[i], false);

        if (list != nullptr)
        {
            if (list == list->next)
                list->steiner = true;

            hole_queue_.push_back(GetLeftmost(list));
        }
    }

    std::sort(hole_queue_.begin(), hole_queue_.end(), HoleQueueLess);

    BuildBlockIndex(vertex_count_, hole_queue_.size());
    IndexSegment(outer_node, outer_node);

    index_active_ = true;

    for (size_t i = 0; i < hole_queue_.size(); i++)
        outer_node = EliminateHole(hole_queue_[i], outer_node);

    index_active_ = false;

    return FilterPoints(outer_node, nullptr);
}

PolygonEarClipper::Node *PolygonEarClipper::EliminateHole(Node *hole, Node *outer_node)
{
    Node *bridge = FindHoleBridge(hole, outer_node);

    if (bridge == nullptr)
        return outer_node;

    Node *bridge_reverse = SplitPolygon(bridge, hole);

    IndexSegment(bridge, bridge_reverse->next->next);

    FilterPoints(bridge_reverse, bridge_reverse->next);

    return FilterPoints(bridge, bridge->next);
}

PolygonEarClipper::Node *PolygonEarClipper::FindHoleBridge(Node *hole, Node *outer_node)
{
    double hx = hole->x;
    double hy = hole->y;
    double qx = -DBL_MAX;
    Node  *m  = nullptr;

    if (PointsEqual(hole, outer_node))
        return outer_node;

    for (size_t block = 0; block < block_count_; block++)
    {
        const double *bounds = &block_bounds_[block * 4];

        if (hy < bounds[1] || hy > bounds[3] || bounds[0] > hx || bounds[2] <= qx)
            continue;

        const Node *stop = LiveBlockStop(block);
        Node       *p    = LiveBlockHead(block);

        do
        {
            if (p->prev->next == p)
            {
                if (PointsEqual(hole, p->next))
                    return p->next;

                if (hy <= p->y && hy >= p->next->y && !epi::AlmostEquals(p->next->y, p->y))
                {
                    double x = p->x + (hy - p->y) * (p->next->x - p->x) / (p->next->y - p->y);

                    if (x <= hx && x > qx)
                    {
                        qx = x;
                        m  = p->x < p->next->x ? p : p->next;

                        if (epi::AlmostEquals(x, hx))
                            return m;
                    }
                }
            }

            p = p->next;
        } while (p != stop);
    }

    if (m == nullptr)
        return nullptr;

    double mx           = m->x;
    double my           = m->y;
    double triangle_min = std::min(hy, my);
    double triangle_max = std::max(hy, my);
    double tangent_min  = DBL_MAX;

    for (size_t block = 0; block < block_count_; block++)
    {
        const double *bounds = &block_bounds_[block * 4];

        if (bounds[2] < mx || bounds[0] > hx || bounds[3] < triangle_min || bounds[1] > triangle_max)
            continue;

        const Node *stop = LiveBlockStop(block);
        Node       *p    = LiveBlockHead(block);

        do
        {
            if (p->prev->next == p && hx >= p->x && p->x >= mx && !epi::AlmostEquals(hx, p->x) &&
                PointInTriangle(hy < my ? hx : qx, hy, mx, my, hy < my ? qx : hx, hy, p->x, p->y))
            {
                double tangent = fabs(hy - p->y) / (hx - p->x);

                bool touches_edge = epi::AlmostEquals(p->y, hy) && epi::AlmostEquals(p->next->y, hy) && p->next->x > hx;

                if ((LocallyInside(p, hole) || touches_edge) &&
                    (tangent < tangent_min ||
                     (epi::AlmostEquals(tangent, tangent_min) &&
                      (p->x > m->x || (epi::AlmostEquals(p->x, m->x) && SectorContainsSector(m, p))))))
                {
                    m           = p;
                    tangent_min = tangent;
                }
            }

            p = p->next;
        } while (p != stop);
    }

    return m;
}

void PolygonEarClipper::BuildBlockIndex(size_t max_nodes, size_t hole_count)
{
    size_t max_blocks = (max_nodes + 2 * hole_count + kEdgesPerBlock - 1) / kEdgesPerBlock + hole_count + 2;

    if (block_bounds_.size() < max_blocks * 4)
        block_bounds_.resize(max_blocks * 4);

    if (block_heads_.size() < max_blocks)
    {
        block_heads_.resize(max_blocks);
        block_stops_.resize(max_blocks);
    }

    block_count_ = 0;
}

void PolygonEarClipper::IndexSegment(Node *head, Node *stop)
{
    Node *p = head;

    do
    {
        size_t  block       = block_count_++;
        double  block_min_x = DBL_MAX;
        double  block_min_y = DBL_MAX;
        double  block_max_x = -DBL_MAX;
        double  block_max_y = -DBL_MAX;
        int32_t edge_count  = 0;
        block_heads_[block] = p;

        do
        {
            Node *c = p->next;

            p->z = (int32_t)block;

            block_min_x = std::min(block_min_x, std::min(p->x, c->x));
            block_min_y = std::min(block_min_y, std::min(p->y, c->y));
            block_max_x = std::max(block_max_x, std::max(p->x, c->x));
            block_max_y = std::max(block_max_y, std::max(p->y, c->y));

            p = c;
        } while (++edge_count < kEdgesPerBlock && p != stop);

        block_stops_[block] = p;

        double *bounds = &block_bounds_[block * 4];

        bounds[0] = block_min_x;
        bounds[1] = block_min_y;
        bounds[2] = block_max_x;
        bounds[3] = block_max_y;
    } while (p != stop);
}

void PolygonEarClipper::GrowBlock(const Node *head, const Node *tail)
{
    double *bounds = &block_bounds_[(size_t)head->z * 4];

    bounds[0] = std::min(bounds[0], tail->x);
    bounds[1] = std::min(bounds[1], tail->y);
    bounds[2] = std::max(bounds[2], tail->x);
    bounds[3] = std::max(bounds[3], tail->y);
}

PolygonEarClipper::Node *PolygonEarClipper::LiveBlockHead(size_t block)
{
    Node *head = block_heads_[block];

    while (head->prev->next != head)
        head = head->next;

    block_heads_[block] = head;

    return head;
}

PolygonEarClipper::Node *PolygonEarClipper::LiveBlockStop(size_t block)
{
    Node *stop = block_stops_[block];

    while (stop->prev->next != stop)
        stop = stop->next;

    block_stops_[block] = stop;

    return stop;
}

bool PolygonEarClipper::SectorContainsSector(const Node *m, const Node *p)
{
    return SignedArea(m->prev, m, p->prev) < 0.0 && SignedArea(p->next, m, m->next) < 0.0;
}

void PolygonEarClipper::IndexCurve(Node *start)
{
    Node *p = start;

    do
    {
        p->z      = ZOrder(p->x, p->y);
        p->prev_z = p->prev;
        p->next_z = p->next;
        p         = p->next;
    } while (p != start);

    p->prev_z->next_z = nullptr;
    p->prev_z         = nullptr;

    SortLinked(p);
}

bool PolygonEarClipper::NodeZLess(const Node *a, const Node *b)
{
    return a->z < b->z;
}

void PolygonEarClipper::SortLinked(Node *list)
{
    sort_buffer_.clear();

    for (Node *p = list; p != nullptr; p = p->next_z)
        sort_buffer_.push_back(p);

    std::sort(sort_buffer_.begin(), sort_buffer_.end(), NodeZLess);

    Node *prev = nullptr;

    for (size_t i = 0; i < sort_buffer_.size(); i++)
    {
        Node *p = sort_buffer_[i];

        p->prev_z = prev;

        if (prev != nullptr)
            prev->next_z = p;

        prev = p;
    }

    prev->next_z = nullptr;
}

int32_t PolygonEarClipper::ZOrder(double x, double y) const
{
    int32_t z_x = (int32_t)((x - min_x_) * inverse_size_);
    int32_t z_y = (int32_t)((y - min_y_) * inverse_size_);

    z_x = (z_x | (z_x << 8)) & 0x00FF00FF;
    z_x = (z_x | (z_x << 4)) & 0x0F0F0F0F;
    z_x = (z_x | (z_x << 2)) & 0x33333333;
    z_x = (z_x | (z_x << 1)) & 0x55555555;

    z_y = (z_y | (z_y << 8)) & 0x00FF00FF;
    z_y = (z_y | (z_y << 4)) & 0x0F0F0F0F;
    z_y = (z_y | (z_y << 2)) & 0x33333333;
    z_y = (z_y | (z_y << 1)) & 0x55555555;

    return z_x | (z_y << 1);
}

PolygonEarClipper::Node *PolygonEarClipper::GetLeftmost(Node *start)
{
    Node *p        = start;
    Node *leftmost = start;

    do
    {
        if (p->x < leftmost->x || (epi::AlmostEquals(p->x, leftmost->x) && p->y < leftmost->y))
            leftmost = p;

        p = p->next;
    } while (p != start);

    return leftmost;
}

bool PolygonEarClipper::PointInTriangle(double ax, double ay, double bx, double by, double cx, double cy, double px,
                                        double py)
{
    return (cx - px) * (ay - py) >= (ax - px) * (cy - py) && (ax - px) * (by - py) >= (bx - px) * (ay - py) &&
           (bx - px) * (cy - py) >= (cx - px) * (by - py);
}

bool PolygonEarClipper::IsValidDiagonal(const Node *a, const Node *b)
{
    bool zero_length =
        PointsEqual(a, b) && SignedArea(a->prev, a, a->next) > 0.0 && SignedArea(b->prev, b, b->next) > 0.0;

    bool locally_visible = LocallyInside(a, b) && LocallyInside(b, a) &&
                           (!epi::AlmostEquals(SignedArea(a->prev, a, b->prev), 0.0) ||
                            !epi::AlmostEquals(SignedArea(a, b->prev, b), 0.0));

    return a->next->index != b->index && (zero_length || locally_visible) && !IntersectsPolygon(a, b) &&
           (zero_length || MiddleInside(a, b));
}

double PolygonEarClipper::SignedArea(const Node *p, const Node *q, const Node *r)
{
    return (q->y - p->y) * (r->x - q->x) - (q->x - p->x) * (r->y - q->y);
}

bool PolygonEarClipper::PointsEqual(const Node *p1, const Node *p2)
{
    return epi::AlmostEquals(p1->x, p2->x) && epi::AlmostEquals(p1->y, p2->y);
}

bool PolygonEarClipper::SegmentsIntersect(const Node *p1, const Node *q1, const Node *p2, const Node *q2,
                                          bool include_boundary)
{
    double o1 = SignedArea(p1, q1, p2);
    double o2 = SignedArea(p1, q1, q2);
    double o3 = SignedArea(p2, q2, p1);
    double o4 = SignedArea(p2, q2, q1);

    if (((o1 > 0.0 && o2 < 0.0) || (o1 < 0.0 && o2 > 0.0)) && ((o3 > 0.0 && o4 < 0.0) || (o3 < 0.0 && o4 > 0.0)))
        return true;

    if (!include_boundary)
        return false;

    if (epi::AlmostEquals(o1, 0.0) && OnSegment(p1, p2, q1))
        return true;

    if (epi::AlmostEquals(o2, 0.0) && OnSegment(p1, q2, q1))
        return true;

    if (epi::AlmostEquals(o3, 0.0) && OnSegment(p2, p1, q2))
        return true;

    if (epi::AlmostEquals(o4, 0.0) && OnSegment(p2, q1, q2))
        return true;

    return false;
}

bool PolygonEarClipper::OnSegment(const Node *p, const Node *q, const Node *r)
{
    return q->x <= std::max(p->x, r->x) && q->x >= std::min(p->x, r->x) && q->y <= std::max(p->y, r->y) &&
           q->y >= std::min(p->y, r->y);
}

bool PolygonEarClipper::IntersectsPolygon(const Node *a, const Node *b)
{
    double diagonal_min_x = std::min(a->x, b->x);
    double diagonal_max_x = std::max(a->x, b->x);
    double diagonal_min_y = std::min(a->y, b->y);
    double diagonal_max_y = std::max(a->y, b->y);

    const Node *p = a;

    do
    {
        const Node *n = p->next;

        bool disjoint =
            (p->x > diagonal_max_x && n->x > diagonal_max_x) || (p->x < diagonal_min_x && n->x < diagonal_min_x) ||
            (p->y > diagonal_max_y && n->y > diagonal_max_y) || (p->y < diagonal_min_y && n->y < diagonal_min_y);

        if (!disjoint && p->index != a->index && n->index != a->index && p->index != b->index && n->index != b->index &&
            SegmentsIntersect(p, n, a, b, true))
            return true;

        p = n;
    } while (p != a);

    return false;
}

bool PolygonEarClipper::LocallyInside(const Node *a, const Node *b)
{
    if (SignedArea(a->prev, a, a->next) < 0.0)
        return SignedArea(a, b, a->next) >= 0.0 && SignedArea(a, a->prev, b) >= 0.0;

    return SignedArea(a, b, a->prev) < 0.0 || SignedArea(a, a->next, b) < 0.0;
}

bool PolygonEarClipper::MiddleInside(const Node *a, const Node *b)
{
    const Node *p      = a;
    bool        inside = false;
    double      px     = (a->x + b->x) / 2.0;
    double      py     = (a->y + b->y) / 2.0;

    do
    {
        const Node *n = p->next;

        if ((p->y > py) != (n->y > py) && px < (n->x - p->x) * (py - p->y) / (n->y - p->y) + p->x)
            inside = !inside;

        p = n;
    } while (p != a);

    return inside;
}

PolygonEarClipper::Node *PolygonEarClipper::SplitPolygon(Node *a, Node *b)
{
    Node *a2 = NewNode(a->index, a->x, a->y);
    Node *b2 = NewNode(b->index, b->x, b->y);
    Node *an = a->next;
    Node *bp = b->prev;

    a->next  = b;
    b->prev  = a;
    a2->next = an;
    an->prev = a2;
    b2->next = a2;
    a2->prev = b2;
    bp->next = b2;
    b2->prev = bp;

    return b2;
}

static PolygonEarClipper polygon_ear_clipper;

static void PolygonAppendRing(std::vector<float> &ring_coords, std::vector<uint32_t> &ring_ends,
                              std::vector<int> &ring_vertices, const std::vector<int> &loop)
{
    for (size_t i = 0; i < loop.size(); i++)
    {
        const Vertex *point = level_vertexes + loop[i];

        ring_coords.push_back(point->X);
        ring_coords.push_back(point->Y);

        ring_vertices.push_back(loop[i]);
    }

    ring_ends.push_back((uint32_t)ring_vertices.size());
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

    std::vector<float>    ring_coords;
    std::vector<uint32_t> ring_ends;
    std::vector<int>      ring_vertices;
    std::vector<uint32_t> result;

    for (size_t i = 0; i < loop_count; i++)
    {
        if ((depths[i] & 1) != 0)
            continue;

        ring_coords.clear();
        ring_ends.clear();
        ring_vertices.clear();

        PolygonAppendRing(ring_coords, ring_ends, ring_vertices, loops[i]);

        for (size_t j = 0; j < loop_count; j++)
        {
            if ((depths[j] & 1) == 0 || parents[j] != (int)i)
                continue;

            PolygonAppendRing(ring_coords, ring_ends, ring_vertices, loops[j]);
        }

        polygon_ear_clipper.Triangulate(ring_coords, ring_ends, &result);

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

    polygon_ear_clipper.Release();

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
