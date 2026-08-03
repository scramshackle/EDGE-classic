//------------------------------------------------------------------------
//
//  AJ-BSP  Copyright (C) 2000-2023  Andrew Apted, et al
//          Copyright (C) 1994-1998  Colin Reed
//          Copyright (C) 1997-1998  Lee Killough
//
//  Originally based on the program 'BSP', version 2.3.
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 3
//  of the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//------------------------------------------------------------------------

#include <limits.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "HandmadeMath.h"
#include "bsp.h"
#include "i_thread.h"
#include "dm_defs.h"
#include "epi.h"
#include "epi_doomdefs.h"
#include "epi_math.h"
#include "epi_file.h"
#include "epi_filesystem.h"
#include "epi_simd.h"
#include "epi_str_compare.h"
#include "epi_str_util.h"
#include "miniz.h"
#include "r_defs.h"
#include "r_misc.h"
#include "r_state.h"

#define AJBSP_DEBUG_LOAD      0
#define AJBSP_DEBUG_BSP       0
#define AJBSP_DEBUG_WALLTIPS  0
#define AJBSP_DEBUG_OVERLAPS  0
#define AJBSP_DEBUG_PICKNODE  0
#define AJBSP_DEBUG_SPLIT     0
#define AJBSP_DEBUG_CUTLIST   0
#define AJBSP_DEBUG_BUILDER   0
#define AJBSP_DEBUG_SORTER    0
#define AJBSP_DEBUG_SUBSEC    0

enum BuildResult
{
    kBuildOK = 0,
    kBuildError
};

namespace ajbsp
{

//------------------------------------------------------------------------
// TYPE DEFINITIONS
//------------------------------------------------------------------------

class Node;
class QuadTree;

struct WallTip
{
    WallTip *next;
    WallTip *previous;
    double   angle;
    bool     open_left;
    bool     open_right;
};

class Vertex
{
  public:
    double x_, y_;
    int    index_;
    bool   is_new_;
    bool   is_used_;

    Vertex  *overlap_;
    WallTip *tip_set_;

  public:
    bool CheckOpen(double dx, double dy) const;
    void AddWallTip(double dx, double dy, bool open_left, bool open_right);
    bool Overlaps(const Vertex *other) const;
};

struct Sector
{
    int index;
};

struct Sidedef
{
    Sector *sector;
    int     index;
};

struct Linedef
{
    Linedef *next;

    Vertex *start;
    Vertex *end;

    Sidedef *right;
    Sidedef *left;

    int  type;
    bool two_sided;
    bool is_precious;
    bool zero_length;
    bool self_referencing;

    Linedef *overlap;

    int index;
};

class Seg
{
  public:
    Seg *next_;

    Vertex *start_;
    Vertex *end_;

    Linedef *linedef_;

    int side_;

    Seg *partner_;

    int  index_;
    bool is_degenerate_;

    QuadTree *quad_;

    double psx_, psy_;
    double pex_, pey_;
    double pdx_, pdy_;

    double p_length_;
    double p_para_;
    double p_perp_;

    Linedef *source_line_;

    double cmp_angle_;

  public:
    void Recompute();
    int  PointOnLineSide(double x, double y) const;

    inline double ParallelDistance(double x, double y) const
    {
        return (x * pdx_ + y * pdy_ + p_para_) / p_length_;
    }

    inline double PerpendicularDistance(double x, double y) const
    {
        return (x * pdy_ - y * pdx_ + p_perp_) / p_length_;
    }
};

class Subsector
{
  public:
    Seg *seg_list_;
    int  seg_count_;
    int  index_;

    double mid_x_;
    double mid_y_;

  public:
    void AddToTail(Seg *seg);
    void DetermineMiddle();
    void ClockwiseOrder();
    void RenumberSegs(int &cur_seg_index);
    void SanityCheckClosed() const;
    void SanityCheckHasRealSeg() const;
};

struct BoundingBox
{
    int minimum_x, minimum_y;
    int maximum_x, maximum_y;
};

struct Child
{
    Node      *node;
    Subsector *subsec;
    BoundingBox bounds;
};

class Node
{
  public:
    double x_, y_;
    double dx_, dy_;

    Child r_;
    Child l_;

    int index_;

  public:
    void SetPartition(const Seg *part);
};

class QuadTree
{
  public:
    int x1_, y1_;
    int x2_, y2_;

    QuadTree *subs_[2];

    int  real_num_;
    int  mini_num_;
    Seg *list_;

  public:
    QuadTree(int _x1, int _y1, int _x2, int _y2);
    ~QuadTree();

    void AddSeg(Seg *seg);
    void AddList(Seg *list);

    inline bool Empty() const
    {
        return (real_num_ + mini_num_) == 0;
    }

    void ConvertToList(Seg **list);

    int OnLineSide(const Seg *part) const;
};

struct Intersection
{
    Intersection *next;
    Intersection *prev;

    Vertex *vertex;

    double along_dist;
    bool   self_ref;
    bool   open_before;
    bool   open_after;
};

//------------------------------------------------------------------------
// CONSTANTS
//------------------------------------------------------------------------

constexpr float  kIffySegLength = 4.0f;
constexpr double kEpsilon       = (1.0 / 1024.0);
constexpr uint8_t kSplitCostDefault = 11;

static constexpr uint8_t kPreciousCostMultiplier = 100;
static constexpr uint8_t kSegFastModeThreshold   = 200;

inline void ListAddSeg(Seg **list_ptr, Seg *seg)
{
    seg->next_ = *list_ptr;
    *list_ptr  = seg;
}

//------------------------------------------------------------------------
// NODE EVALUATION HELPER
//------------------------------------------------------------------------

class EvalInfo
{
  public:
    double cost;
    int    splits;
    int    iffy;
    int    near_miss;

    int real_left;
    int real_right;
    int mini_left;
    int mini_right;

  public:
    void BumpLeft(const Linedef *linedef)
    {
        if (linedef != nullptr)
            real_left++;
        else
            mini_left++;
    }

    void BumpRight(const Linedef *linedef)
    {
        if (linedef != nullptr)
            real_right++;
        else
            mini_right++;
    }
};

//------------------------------------------------------------------------
// DATA
//------------------------------------------------------------------------

std::vector<Vertex *>  level_vertices;
std::vector<Linedef *> level_linedefs;
std::vector<Sidedef *> level_sidedefs;
std::vector<Sector *>  level_sectors;

std::vector<Seg *>       level_segs;
std::vector<Subsector *> level_subsecs;
std::vector<Node *>      level_nodes;
std::vector<WallTip *>   level_walltips;

int num_old_vert   = 0;
int num_new_vert   = 0;
int num_real_lines = 0;

std::vector<Intersection *> alloc_cuts;

static SystemMutex *alloc_mutex = nullptr;

//------------------------------------------------------------------------
// UTILITY
//------------------------------------------------------------------------

void *UtilCalloc(int size)
{
    void *ret = calloc(1, size);

    if (!ret)
        FatalError("AJBSP: Out of memory (cannot allocate %d bytes)\n", size);

    return ret;
}

void UtilFree(void *data)
{
    if (data == nullptr)
        FatalError("AJBSP: Trying to free a nullptr pointer\n");

    free(data);
}

double ComputeAngle(double dx, double dy)
{
    double angle;

    if (epi::AlmostEquals(dx, 0.0))
        return (dy > 0) ? 90.0 : 270.0;

    angle = atan2((double)dy, (double)dx) * 180.0 / HMM_PI;

    if (angle < 0)
        angle += 360.0;

    return angle;
}

//------------------------------------------------------------------------
// ALLOCATIONS
//------------------------------------------------------------------------

Vertex *NewVertex()
{
    Vertex *V = (Vertex *)UtilCalloc(sizeof(Vertex));
    LockSystemMutex(alloc_mutex);
    V->index_ = (int)level_vertices.size();
    level_vertices.push_back(V);
    UnlockSystemMutex(alloc_mutex);
    return V;
}

Linedef *NewLinedef()
{
    Linedef *L = (Linedef *)UtilCalloc(sizeof(Linedef));
    L->index   = (int)level_linedefs.size();
    level_linedefs.push_back(L);
    return L;
}

Sidedef *NewSidedef()
{
    Sidedef *S = (Sidedef *)UtilCalloc(sizeof(Sidedef));
    S->index   = (int)level_sidedefs.size();
    level_sidedefs.push_back(S);
    return S;
}

Sector *NewSector()
{
    Sector *S = (Sector *)UtilCalloc(sizeof(Sector));
    S->index  = (int)level_sectors.size();
    level_sectors.push_back(S);
    return S;
}

Seg *NewSeg()
{
    Seg *S = (Seg *)UtilCalloc(sizeof(Seg));
    LockSystemMutex(alloc_mutex);
    level_segs.push_back(S);
    UnlockSystemMutex(alloc_mutex);
    return S;
}

Subsector *NewSubsec()
{
    Subsector *S = (Subsector *)UtilCalloc(sizeof(Subsector));
    LockSystemMutex(alloc_mutex);
    S->index_ = (int)level_subsecs.size();
    level_subsecs.push_back(S);
    UnlockSystemMutex(alloc_mutex);
    return S;
}

Node *NewNode()
{
    Node *N = (Node *)UtilCalloc(sizeof(Node));
    LockSystemMutex(alloc_mutex);
    level_nodes.push_back(N);
    UnlockSystemMutex(alloc_mutex);
    return N;
}

WallTip *NewWallTip()
{
    WallTip *WT = (WallTip *)UtilCalloc(sizeof(WallTip));
    LockSystemMutex(alloc_mutex);
    level_walltips.push_back(WT);
    UnlockSystemMutex(alloc_mutex);
    return WT;
}

Intersection *NewIntersection()
{
    Intersection *cut = new Intersection;
    LockSystemMutex(alloc_mutex);
    alloc_cuts.push_back(cut);
    UnlockSystemMutex(alloc_mutex);
    return cut;
}

//------------------------------------------------------------------------
// FREE ROUTINES
//------------------------------------------------------------------------

void FreeVertices()
{
    for (unsigned int i = 0; i < level_vertices.size(); i++)
        UtilFree((void *)level_vertices[i]);
    level_vertices.clear();
}

void FreeLinedefs()
{
    for (unsigned int i = 0; i < level_linedefs.size(); i++)
        UtilFree((void *)level_linedefs[i]);
    level_linedefs.clear();
}

void FreeSidedefs()
{
    for (unsigned int i = 0; i < level_sidedefs.size(); i++)
        UtilFree((void *)level_sidedefs[i]);
    level_sidedefs.clear();
}

void FreeSectors()
{
    for (unsigned int i = 0; i < level_sectors.size(); i++)
        UtilFree((void *)level_sectors[i]);
    level_sectors.clear();
}

void FreeSegs()
{
    for (unsigned int i = 0; i < level_segs.size(); i++)
        UtilFree((void *)level_segs[i]);
    level_segs.clear();
}

void FreeSubsecs()
{
    for (unsigned int i = 0; i < level_subsecs.size(); i++)
        UtilFree((void *)level_subsecs[i]);
    level_subsecs.clear();
}

void FreeNodes()
{
    for (unsigned int i = 0; i < level_nodes.size(); i++)
        UtilFree((void *)level_nodes[i]);
    level_nodes.clear();
}

void FreeWallTips()
{
    for (unsigned int i = 0; i < level_walltips.size(); i++)
        UtilFree((void *)level_walltips[i]);
    level_walltips.clear();
}

void FreeIntersections(void)
{
    for (size_t i = 0; i < alloc_cuts.size(); i++)
        delete alloc_cuts[i];
    alloc_cuts.clear();
}

void FreeLevel()
{
    FreeVertices();
    FreeSidedefs();
    FreeLinedefs();
    FreeSectors();
    FreeSegs();
    FreeSubsecs();
    FreeNodes();
    FreeWallTips();
    FreeIntersections();
}

//------------------------------------------------------------------------
// ANALYSIS
//------------------------------------------------------------------------

bool Vertex::Overlaps(const Vertex *other) const
{
    double dx = fabs(other->x_ - x_);
    double dy = fabs(other->y_ - y_);
    return (dx < kEpsilon) && (dy < kEpsilon);
}

static inline int cmpVertex(const Vertex *A, const Vertex *B)
{
    const double xdiff = (A->x_ - B->x_);
    if (fabs(xdiff) > 0.0001)
        return (xdiff < 0 ? -1 : 1);

    const double ydiff = (A->y_ - B->y_);
    if (fabs(ydiff) > 0.0001)
        return (ydiff < 0 ? -1 : 1);

    return 0;
}

static int VertexCompare(const void *p1, const void *p2)
{
    int vert1 = ((const uint32_t *)p1)[0];
    int vert2 = ((const uint32_t *)p2)[0];

    if (vert1 == vert2)
        return 0;

    Vertex *A = level_vertices[vert1];
    Vertex *B = level_vertices[vert2];

    return cmpVertex(A, B);
}

void DetectOverlappingVertices(void)
{
    size_t    i;
    uint32_t *array = (uint32_t *)UtilCalloc(level_vertices.size() * sizeof(uint32_t));

    for (i = 0; i < level_vertices.size(); i++)
        array[i] = i;

    qsort(array, level_vertices.size(), sizeof(uint32_t), VertexCompare);

    for (i = 0; i < level_vertices.size() - 1; i++)
    {
        if (VertexCompare(array + i, array + i + 1) == 0)
        {
            Vertex *A = level_vertices[array[i]];
            Vertex *B = level_vertices[array[i + 1]];

            B->overlap_ = A->overlap_ ? A->overlap_ : A;
        }
    }

    UtilFree(array);

    for (i = 0; i < level_linedefs.size(); i++)
    {
        Linedef *L = level_linedefs[i];

        while (L->start->overlap_)
            L->start = L->start->overlap_;

        while (L->end->overlap_)
            L->end = L->end->overlap_;
    }
}

void PruneVerticesAtEnd(void)
{
    int old_num = level_vertices.size();

    for (int i = level_vertices.size() - 1; i >= 0; i--)
    {
        Vertex *V = level_vertices[i];

        if (V->is_used_)
            break;

        UtilFree(V);
        level_vertices.pop_back();
    }

    int unused = old_num - level_vertices.size();

    if (unused > 0)
        LogDebug("    Pruned %d unused vertices at end\n", unused);

    num_old_vert = level_vertices.size();
}

static inline int LineVertexLowest(const Linedef *L)
{
    return ((int)L->start->x_ < (int)L->end->x_ ||
            ((int)L->start->x_ == (int)L->end->x_ && (int)L->start->y_ < (int)L->end->y_))
               ? 0
               : 1;
}

static int LineStartCompare(const void *p1, const void *p2)
{
    int line1 = ((const int *)p1)[0];
    int line2 = ((const int *)p2)[0];

    if (line1 == line2)
        return 0;

    Linedef *A = level_linedefs[line1];
    Linedef *B = level_linedefs[line2];

    Vertex *C = LineVertexLowest(A) ? A->end : A->start;
    Vertex *D = LineVertexLowest(B) ? B->end : B->start;

    return cmpVertex(C, D);
}

static int LineEndCompare(const void *p1, const void *p2)
{
    int line1 = ((const int *)p1)[0];
    int line2 = ((const int *)p2)[0];

    if (line1 == line2)
        return 0;

    Linedef *A = level_linedefs[line1];
    Linedef *B = level_linedefs[line2];

    Vertex *C = LineVertexLowest(A) ? A->start : A->end;
    Vertex *D = LineVertexLowest(B) ? B->start : B->end;

    return cmpVertex(C, D);
}

void DetectOverlappingLines(void)
{
    size_t i;
    int   *array = (int *)UtilCalloc(level_linedefs.size() * sizeof(int));

    for (i = 0; i < level_linedefs.size(); i++)
        array[i] = i;

    qsort(array, level_linedefs.size(), sizeof(int), LineStartCompare);

    for (i = 0; i < level_linedefs.size() - 1; i++)
    {
        size_t j;

        for (j = i + 1; j < level_linedefs.size(); j++)
        {
            if (LineStartCompare(array + i, array + j) != 0)
                break;

            if (LineEndCompare(array + i, array + j) == 0)
            {
                Linedef *A = level_linedefs[array[i]];
                Linedef *B = level_linedefs[array[j]];

                B->overlap = A->overlap ? A->overlap : A;
            }
        }
    }

    UtilFree(array);
}

void Vertex::AddWallTip(double dx, double dy, bool open_left, bool open_right)
{
    EPI_ASSERT(overlap_ == nullptr);

    WallTip *tip = NewWallTip();
    WallTip *after;

    tip->angle      = ComputeAngle(dx, dy);
    tip->open_left  = open_left;
    tip->open_right = open_right;

    for (after = tip_set_; after && after->next; after = after->next)
    {
    }

    while (after && tip->angle + kEpsilon < after->angle)
        after = after->previous;

    tip->next     = after ? after->next : tip_set_;
    tip->previous = after;

    if (after)
    {
        if (after->next)
            after->next->previous = tip;

        after->next = tip;
    }
    else
    {
        if (tip_set_ != nullptr)
            tip_set_->previous = tip;

        tip_set_ = tip;
    }
}

void CalculateWallTips()
{
    for (size_t i = 0; i < level_linedefs.size(); i++)
    {
        const Linedef *L = level_linedefs[i];

        if (L->overlap || L->zero_length)
            continue;

        double x1 = L->start->x_;
        double y1 = L->start->y_;
        double x2 = L->end->x_;
        double y2 = L->end->y_;

        bool left  = (L->left != nullptr) && (L->left->sector != nullptr);
        bool right = (L->right != nullptr) && (L->right->sector != nullptr);

        L->start->AddWallTip(x2 - x1, y2 - y1, left, right);
        L->end->AddWallTip(x1 - x2, y1 - y2, right, left);
    }
}

Vertex *NewVertexFromSplitSeg(Seg *seg, double x, double y)
{
    Vertex *vert = NewVertex();

    vert->x_ = x;
    vert->y_ = y;

    vert->is_new_  = true;
    vert->is_used_ = true;

    LockSystemMutex(alloc_mutex);
    vert->index_ = num_new_vert;
    num_new_vert++;
    UnlockSystemMutex(alloc_mutex);

    if (seg->linedef_ == nullptr)
    {
        vert->AddWallTip(seg->pdx_, seg->pdy_, true, true);
        vert->AddWallTip(-seg->pdx_, -seg->pdy_, true, true);
    }
    else
    {
        const Sidedef *front = seg->side_ ? seg->linedef_->left : seg->linedef_->right;
        const Sidedef *back  = seg->side_ ? seg->linedef_->right : seg->linedef_->left;

        bool left  = (back != nullptr) && (back->sector != nullptr);
        bool right = (front != nullptr) && (front->sector != nullptr);

        vert->AddWallTip(seg->pdx_, seg->pdy_, left, right);
        vert->AddWallTip(-seg->pdx_, -seg->pdy_, right, left);
    }

    return vert;
}

Vertex *NewVertexDegenerate(Vertex *start, Vertex *end)
{
    double dx = end->x_ - start->x_;
    double dy = end->y_ - start->y_;

    double dlen = hypot(dx, dy);

    Vertex *vert = NewVertex();

    vert->is_new_  = false;
    vert->is_used_ = true;

    LockSystemMutex(alloc_mutex);
    vert->index_ = num_old_vert;
    num_old_vert++;
    UnlockSystemMutex(alloc_mutex);

    vert->x_ = start->x_;
    vert->y_ = start->x_;

    if (epi::AlmostEquals(dlen, 0.0))
        FatalError("AJBSP: NewVertexDegenerate: bad delta!\n");

    dx /= dlen;
    dy /= dlen;

    while (RoundToInteger(vert->x_) == RoundToInteger(start->x_) &&
           RoundToInteger(vert->y_) == RoundToInteger(start->y_))
    {
        vert->x_ += dx;
        vert->y_ += dy;
    }

    return vert;
}

bool Vertex::CheckOpen(double dx, double dy) const
{
    const WallTip *tip;

    double angle = ComputeAngle(dx, dy);

    for (tip = tip_set_; tip; tip = tip->next)
    {
        if (fabs(tip->angle - angle) < kEpsilon || fabs(tip->angle - angle) > (360.0 - kEpsilon))
            return false;
    }

    for (tip = tip_set_; tip; tip = tip->next)
    {
        if (angle + kEpsilon < tip->angle)
            return tip->open_right;

        if (!tip->next)
            return tip->open_left;
    }

    return true;
}

//------------------------------------------------------------------------
// NODE BUILDING
//------------------------------------------------------------------------

void Seg::Recompute()
{
    psx_ = start_->x_;
    psy_ = start_->y_;
    pex_ = end_->x_;
    pey_ = end_->y_;
    pdx_ = pex_ - psx_;
    pdy_ = pey_ - psy_;

    p_length_ = hypot(pdx_, pdy_);

    if (p_length_ <= 0)
        FatalError("AJBSP: Seg %p has zero p_length_.\n", this);

    p_perp_ = psy_ * pdx_ - psx_ * pdy_;
    p_para_ = -psx_ * pdx_ - psy_ * pdy_;
}

Seg *SplitSeg(Seg *old_seg, double x, double y)
{
    Vertex *new_vert = NewVertexFromSplitSeg(old_seg, x, y);
    Seg    *new_seg  = NewSeg();

    new_seg[0]     = old_seg[0];
    new_seg->next_ = nullptr;

    old_seg->end_   = new_vert;
    new_seg->start_ = new_vert;

    old_seg->Recompute();
    new_seg->Recompute();

    if (old_seg->partner_)
    {
        new_seg->partner_ = NewSeg();

        new_seg->partner_[0] = old_seg->partner_[0];

        new_seg->partner_->partner_ = new_seg;

        old_seg->partner_->start_ = new_vert;
        new_seg->partner_->end_   = new_vert;

        old_seg->partner_->Recompute();
        new_seg->partner_->Recompute();

        old_seg->partner_->next_ = new_seg->partner_;
    }

    return new_seg;
}

inline void ComputeIntersection(Seg *seg, Seg *part, double perp_c, double perp_d, double *x, double *y)
{
    if (epi::AlmostEquals(part->pdy_, 0.0) && epi::AlmostEquals(seg->pdx_, 0.0))
    {
        *x = seg->psx_;
        *y = part->psy_;
        return;
    }

    if (epi::AlmostEquals(part->pdx_, 0.0) && epi::AlmostEquals(seg->pdy_, 0.0))
    {
        *x = part->psx_;
        *y = seg->psy_;
        return;
    }

    double ds = perp_c / (perp_c - perp_d);

    if (epi::AlmostEquals(seg->pdx_, 0.0))
        *x = seg->psx_;
    else
        *x = seg->psx_ + (seg->pdx_ * ds);

    if (epi::AlmostEquals(seg->pdy_, 0.0))
        *y = seg->psy_;
    else
        *y = seg->psy_ + (seg->pdy_ * ds);
}

void AddIntersection(Intersection **cut_list, Vertex *vert, Seg *part, bool self_ref)
{
    bool open_before = vert->CheckOpen(-part->pdx_, -part->pdy_);
    bool open_after  = vert->CheckOpen(part->pdx_, part->pdy_);

    double along_dist = part->ParallelDistance(vert->x_, vert->y_);

    Intersection *cut;
    Intersection *after;

    for (cut = (*cut_list); cut; cut = cut->next)
    {
        if (vert->Overlaps(cut->vertex))
            return;
    }

    cut = NewIntersection();

    cut->vertex      = vert;
    cut->along_dist  = along_dist;
    cut->self_ref    = self_ref;
    cut->open_before = open_before;
    cut->open_after  = open_after;

    for (after = (*cut_list); after && after->next; after = after->next)
    {
    }

    while (after && cut->along_dist < after->along_dist)
        after = after->prev;

    cut->next = after ? after->next : (*cut_list);
    cut->prev = after;

    if (after)
    {
        if (after->next)
            after->next->prev = cut;

        after->next = cut;
    }
    else
    {
        if (*cut_list)
            (*cut_list)->prev = cut;

        (*cut_list) = cut;
    }
}

bool EvalPartitionWorker(QuadTree *tree, Seg *part, double best_cost, EvalInfo *info)
{
    double split_cost = kSplitCostDefault;

    int side = tree->OnLineSide(part);

    if (side < 0)
    {
        info->real_left += tree->real_num_;
        info->mini_left += tree->mini_num_;
        return false;
    }
    else if (side > 0)
    {
        info->real_right += tree->real_num_;
        info->mini_right += tree->mini_num_;
        return false;
    }

    for (Seg *check = tree->list_; check; check = check->next_)
    {
        if (info->cost > best_cost)
            return true;

        double qnty;

        double a = 0, fa = 0;
        double b = 0, fb = 0;

        if (check->source_line_ != part->source_line_)
        {
            epi::SimdF64x2 ab = epi::PerpDistPairF64x2(
                check->psx_, check->psy_, check->pex_, check->pey_,
                part->pdx_, part->pdy_, part->p_perp_, part->p_length_);
            a  = epi::GetLane0F64x2(ab);
            b  = epi::GetLane1F64x2(ab);
            fa = fabs(a);
            fb = fabs(b);
        }

        if (fa <= kEpsilon && fb <= kEpsilon)
        {
            if (check->pdx_ * part->pdx_ + check->pdy_ * part->pdy_ < 0)
                info->BumpLeft(check->linedef_);
            else
                info->BumpRight(check->linedef_);

            continue;
        }

        if (fa <= kEpsilon || fb <= kEpsilon)
        {
            if (check->linedef_ != nullptr && check->linedef_->is_precious)
                info->cost += 40.0 * split_cost * kPreciousCostMultiplier;
        }

        if (a > -kEpsilon && b > -kEpsilon)
        {
            info->BumpRight(check->linedef_);

            if ((a >= kIffySegLength && b >= kIffySegLength) || (a <= kEpsilon && b >= kIffySegLength) ||
                (b <= kEpsilon && a >= kIffySegLength))
            {
                continue;
            }

            info->near_miss++;

            if (a <= kEpsilon || b <= kEpsilon)
                qnty = kIffySegLength / HMM_MAX(a, b);
            else
                qnty = kIffySegLength / HMM_MIN(a, b);

            info->cost += 70.0 * split_cost * (qnty * qnty - 1.0);
            continue;
        }

        if (a < kEpsilon && b < kEpsilon)
        {
            info->BumpLeft(check->linedef_);

            if ((a <= -kIffySegLength && b <= -kIffySegLength) || (a >= -kEpsilon && b <= -kIffySegLength) ||
                (b >= -kEpsilon && a <= -kIffySegLength))
            {
                continue;
            }

            info->near_miss++;

            if (a >= -kEpsilon || b >= -kEpsilon)
                qnty = kIffySegLength / -HMM_MIN(a, b);
            else
                qnty = kIffySegLength / -HMM_MAX(a, b);

            info->cost += 70.0 * split_cost * (qnty * qnty - 1.0);
            continue;
        }

        info->splits++;

        if (check->linedef_ && check->linedef_->is_precious)
            info->cost += 100.0 * split_cost * kPreciousCostMultiplier;
        else
            info->cost += 100.0 * split_cost;

        if (fa < kIffySegLength || fb < kIffySegLength)
        {
            info->iffy++;

            qnty = kIffySegLength / HMM_MIN(fa, fb);
            info->cost += 140.0 * split_cost * (qnty * qnty - 1.0);
        }
    }

    for (int c = 0; c < 2; c++)
    {
        if (info->cost > best_cost)
            return true;

        if (tree->subs_[c] != nullptr && !tree->subs_[c]->Empty())
        {
            if (EvalPartitionWorker(tree->subs_[c], part, best_cost, info))
                return true;
        }
    }

    return false;
}

double EvalPartition(QuadTree *tree, Seg *part, double best_cost)
{
    EvalInfo info;

    info.cost      = 0;
    info.splits    = 0;
    info.iffy      = 0;
    info.near_miss = 0;

    info.real_left  = 0;
    info.real_right = 0;
    info.mini_left  = 0;
    info.mini_right = 0;

    if (EvalPartitionWorker(tree, part, best_cost, &info))
        return -1.0;

    if (info.real_left == 0 || info.real_right == 0)
        return -1;

    info.cost += 100.0 * abs(info.real_left - info.real_right);
    info.cost += 50.0 * abs(info.mini_left - info.mini_right);

    if (!epi::AlmostEquals(part->pdx_, 0.0) && !epi::AlmostEquals(part->pdy_, 0.0))
        info.cost += 25.0;

    return info.cost;
}

void EvaluateFastWorker(QuadTree *tree, Seg **best_H, Seg **best_V, int mid_x, int mid_y)
{
    for (Seg *part = tree->list_; part; part = part->next_)
    {
        if (part->linedef_ == nullptr)
            continue;

        if (part->linedef_->is_precious)
            continue;

        if (epi::AlmostEquals(part->pdy_, 0.0))
        {
            if (!*best_H)
            {
                *best_H = part;
            }
            else
            {
                double old_dist = fabs((*best_H)->psy_ - mid_y);
                double new_dist = fabs((part)->psy_ - mid_y);

                if (new_dist < old_dist)
                    *best_H = part;
            }
        }
        else if (epi::AlmostEquals(part->pdx_, 0.0))
        {
            if (!*best_V)
            {
                *best_V = part;
            }
            else
            {
                double old_dist = fabs((*best_V)->psx_ - mid_x);
                double new_dist = fabs((part)->psx_ - mid_x);

                if (new_dist < old_dist)
                    *best_V = part;
            }
        }
    }

    for (int c = 0; c < 2; c++)
    {
        if (tree->subs_[c] != nullptr && !tree->subs_[c]->Empty())
            EvaluateFastWorker(tree->subs_[c], best_H, best_V, mid_x, mid_y);
    }
}

Seg *FindFastSeg(QuadTree *tree)
{
    Seg *best_H = nullptr;
    Seg *best_V = nullptr;

    int mid_x = (tree->x1_ + tree->x2_) / 2;
    int mid_y = (tree->y1_ + tree->y2_) / 2;

    EvaluateFastWorker(tree, &best_H, &best_V, mid_x, mid_y);

    double H_cost = -1.0;
    double V_cost = -1.0;

    if (best_H)
        H_cost = EvalPartition(tree, best_H, 1.0e99);

    if (best_V)
        V_cost = EvalPartition(tree, best_V, 1.0e99);

    if (H_cost < 0 && V_cost < 0)
        return nullptr;

    if (H_cost < 0)
        return best_V;
    if (V_cost < 0)
        return best_H;

    return (V_cost < H_cost) ? best_V : best_H;
}

static bool PickNodeWorker(QuadTree *part_list, QuadTree *tree, Seg **best, double *best_cost)
{
    for (Seg *part = part_list->list_; part; part = part->next_)
    {
        if (part->linedef_ == nullptr)
            continue;

        double cost = EvalPartition(tree, part, *best_cost);

        if (cost < 0 || cost >= *best_cost)
            continue;

        *best_cost = cost;
        *best      = part;
    }

    for (int c = 0; c < 2; c++)
    {
        if (part_list->subs_[c] != nullptr && !part_list->subs_[c]->Empty())
            PickNodeWorker(part_list->subs_[c], tree, best, best_cost);
    }

    return true;
}

static Seg *PickNode(QuadTree *tree)
{
    Seg   *best      = nullptr;
    double best_cost = 1.0e99;

    if (tree->real_num_ >= kSegFastModeThreshold)
    {
        best = FindFastSeg(tree);

        if (best != nullptr)
            return best;
    }

    if (!PickNodeWorker(tree, tree, &best, &best_cost))
        return nullptr;

    return best;
}

static void DivideOneSeg(Seg *seg, Seg *part, Seg **left_list, Seg **right_list, Intersection **cut_list)
{
    epi::SimdF64x2 ab = epi::PerpDistPairF64x2(
        seg->psx_, seg->psy_, seg->pex_, seg->pey_,
        part->pdx_, part->pdy_, part->p_perp_, part->p_length_);
    double a = epi::GetLane0F64x2(ab);
    double b = epi::GetLane1F64x2(ab);

    bool self_ref = seg->linedef_ ? seg->linedef_->self_referencing : false;

    if (seg->source_line_ == part->source_line_)
        a = b = 0;

    if (fabs(a) <= kEpsilon && fabs(b) <= kEpsilon)
    {
        AddIntersection(cut_list, seg->start_, part, self_ref);
        AddIntersection(cut_list, seg->end_, part, self_ref);

        if (seg->pdx_ * part->pdx_ + seg->pdy_ * part->pdy_ < 0)
            ListAddSeg(left_list, seg);
        else
            ListAddSeg(right_list, seg);

        return;
    }

    if (a > -kEpsilon && b > -kEpsilon)
    {
        if (a < kEpsilon)
            AddIntersection(cut_list, seg->start_, part, self_ref);
        else if (b < kEpsilon)
            AddIntersection(cut_list, seg->end_, part, self_ref);

        ListAddSeg(right_list, seg);
        return;
    }

    if (a < kEpsilon && b < kEpsilon)
    {
        if (a > -kEpsilon)
            AddIntersection(cut_list, seg->start_, part, self_ref);
        else if (b > -kEpsilon)
            AddIntersection(cut_list, seg->end_, part, self_ref);

        ListAddSeg(left_list, seg);
        return;
    }

    double x, y;
    ComputeIntersection(seg, part, a, b, &x, &y);

    Seg *new_seg = SplitSeg(seg, x, y);

    AddIntersection(cut_list, seg->end_, part, self_ref);

    if (a < 0)
    {
        ListAddSeg(left_list, seg);
        ListAddSeg(right_list, new_seg);
    }
    else
    {
        ListAddSeg(right_list, seg);
        ListAddSeg(left_list, new_seg);
    }
}

static void SeparateSegs(QuadTree *tree, Seg *part, Seg **left_list, Seg **right_list, Intersection **cut_list)
{
    while (tree->list_ != nullptr)
    {
        Seg *seg    = tree->list_;
        tree->list_ = seg->next_;

        seg->quad_ = nullptr;
        DivideOneSeg(seg, part, left_list, right_list, cut_list);
    }

    if (tree->subs_[0] != nullptr)
    {
        SeparateSegs(tree->subs_[0], part, left_list, right_list, cut_list);
        SeparateSegs(tree->subs_[1], part, left_list, right_list, cut_list);
    }
}

static void FindLimits2(Seg *list, BoundingBox *bbox)
{
    if (list == nullptr)
    {
        bbox->minimum_x = 0;
        bbox->minimum_y = 0;
        bbox->maximum_x = 4;
        bbox->maximum_y = 4;
        return;
    }

    epi::SimdF32x4 running = epi::SetF32x4(-(float)SHRT_MAX, -(float)SHRT_MAX,
                                               (float)SHRT_MIN,  (float)SHRT_MIN);

    for (; list != nullptr; list = list->next_)
    {
        double x1 = list->start_->x_;
        double y1 = list->start_->y_;
        double x2 = list->end_->x_;
        double y2 = list->end_->y_;

        float lx = (float)floor(HMM_MIN(x1, x2) - 0.2);
        float ly = (float)floor(HMM_MIN(y1, y2) - 0.2);
        float hx = (float)ceil(HMM_MAX(x1, x2) + 0.2);
        float hy = (float)ceil(HMM_MAX(y1, y2) + 0.2);

        running = epi::MaxF32x4(running, epi::SetF32x4(-lx, -ly, hx, hy));
    }

    float out[4];
    epi::StoreF32x4(out, running);
    bbox->minimum_x = -(int)out[0];
    bbox->minimum_y = -(int)out[1];
    bbox->maximum_x =  (int)out[2];
    bbox->maximum_y =  (int)out[3];
}

static void AddMinisegs(Intersection *cut_list, Seg *part, Seg **left_list, Seg **right_list)
{
    Intersection *cut, *next;

    for (cut = cut_list; cut && cut->next; cut = cut->next)
    {
        next = cut->next;

        double len = next->along_dist - cut->along_dist;
        if (len < -0.001)
            FatalError("AJBSP: Bad order in intersect list: %1.3f > %1.3f\n", cut->along_dist, next->along_dist);

        bool A = cut->open_after;
        bool B = next->open_before;

        if (!(A || B))
            continue;

        if (A != B)
            continue;

        Seg *seg   = NewSeg();
        Seg *buddy = NewSeg();

        seg->partner_   = buddy;
        buddy->partner_ = seg;

        seg->start_ = cut->vertex;
        seg->end_   = next->vertex;

        buddy->start_ = next->vertex;
        buddy->end_   = cut->vertex;

        seg->index_ = buddy->index_ = -1;
        seg->linedef_ = buddy->linedef_ = nullptr;
        seg->side_ = buddy->side_ = 0;

        seg->source_line_ = buddy->source_line_ = part->linedef_;

        seg->Recompute();
        buddy->Recompute();

        ListAddSeg(right_list, seg);
        ListAddSeg(left_list, buddy);
    }
}

void Node::SetPartition(const Seg *part)
{
    EPI_ASSERT(part->linedef_);

    if (part->side_ == 0)
    {
        x_  = part->linedef_->start->x_;
        y_  = part->linedef_->start->y_;
        dx_ = part->linedef_->end->x_ - x_;
        dy_ = part->linedef_->end->y_ - y_;
    }
    else
    {
        x_  = part->linedef_->end->x_;
        y_  = part->linedef_->end->y_;
        dx_ = part->linedef_->start->x_ - x_;
        dy_ = part->linedef_->start->y_ - y_;
    }

    if (fabs(dx_) > 32766 || fabs(dy_) > 32766)
    {
        dx_ = dx_ / 2.0;
        dy_ = dy_ / 2.0;
    }
}

int Seg::PointOnLineSide(double x, double y) const
{
    double perp = PerpendicularDistance(x, y);

    if (fabs(perp) <= kEpsilon)
        return 0;

    return (perp < 0) ? -1 : +1;
}

QuadTree::QuadTree(int x1, int y1, int x2, int y2)
    : x1_(x1), y1_(y1), x2_(x2), y2_(y2), real_num_(0), mini_num_(0), list_(nullptr)
{
    int dx = x2 - x1;
    int dy = y2 - y1;

    if (dx <= 320 && dy <= 320)
    {
        subs_[0] = nullptr;
        subs_[1] = nullptr;
    }
    else if (dx >= dy)
    {
        subs_[0] = new QuadTree(x1, y1, x1 + dx / 2, y2);
        subs_[1] = new QuadTree(x1 + dx / 2, y1, x2, y2);
    }
    else
    {
        subs_[0] = new QuadTree(x1, y1, x2, y1 + dy / 2);
        subs_[1] = new QuadTree(x1, y1 + dy / 2, x2, y2);
    }
}

QuadTree::~QuadTree()
{
    if (subs_[0] != nullptr)
        delete subs_[0];
    if (subs_[1] != nullptr)
        delete subs_[1];
}

void QuadTree::AddSeg(Seg *seg)
{
    if (seg->linedef_ != nullptr)
        real_num_++;
    else
        mini_num_++;

    if (subs_[0] != nullptr)
    {
        double x_min = HMM_MIN(seg->start_->x_, seg->end_->x_);
        double y_min = HMM_MIN(seg->start_->y_, seg->end_->y_);

        double x_max = HMM_MAX(seg->start_->x_, seg->end_->x_);
        double y_max = HMM_MAX(seg->start_->y_, seg->end_->y_);

        if ((x2_ - x1_) >= (y2_ - y1_))
        {
            if (x_min > subs_[1]->x1_)
            {
                subs_[1]->AddSeg(seg);
                return;
            }
            else if (x_max < subs_[0]->x2_)
            {
                subs_[0]->AddSeg(seg);
                return;
            }
        }
        else
        {
            if (y_min > subs_[1]->y1_)
            {
                subs_[1]->AddSeg(seg);
                return;
            }
            else if (y_max < subs_[0]->y2_)
            {
                subs_[0]->AddSeg(seg);
                return;
            }
        }
    }

    ListAddSeg(&list_, seg);
    seg->quad_ = this;
}

void QuadTree::AddList(Seg *new_list)
{
    while (new_list != nullptr)
    {
        Seg *seg = new_list;
        new_list = seg->next_;
        AddSeg(seg);
    }
}

void QuadTree::ConvertToList(Seg **list)
{
    while (list_ != nullptr)
    {
        Seg *seg = list_;
        list_    = seg->next_;
        ListAddSeg(list, seg);
    }

    if (subs_[0] != nullptr)
    {
        subs_[0]->ConvertToList(list);
        subs_[1]->ConvertToList(list);
    }
}

int QuadTree::OnLineSide(const Seg *part) const
{
    double tx1 = (double)x1_ - 0.4;
    double ty1 = (double)y1_ - 0.4;
    double tx2 = (double)x2_ + 0.4;
    double ty2 = (double)y2_ + 0.4;

    int p1, p2;

    if (epi::AlmostEquals(part->pdx_, 0.0))
    {
        p1 = (tx1 > part->psx_) ? +1 : -1;
        p2 = (tx2 > part->psx_) ? +1 : -1;

        if (part->pdy_ < 0)
        {
            p1 = -p1;
            p2 = -p2;
        }
    }
    else if (epi::AlmostEquals(part->pdy_, 0.0))
    {
        p1 = (ty1 < part->psy_) ? +1 : -1;
        p2 = (ty2 < part->psy_) ? +1 : -1;

        if (part->pdx_ < 0)
        {
            p1 = -p1;
            p2 = -p2;
        }
    }
    else if (part->pdx_ * part->pdy_ > 0)
    {
        p1 = part->PointOnLineSide(tx1, ty2);
        p2 = part->PointOnLineSide(tx2, ty1);
    }
    else
    {
        p1 = part->PointOnLineSide(tx1, ty1);
        p2 = part->PointOnLineSide(tx2, ty2);
    }

    if (p1 != p2)
        return 0;

    return p1;
}

Seg *CreateOneSeg(Linedef *line, Vertex *start, Vertex *end, Sidedef *side, int what_side)
{
    Seg *seg = NewSeg();

    if (side->sector == nullptr)
        LogPrint("Bad sidedef on linedef #%d (Z_CheckHeap error)\n", line->index);

    if (start->overlap_)
        start = start->overlap_;
    if (end->overlap_)
        end = end->overlap_;

    seg->start_   = start;
    seg->end_     = end;
    seg->linedef_ = line;
    seg->side_    = what_side;
    seg->partner_ = nullptr;

    seg->source_line_ = seg->linedef_;
    seg->index_       = -1;

    seg->Recompute();

    return seg;
}

Seg *CreateSegs()
{
    Seg *list = nullptr;

    for (size_t i = 0; i < level_linedefs.size(); i++)
    {
        Linedef *line = level_linedefs[i];

        Seg *left  = nullptr;
        Seg *right = nullptr;

        if (line->zero_length)
            continue;

        if (line->overlap != nullptr)
            continue;

        double ldx = line->start->x_ - line->end->x_;
        double ldy = line->start->y_ - line->end->y_;
        if (ldx * ldx + ldy * ldy >= 32000.0 * 32000.0)
            LogPrint("Linedef #%d is VERY long, it may cause problems\n", line->index);

        if (line->right != nullptr)
        {
            right = CreateOneSeg(line, line->start, line->end, line->right, 0);
            ListAddSeg(&list, right);
        }
        else
            LogPrint("Linedef #%d has no right sidedef!\n", line->index);

        if (line->left != nullptr)
        {
            left = CreateOneSeg(line, line->end, line->start, line->left, 1);
            ListAddSeg(&list, left);

            if (right != nullptr)
            {
                left->partner_  = right;
                right->partner_ = left;
            }
        }
        else
        {
            if (line->two_sided)
            {
                LogPrint("Linedef #%d is 2s but has no left sidedef\n", line->index);
                line->two_sided = false;
            }
        }
    }

    return list;
}

static QuadTree *TreeFromSegList(Seg *list, const BoundingBox *bounds)
{
    QuadTree *tree = new QuadTree(bounds->minimum_x, bounds->minimum_y, bounds->maximum_x, bounds->maximum_y);
    tree->AddList(list);
    return tree;
}

void Subsector::DetermineMiddle()
{
    mid_x_ = 0.0;
    mid_y_ = 0.0;

    int total = 0;

    for (Seg *seg = seg_list_; seg; seg = seg->next_)
    {
        mid_x_ += seg->start_->x_ + seg->end_->x_;
        mid_y_ += seg->start_->y_ + seg->end_->y_;
        total += 2;
    }

    if (total > 0)
    {
        mid_x_ /= total;
        mid_y_ /= total;
    }
}

void Subsector::AddToTail(Seg *seg)
{
    seg->next_ = nullptr;

    if (seg_list_ == nullptr)
    {
        seg_list_ = seg;
        return;
    }

    Seg *tail = seg_list_;
    while (tail->next_ != nullptr)
        tail = tail->next_;

    tail->next_ = seg;
}

void Subsector::ClockwiseOrder()
{
    std::vector<Seg *> array;

    for (Seg *seg = seg_list_; seg; seg = seg->next_)
    {
        seg->cmp_angle_ = ComputeAngle(seg->start_->x_ - mid_x_, seg->start_->y_ - mid_y_);
        array.push_back(seg);
    }

    size_t i = 0;

    while (i + 1 < array.size())
    {
        Seg *A = array[i];
        Seg *B = array[i + 1];

        if (A->cmp_angle_ < B->cmp_angle_)
        {
            array[i]     = B;
            array[i + 1] = A;

            if (i > 0)
                i--;
        }
        else
        {
            i++;
        }
    }

    int first = 0;
    int score = -1;

    for (i = 0; i < array.size(); i++)
    {
        int cur_score = 3;

        if (!array[i]->linedef_)
            cur_score = 0;
        else if (array[i]->linedef_->self_referencing)
            cur_score = 2;

        if (cur_score > score)
        {
            first = i;
            score = cur_score;
        }
    }

    seg_list_ = nullptr;

    for (i = 0; i < array.size(); i++)
    {
        size_t k = (first + i) % array.size();
        AddToTail(array[k]);
    }
}

void Subsector::SanityCheckClosed() const
{
    int gaps  = 0;
    int total = 0;

    Seg *seg, *next;

    for (seg = seg_list_; seg; seg = seg->next_)
    {
        next = seg->next_ ? seg->next_ : seg_list_;

        double dx = seg->end_->x_ - next->start_->x_;
        double dy = seg->end_->y_ - next->start_->y_;

        if (fabs(dx) > kEpsilon || fabs(dy) > kEpsilon)
            gaps++;

        total++;
    }

    if (gaps > 0)
        LogPrint("Subsector #%d near (%1.1f,%1.1f) is not closed (%d gaps, %d segs)\n", index_, mid_x_, mid_y_, gaps,
                 total);
}

void Subsector::SanityCheckHasRealSeg() const
{
    for (Seg *seg = seg_list_; seg; seg = seg->next_)
        if (seg->linedef_ != nullptr)
            return;

    FatalError("AJBSP: Subsector #%d near (%1.1f,%1.1f) has no real seg!\n", index_, mid_x_, mid_y_);
}

void Subsector::RenumberSegs(int &cur_seg_index)
{
    seg_count_ = 0;

    for (Seg *seg = seg_list_; seg; seg = seg->next_)
    {
        seg->index_ = cur_seg_index;
        cur_seg_index += 1;
        seg_count_++;
    }
}

Subsector *CreateSubsec(QuadTree *tree)
{
    Subsector *sub = NewSubsec();

    sub->seg_list_ = nullptr;
    tree->ConvertToList(&sub->seg_list_);
    sub->DetermineMiddle();

    return sub;
}

int ComputeBSPHeight(const Node *node)
{
    if (node == nullptr)
        return 1;

    int right = ComputeBSPHeight(node->r_.node);
    int left  = ComputeBSPHeight(node->l_.node);

    return HMM_MAX(left, right) + 1;
}

static BuildResult BuildNodes(Seg *list, BoundingBox *bounds, Node **N, Subsector **S);

BuildResult BuildNodes(Seg *list, BoundingBox *bounds, Node **N, Subsector **S)
{
    *N = nullptr;
    *S = nullptr;

    FindLimits2(list, bounds);

    QuadTree *tree = TreeFromSegList(list, bounds);

    Seg *part = PickNode(tree);

    if (part == nullptr)
    {
        *S = CreateSubsec(tree);
        delete tree;

        return kBuildOK;
    }

    Node *node = NewNode();
    *N         = node;

    Seg          *lefts    = nullptr;
    Seg          *rights   = nullptr;
    Intersection *cut_list = nullptr;

    SeparateSegs(tree, part, &lefts, &rights, &cut_list);

    delete tree;
    tree = nullptr;

    if (rights == nullptr)
        FatalError("AJBSP: Separated seg-list has empty RIGHT side\n");

    if (lefts == nullptr)
        FatalError("AJBSP: Separated seg-list has empty LEFT side\n");

    if (cut_list != nullptr)
        AddMinisegs(cut_list, part, &lefts, &rights);

    node->SetPartition(part);

    BuildResult ret_l = BuildNodes(lefts,  &node->l_.bounds, &node->l_.node, &node->l_.subsec);
    BuildResult ret_r = BuildNodes(rights, &node->r_.bounds, &node->r_.node, &node->r_.subsec);

    if (ret_l != kBuildOK) return ret_l;
    if (ret_r != kBuildOK) return ret_r;
    return kBuildOK;
}

struct BuildForkArgs
{
    Seg        *list;
    BoundingBox *bounds;
    Node       **node_out;
    Subsector  **sub_out;
    BuildResult  result;
};

static int BuildSubtreeTask(void *data)
{
    BuildForkArgs *args = (BuildForkArgs *)data;
    args->result        = BuildNodes(args->list, args->bounds, args->node_out, args->sub_out);
    return 0;
}

static void SeverCrossListPartners(Seg *rights)
{
    std::unordered_set<Seg *> right_set;
    for (Seg *s = rights; s; s = s->next_)
        right_set.insert(s);

    for (Seg *s = rights; s; s = s->next_)
    {
        if (s->partner_ != nullptr && right_set.count(s->partner_) == 0)
        {
            s->partner_->partner_ = nullptr;
            s->partner_           = nullptr;
        }
    }
}

static void MatchByOverlap(std::vector<Seg *> &group_a, std::vector<Seg *> &group_b,
                           const Vertex *ld_start, double ldx, double ldy, double len2,
                           bool a_is_forward)
{
    bool b_is_forward = !a_is_forward;

    for (size_t ai = 0; ai < group_a.size(); ai++)
    {
        Seg          *a      = group_a[ai];
        const Vertex *a_lo_v = a_is_forward ? a->start_ : a->end_;
        const Vertex *a_hi_v = a_is_forward ? a->end_   : a->start_;
        double        a_lo   = ((a_lo_v->x_ - ld_start->x_) * ldx + (a_lo_v->y_ - ld_start->y_) * ldy) / len2;
        double        a_hi   = ((a_hi_v->x_ - ld_start->x_) * ldx + (a_hi_v->y_ - ld_start->y_) * ldy) / len2;

        Seg   *best    = nullptr;
        double best_ov = 0.0;

        for (size_t bi = 0; bi < group_b.size(); bi++)
        {
            Seg          *b      = group_b[bi];
            const Vertex *b_lo_v = b_is_forward ? b->start_ : b->end_;
            const Vertex *b_hi_v = b_is_forward ? b->end_   : b->start_;
            double        b_lo   = ((b_lo_v->x_ - ld_start->x_) * ldx + (b_lo_v->y_ - ld_start->y_) * ldy) / len2;
            double        b_hi   = ((b_hi_v->x_ - ld_start->x_) * ldx + (b_hi_v->y_ - ld_start->y_) * ldy) / len2;
            double        ov     = HMM_MIN(a_hi, b_hi) - HMM_MAX(a_lo, b_lo);
            if (ov > best_ov)
            {
                best_ov = ov;
                best    = b;
            }
        }

        if (best != nullptr)
            a->partner_ = best;
    }
}

static void RepairPartnerLinks()
{
    std::unordered_map<int, std::vector<Seg *>> front_map, back_map;
    std::unordered_map<int, std::vector<Seg *>> mini_fwd_map, mini_bwd_map;

    for (size_t i = 0; i < level_segs.size(); i++)
    {
        Seg *s = level_segs[i];
        if (s->partner_ != nullptr)
            continue;

        if (s->linedef_ != nullptr)
        {
            if (!s->linedef_->two_sided)
                continue;
            if (s->side_ == 0)
                front_map[s->linedef_->index].push_back(s);
            else
                back_map[s->linedef_->index].push_back(s);
        }
        else if (s->source_line_ != nullptr)
        {
            const Linedef *sl  = s->source_line_;
            double         slx = sl->end->x_ - sl->start->x_;
            double         sly = sl->end->y_ - sl->start->y_;
            double         len2 = slx * slx + sly * sly;
            if (len2 < kEpsilon)
                continue;
            double t_s = ((s->start_->x_ - sl->start->x_) * slx + (s->start_->y_ - sl->start->y_) * sly) / len2;
            double t_e = ((s->end_->x_   - sl->start->x_) * slx + (s->end_->y_   - sl->start->y_) * sly) / len2;
            if (t_s < t_e)
                mini_fwd_map[sl->index].push_back(s);
            else
                mini_bwd_map[sl->index].push_back(s);
        }
    }

    for (std::unordered_map<int, std::vector<Seg *>>::iterator it_f = front_map.begin();
         it_f != front_map.end(); ++it_f)
    {
        std::unordered_map<int, std::vector<Seg *>>::iterator it_b = back_map.find(it_f->first);
        if (it_b == back_map.end())
            continue;

        std::vector<Seg *> &fronts = it_f->second;
        std::vector<Seg *> &backs  = it_b->second;

        const Linedef *ld   = fronts[0]->linedef_;
        double         ldx  = ld->end->x_ - ld->start->x_;
        double         ldy  = ld->end->y_ - ld->start->y_;
        double         len2 = ldx * ldx + ldy * ldy;
        if (len2 < kEpsilon)
            continue;

        MatchByOverlap(fronts, backs, ld->start, ldx, ldy, len2, true);
        MatchByOverlap(backs,  fronts, ld->start, ldx, ldy, len2, false);
    }

    for (std::unordered_map<int, std::vector<Seg *>>::iterator it_f = mini_fwd_map.begin();
         it_f != mini_fwd_map.end(); ++it_f)
    {
        std::unordered_map<int, std::vector<Seg *>>::iterator it_b = mini_bwd_map.find(it_f->first);
        if (it_b == mini_bwd_map.end())
            continue;

        std::vector<Seg *> &fwds = it_f->second;
        std::vector<Seg *> &bwds = it_b->second;

        const Linedef *sl   = fwds[0]->source_line_;
        double         slx  = sl->end->x_ - sl->start->x_;
        double         sly  = sl->end->y_ - sl->start->y_;
        double         len2 = slx * slx + sly * sly;
        if (len2 < kEpsilon)
            continue;

        MatchByOverlap(fwds, bwds, sl->start, slx, sly, len2, true);
        MatchByOverlap(bwds, fwds, sl->start, slx, sly, len2, false);
    }
}

static BuildResult BuildNodesRoot(Seg *list, BoundingBox *bounds, Node **N, Subsector **S)
{
    *N = nullptr;
    *S = nullptr;

    FindLimits2(list, bounds);

    QuadTree *tree = TreeFromSegList(list, bounds);

    Seg *part = PickNode(tree);

    if (part == nullptr)
    {
        *S = CreateSubsec(tree);
        delete tree;
        return kBuildOK;
    }

    Node *node = NewNode();
    *N         = node;

    Seg          *lefts    = nullptr;
    Seg          *rights   = nullptr;
    Intersection *cut_list = nullptr;

    SeparateSegs(tree, part, &lefts, &rights, &cut_list);

    delete tree;
    tree = nullptr;

    if (rights == nullptr)
        FatalError("AJBSP: Separated seg-list has empty RIGHT side\n");
    if (lefts == nullptr)
        FatalError("AJBSP: Separated seg-list has empty LEFT side\n");

    if (cut_list != nullptr)
        AddMinisegs(cut_list, part, &lefts, &rights);

    SeverCrossListPartners(rights);

    node->SetPartition(part);

    BuildForkArgs fork_args;
    fork_args.list     = lefts;
    fork_args.bounds   = &node->l_.bounds;
    fork_args.node_out = &node->l_.node;
    fork_args.sub_out  = &node->l_.subsec;
    fork_args.result   = kBuildOK;

    SystemThread *fork_thread = StartThread(BuildSubtreeTask, &fork_args, "BSPBuild");

    BuildResult ret_r = BuildNodes(rights, &node->r_.bounds, &node->r_.node, &node->r_.subsec);

    if (fork_thread)
        JoinThread(fork_thread);
    else
        BuildSubtreeTask(&fork_args);

    if (fork_args.result != kBuildOK)
        return fork_args.result;
    if (ret_r != kBuildOK)
        return ret_r;
    return kBuildOK;
}

void ClockwiseBSPTree()
{
    int cur_seg_index = 0;

    for (size_t i = 0; i < level_subsecs.size(); i++)
    {
        Subsector *sub = level_subsecs[i];

        sub->ClockwiseOrder();
        sub->RenumberSegs(cur_seg_index);

        sub->SanityCheckClosed();
        sub->SanityCheckHasRealSeg();
    }
}

//------------------------------------------------------------------------
// LEVEL TRANSFER
//------------------------------------------------------------------------

static void LoadLevelFromInput(const InputLevel &input)
{
    num_new_vert   = 0;
    num_real_lines = 0;

    for (size_t i = 0; i < input.vertexes.size(); i++)
    {
        Vertex *v = NewVertex();
        v->x_     = (double)input.vertexes[i].x;
        v->y_     = (double)input.vertexes[i].y;
    }

    num_old_vert = (int)level_vertices.size();

    for (int i = 0; i < input.sector_count; i++)
        NewSector();

    for (size_t i = 0; i < input.sidedef_sectors.size(); i++)
    {
        Sidedef *side = NewSidedef();
        int32_t  sec  = input.sidedef_sectors[i];
        if (sec >= 0 && sec < input.sector_count)
            side->sector = level_sectors[sec];
    }

    for (size_t i = 0; i < input.linedefs.size(); i++)
    {
        const InputLinedef &iline = input.linedefs[i];

        Vertex *start = level_vertices[iline.vertex_1];
        Vertex *end   = level_vertices[iline.vertex_2];

        start->is_used_ = true;
        end->is_used_   = true;

        Linedef *line = NewLinedef();

        line->start       = start;
        line->end         = end;
        line->zero_length = (fabs(start->x_ - end->x_) < kEpsilon) && (fabs(start->y_ - end->y_) < kEpsilon);

        line->type        = 0;
        line->two_sided   = (iline.left_side >= 0);
        line->is_precious = (iline.tag >= 900 && iline.tag < 1000);

        if (iline.right_side >= 0)
            line->right = level_sidedefs[iline.right_side];

        if (iline.left_side >= 0)
            line->left = level_sidedefs[iline.left_side];

        if (line->right || line->left)
            num_real_lines++;

        line->self_referencing = (line->left && line->right && (line->left->sector == line->right->sector));
        if (line->self_referencing)
            line->is_precious = true;
    }

    LogDebug("AJBSP: Loaded %zu vertices, %zu sectors, %zu sides, %zu lines\n", level_vertices.size(),
             level_sectors.size(), level_sidedefs.size(), level_linedefs.size());

    PruneVerticesAtEnd();
    DetectOverlappingVertices();
    DetectOverlappingLines();
    CalculateWallTips();
}

static int engine_node_idx;

struct CachedVertex
{
    float x, y;
};

struct CachedSeg
{
    int32_t vertex_1;
    int32_t partner;
    int32_t linedef;
    uint8_t vertex_1_is_gl;
    uint8_t side;
};

struct CachedSubsector
{
    int32_t first_seg;
    int32_t seg_count;
};

static std::vector<CachedVertex>    cached_vertexes;
static std::vector<CachedSeg>       cached_segs;
static std::vector<CachedSubsector> cached_subsectors;
static std::vector<::BSPNode>       cached_nodes;

static void TransferOneNode(Node *node)
{
    if (node->r_.node)
        TransferOneNode(node->r_.node);
    if (node->l_.node)
        TransferOneNode(node->l_.node);

    node->index_ = engine_node_idx++;

    ::BSPNode &en      = cached_nodes[node->index_];
    en.divider.x       = (float)node->x_;
    en.divider.y       = (float)node->y_;
    en.divider.delta_x = (float)node->dx_;
    en.divider.delta_y = (float)node->dy_;
    en.divider_length  = PointToDistance(0, 0, en.divider.delta_x, en.divider.delta_y);

    en.bounding_boxes[0][0] = (float)node->r_.bounds.maximum_y;
    en.bounding_boxes[0][1] = (float)node->r_.bounds.minimum_y;
    en.bounding_boxes[0][2] = (float)node->r_.bounds.minimum_x;
    en.bounding_boxes[0][3] = (float)node->r_.bounds.maximum_x;

    en.bounding_boxes[1][0] = (float)node->l_.bounds.maximum_y;
    en.bounding_boxes[1][1] = (float)node->l_.bounds.minimum_y;
    en.bounding_boxes[1][2] = (float)node->l_.bounds.minimum_x;
    en.bounding_boxes[1][3] = (float)node->l_.bounds.maximum_x;

    if (node->r_.node)
        en.children[0] = (unsigned int)node->r_.node->index_;
    else if (node->r_.subsec)
    {
        en.children[0] = (unsigned int)(node->r_.subsec->index_ | 0x80000000U);
    }
    else
        FatalError("AJBSP: Bad right child in node %d\n", node->index_);

    if (node->l_.node)
        en.children[1] = (unsigned int)node->l_.node->index_;
    else if (node->l_.subsec)
    {
        en.children[1] = (unsigned int)(node->l_.subsec->index_ | 0x80000000U);
    }
    else
        FatalError("AJBSP: Bad left child in node %d\n", node->index_);
}

static void BuildCachedFromNodes(Node *root_node)
{
    cached_vertexes.clear();
    cached_segs.clear();
    cached_subsectors.clear();
    cached_nodes.clear();

    cached_vertexes.resize(num_new_vert);

    for (int i = 0; i < (int)level_vertices.size(); i++)
    {
        const Vertex *v = level_vertices[i];
        if (!v->is_new_)
            continue;
        cached_vertexes[v->index_].x = (float)v->x_;
        cached_vertexes[v->index_].y = (float)v->y_;
    }

    cached_segs.resize(level_segs.size());

    cached_subsectors.resize(level_subsecs.size());
    if (cached_subsectors.empty())
        FatalError("AJBSP: No subsectors produced.\n");

    for (int i = 0; i < (int)level_subsecs.size(); i++)
    {
        const Subsector *asub = level_subsecs[i];

        if (asub->seg_count_ == 0)
            FatalError("AJBSP: Subsector %d has no segs.\n", i);

        cached_subsectors[i].first_seg = asub->seg_list_->index_;
        cached_subsectors[i].seg_count = asub->seg_count_;

        for (const Seg *aseg = asub->seg_list_; aseg; aseg = aseg->next_)
        {
            CachedSeg &cs = cached_segs[aseg->index_];

            cs.vertex_1       = aseg->start_->index_;
            cs.vertex_1_is_gl = aseg->start_->is_new_ ? 1 : 0;
            cs.side           = (uint8_t)aseg->side_;
            cs.partner        = aseg->partner_ ? aseg->partner_->index_ : -1;
            cs.linedef        = aseg->linedef_ ? aseg->linedef_->index : -1;
        }
    }

    cached_nodes.resize(level_nodes.size());

    engine_node_idx = 0;
    if (root_node != nullptr)
        TransferOneNode(root_node);

    if (engine_node_idx != (int)cached_nodes.size())
        FatalError("AJBSP: Node count mismatch (%d != %d)\n", engine_node_idx, (int)cached_nodes.size());
}

static void ExpandCachedNodes(void)
{
    ::level_gl_vertexes = new ::Vertex[cached_vertexes.size()];

    for (size_t i = 0; i < cached_vertexes.size(); i++)
    {
        ::Vertex &ev = ::level_gl_vertexes[i];
        ev.X         = cached_vertexes[i].x;
        ev.Y         = cached_vertexes[i].y;
        ev.Z         = -40000.0f;
        ev.W         = 40000.0f;
    }

    ::total_level_segs = (int)cached_segs.size();
    ::level_segs       = new ::Seg[::total_level_segs];
    EPI_CLEAR_MEMORY(::level_segs, ::Seg, ::total_level_segs);

    ::total_level_subsectors = (int)cached_subsectors.size();
    ::level_subsectors       = new ::Subsector[::total_level_subsectors];
    EPI_CLEAR_MEMORY(::level_subsectors, ::Subsector, ::total_level_subsectors);

    for (int i = 0; i < ::total_level_subsectors; i++)
    {
        ::Subsector *ess           = &::level_subsectors[i];
        int          first_eseg    = cached_subsectors[i].first_seg;
        int          countsegs     = cached_subsectors[i].seg_count;

        ::Seg **prevptr = &ess->segs;

        for (int j = 0; j < countsegs; j++)
        {
            const CachedSeg &cs   = cached_segs[first_eseg + j];
            ::Seg           *eseg = &::level_segs[first_eseg + j];

            if (cs.vertex_1_is_gl)
                eseg->vertex_1 = &::level_gl_vertexes[cs.vertex_1];
            else
                eseg->vertex_1 = &::level_vertexes[cs.vertex_1];

            eseg->side    = cs.side;
            eseg->partner = (cs.partner >= 0) ? &::level_segs[cs.partner] : nullptr;

            eseg->front_sector = eseg->back_sector = nullptr;

            if (cs.linedef < 0)
            {
                eseg->miniseg = true;
            }
            else
            {
                if (cs.linedef >= ::total_level_lines)
                    FatalError("AJBSP: seg %d references invalid linedef %d.\n", first_eseg + j, cs.linedef);

                eseg->miniseg = false;
                eseg->linedef = &::level_lines[cs.linedef];

                float sx = eseg->side ? eseg->linedef->vertex_2->X : eseg->linedef->vertex_1->X;
                float sy = eseg->side ? eseg->linedef->vertex_2->Y : eseg->linedef->vertex_1->Y;

                eseg->sidedef = eseg->linedef->side[eseg->side];
                if (!eseg->sidedef)
                    FatalError("AJBSP: seg %d has no sidedef.\n", first_eseg + j);

                eseg->front_sector = eseg->sidedef->sector;

                if (eseg->linedef->flags & kLineFlagTwoSided)
                {
                    ::Side *other = eseg->linedef->side[eseg->side ^ 1];
                    if (other)
                        eseg->back_sector = other->sector;
                }

                eseg->offset = PointToDistance(sx, sy, eseg->vertex_1->X, eseg->vertex_1->Y);
            }

            eseg->subsector_next  = (::Seg *)-3;
            eseg->back_subsector  = nullptr;
            eseg->front_subsector = ess;

            *prevptr = eseg;
            prevptr  = &eseg->subsector_next;
        }
        *prevptr = nullptr;

        for (int j = 0; j < countsegs; j++)
        {
            ::Seg *cur      = &::level_segs[first_eseg + j];
            int    next_idx = (j == countsegs - 1) ? first_eseg : first_eseg + j + 1;
            cur->vertex_2   = ::level_segs[next_idx].vertex_1;
            cur->angle      = PointToAngle(cur->vertex_1->X, cur->vertex_1->Y, cur->vertex_2->X, cur->vertex_2->Y);
            cur->length     = PointToDistance(cur->vertex_1->X, cur->vertex_1->Y, cur->vertex_2->X, cur->vertex_2->Y);
        }
    }

    ::total_level_nodes = (int)cached_nodes.size();
    ::level_nodes       = new ::BSPNode[::total_level_nodes + 1];
    EPI_CLEAR_MEMORY(::level_nodes, ::BSPNode, ::total_level_nodes);

    for (int i = 0; i < ::total_level_nodes; i++)
    {
        ::BSPNode &en = ::level_nodes[i];
        en            = cached_nodes[i];

        for (int c = 0; c < 2; c++)
        {
            if (en.children[c] & kLeafSubsector)
            {
                int sub = (int)(en.children[c] & ~kLeafSubsector);
                ::level_subsectors[sub].bounding_box = &en.bounding_boxes[c][0];
            }
        }
    }
}



static constexpr char     kNodeCacheMagic[4]      = {'E', 'C', 'N', '1'};
static constexpr uint32_t kNodeCacheVersion       = 1;
static constexpr int      kNodeCacheCompression   = MZ_BEST_SPEED;
static constexpr uint32_t kNodeCacheHeaderSize    = 12;
static constexpr uint32_t kNodeCacheDirectorySize = 32;
static constexpr size_t   kNodeCacheNameSize      = 12;

struct NodeCacheEntry
{
    char                 name[kNodeCacheNameSize];
    uint32_t             geometry_crc;
    uint32_t             raw_size;
    uint32_t             checksum;
    std::vector<uint8_t> deflated;
};

static std::vector<NodeCacheEntry> node_cache_entries;

static void AppendBytes(std::vector<uint8_t> &out, const void *data, size_t bytes)
{
    const uint8_t *src = (const uint8_t *)data;
    out.insert(out.end(), src, src + bytes);
}

template <typename T> static void AppendArray(std::vector<uint8_t> &out, const std::vector<T> &array)
{
    uint32_t count = (uint32_t)array.size();
    AppendBytes(out, &count, sizeof(count));
    if (count)
        AppendBytes(out, array.data(), (size_t)count * sizeof(T));
}

template <typename T> static bool ReadArray(const uint8_t *&pos, const uint8_t *end, std::vector<T> &array)
{
    if ((size_t)(end - pos) < sizeof(uint32_t))
        return false;

    uint32_t count;
    memcpy(&count, pos, sizeof(count));
    pos += sizeof(count);

    if ((size_t)(end - pos) < (size_t)count * sizeof(T))
        return false;

    array.resize(count);
    if (count)
        memcpy(array.data(), pos, (size_t)count * sizeof(T));
    pos += (size_t)count * sizeof(T);

    return true;
}

void BeginNodeCache(void)
{
    node_cache_entries.clear();
}

void ClearNodeCache(void)
{
    node_cache_entries.clear();
    node_cache_entries.shrink_to_fit();
}

bool AddNodeCacheLevel(const char *name, uint32_t geometry_crc)
{
    if (name == nullptr || name[0] == 0 || strlen(name) >= kNodeCacheNameSize)
        return false;

    std::vector<uint8_t> payload;

    AppendArray(payload, cached_vertexes);
    AppendArray(payload, cached_segs);
    AppendArray(payload, cached_subsectors);
    AppendArray(payload, cached_nodes);

    mz_ulong bound = mz_compressBound((mz_ulong)payload.size());

    std::vector<uint8_t> deflated(bound);
    mz_ulong             deflated_size = bound;

    if (mz_compress2(deflated.data(), &deflated_size, payload.data(), (mz_ulong)payload.size(),
                     kNodeCacheCompression) != MZ_OK)
        return false;

    deflated.resize(deflated_size);

    node_cache_entries.emplace_back();
    NodeCacheEntry &entry = node_cache_entries.back();

    EPI_CLEAR_MEMORY(entry.name, char, kNodeCacheNameSize);
    epi::CStringCopyMax(entry.name, name, kNodeCacheNameSize - 1);

    entry.geometry_crc = geometry_crc;
    entry.raw_size     = (uint32_t)payload.size();
    entry.checksum     = (uint32_t)mz_adler32(MZ_ADLER32_INIT, payload.data(), payload.size());
    entry.deflated.swap(deflated);

    return true;
}

bool WriteNodeCache(const std::string &path)
{
    uint32_t level_count = (uint32_t)node_cache_entries.size();

    epi::File *file = epi::FileOpen(path, epi::kFileAccessWrite | epi::kFileAccessBinary);

    if (!file)
    {
        epi::FileDelete(path);
        return false;
    }

    uint32_t version = kNodeCacheVersion;

    bool ok = true;
    ok      = ok && file->Write(kNodeCacheMagic, 4) == 4;
    ok      = ok && file->Write(&version, sizeof(version)) == sizeof(version);
    ok      = ok && file->Write(&level_count, sizeof(level_count)) == sizeof(level_count);

    uint32_t offset = kNodeCacheHeaderSize + level_count * kNodeCacheDirectorySize;

    for (uint32_t i = 0; ok && i < level_count; i++)
    {
        const NodeCacheEntry &entry = node_cache_entries[i];

        uint32_t compressed_size = (uint32_t)entry.deflated.size();

        ok = ok && file->Write(entry.name, kNodeCacheNameSize) == kNodeCacheNameSize;
        ok = ok && file->Write(&entry.geometry_crc, sizeof(uint32_t)) == sizeof(uint32_t);
        ok = ok && file->Write(&offset, sizeof(uint32_t)) == sizeof(uint32_t);
        ok = ok && file->Write(&compressed_size, sizeof(uint32_t)) == sizeof(uint32_t);
        ok = ok && file->Write(&entry.raw_size, sizeof(uint32_t)) == sizeof(uint32_t);
        ok = ok && file->Write(&entry.checksum, sizeof(uint32_t)) == sizeof(uint32_t);

        offset += compressed_size;
    }

    for (uint32_t i = 0; ok && i < level_count; i++)
    {
        const NodeCacheEntry &entry = node_cache_entries[i];

        unsigned int compressed_size = (unsigned int)entry.deflated.size();

        ok = ok && file->Write(entry.deflated.data(), compressed_size) == compressed_size;
    }

    delete file;

    if (!ok)
        epi::FileDelete(path);

    return ok;
}

bool IsNodeCacheCurrent(const std::string &path)
{
    epi::File *file = epi::FileOpen(path, epi::kFileAccessRead | epi::kFileAccessBinary);

    if (!file)
        return false;

    uint8_t header[kNodeCacheHeaderSize];
    bool    ok = file->Read(header, kNodeCacheHeaderSize) == kNodeCacheHeaderSize;

    delete file;

    if (!ok || memcmp(header, kNodeCacheMagic, 4) != 0)
        return false;

    uint32_t version;
    memcpy(&version, header + 4, sizeof(version));

    return version == kNodeCacheVersion;
}

NodeCacheResult LoadNodeCacheLevel(const std::string &path, const char *name, uint32_t geometry_crc)
{
    if (name == nullptr || name[0] == 0)
        return kNodeCacheLevelMissing;

    epi::File *file = epi::FileOpen(path, epi::kFileAccessRead | epi::kFileAccessBinary);

    if (!file)
        return kNodeCacheCorrupt;

    uint8_t header[kNodeCacheHeaderSize];

    if (file->Read(header, kNodeCacheHeaderSize) != kNodeCacheHeaderSize ||
        memcmp(header, kNodeCacheMagic, 4) != 0)
    {
        delete file;
        return kNodeCacheCorrupt;
    }

    uint32_t version, level_count;
    memcpy(&version, header + 4, sizeof(version));
    memcpy(&level_count, header + 8, sizeof(level_count));

    if (version != kNodeCacheVersion || level_count == 0)
    {
        delete file;
        return kNodeCacheCorrupt;
    }

    std::vector<uint8_t> directory(level_count * kNodeCacheDirectorySize);

    if (file->Read(directory.data(), (unsigned int)directory.size()) != (unsigned int)directory.size())
    {
        delete file;
        return kNodeCacheCorrupt;
    }

    uint32_t offset = 0, compressed_size = 0, raw_size = 0, checksum = 0;
    bool     name_found  = false;
    bool     entry_valid = false;

    for (uint32_t i = 0; i < level_count; i++)
    {
        const uint8_t *dir_entry = directory.data() + i * kNodeCacheDirectorySize;

        char entry_name[kNodeCacheNameSize + 1];
        memcpy(entry_name, dir_entry, kNodeCacheNameSize);
        entry_name[kNodeCacheNameSize] = 0;

        if (epi::StringCaseCompareASCII(entry_name, name) != 0)
            continue;

        name_found = true;

        uint32_t entry_crc;
        memcpy(&entry_crc, dir_entry + kNodeCacheNameSize, sizeof(uint32_t));

        if (entry_crc != geometry_crc)
            break;

        memcpy(&offset, dir_entry + kNodeCacheNameSize + 4, sizeof(uint32_t));
        memcpy(&compressed_size, dir_entry + kNodeCacheNameSize + 8, sizeof(uint32_t));
        memcpy(&raw_size, dir_entry + kNodeCacheNameSize + 12, sizeof(uint32_t));
        memcpy(&checksum, dir_entry + kNodeCacheNameSize + 16, sizeof(uint32_t));

        entry_valid = true;
        break;
    }

    if (!name_found)
    {
        delete file;
        return kNodeCacheLevelMissing;
    }

    if (!entry_valid || compressed_size == 0 || raw_size == 0 ||
        (int)(offset + compressed_size) > file->GetLength() || !file->Seek((int)offset, epi::File::kSeekpointStart))
    {
        delete file;
        return kNodeCacheCorrupt;
    }

    std::vector<uint8_t> deflated(compressed_size);

    bool read_ok = file->Read(deflated.data(), compressed_size) == compressed_size;

    delete file;

    if (!read_ok)
        return kNodeCacheCorrupt;

    std::vector<uint8_t> payload(raw_size);
    mz_ulong             out_size = raw_size;

    if (mz_uncompress(payload.data(), &out_size, deflated.data(), (mz_ulong)compressed_size) != MZ_OK)
        return kNodeCacheCorrupt;

    if (out_size != raw_size)
        return kNodeCacheCorrupt;

    if ((uint32_t)mz_adler32(MZ_ADLER32_INIT, payload.data(), payload.size()) != checksum)
        return kNodeCacheCorrupt;

    const uint8_t *pos = payload.data();
    const uint8_t *end = payload.data() + payload.size();

    if (!ReadArray(pos, end, cached_vertexes) || !ReadArray(pos, end, cached_segs) ||
        !ReadArray(pos, end, cached_subsectors) || !ReadArray(pos, end, cached_nodes))
        return kNodeCacheCorrupt;

    if (cached_subsectors.empty() || cached_segs.empty())
        return kNodeCacheCorrupt;

    ExpandCachedNodes();

    return kNodeCacheOK;
}

void BuildNodes(const InputLevel &input)
{
    alloc_mutex = CreateSystemMutex();

    Node      *root_node = nullptr;
    Subsector *root_sub  = nullptr;

    LoadLevelFromInput(input);

    if (num_real_lines == 0)
        FatalError("AJBSP: Level has no valid linedefs.\n");

    BoundingBox dummy;

    Seg        *list = CreateSegs();
    BuildResult ret  = BuildNodesRoot(list, &dummy, &root_node, &root_sub);

    if (ret != kBuildOK)
        FatalError("AJBSP: Failed to build BSP nodes.\n");

    LogDebug("AJBSP: Built %zu NODES, %zu SSECTORS, %zu SEGS, %d VERTEXES\n", level_nodes.size(),
             level_subsecs.size(), level_segs.size(), num_old_vert + num_new_vert);

    if (root_node != nullptr)
    {
        LogDebug("AJBSP: Heights of subtrees: %d / %d\n", ComputeBSPHeight(root_node->r_.node),
                 ComputeBSPHeight(root_node->l_.node));
    }

    ClockwiseBSPTree();
    RepairPartnerLinks();
    BuildCachedFromNodes(root_node);

    FreeLevel();

    DestroySystemMutex(alloc_mutex);
    alloc_mutex = nullptr;
}

} // namespace ajbsp

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
