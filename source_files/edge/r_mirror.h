#pragma once

#include "i_defs_gl.h"
#include "r_defs.h"
#include "r_gldefs.h"
#include "r_misc.h"
#include "r_state.h"

constexpr uint8_t kMaximumMirrors = 3;

struct MirrorViewState
{
    const DrawMirror *mirror;

    int32_t depth;

    bool reflective;

    float xy_scale;
    float z_scale;

    HMM_Vec2 sprite_right;
    HMM_Vec2 sprite_forward;

    HMM_Vec3 view_position;
    HMM_Vec3 view_plane;
};

extern MirrorViewState mirror_view;

void ResetMirrorView(void);

void InstallMirrorNearPlane(const DrawMirror *mir);

void RenderMirror(DrawMirror *mir);

inline void ClipPlaneHorizontalLine(GLdouble *p, const HMM_Vec2 &s, const HMM_Vec2 &e)
{
    p[0] = e.Y - s.Y;
    p[1] = s.X - e.X;
    p[2] = 0.0f;
    p[3] = e.X * s.Y - s.X * e.Y;
}

class MirrorSet
{
  public:
    void Transform(int32_t index, float &x, float &y)
    {
        active_mirrors_[index].Transform(x, y);
    }

    bool IsPortal(int32_t index)
    {
        return active_mirrors_[index].draw_mirror_->is_portal;
    }

    Seg *GetSeg(int32_t index)
    {
        return active_mirrors_[index].draw_mirror_->seg;
    }

    int32_t TotalActive()
    {
        return active_;
    }

    DrawMirror *InnermostMirror()
    {
        return (active_ > 0) ? active_mirrors_[active_ - 1].draw_mirror_ : nullptr;
    }

    void Coordinate(float &x, float &y)
    {
        for (int i = active_ - 1; i >= 0; i--)
            active_mirrors_[i].Transform(x, y);
    }

    void Angle(BAMAngle &ang)
    {
        for (int i = active_ - 1; i >= 0; i--)
            active_mirrors_[i].Turn(ang);
    }

    HMM_Mat4 Matrix(void)
    {
        HMM_Mat4 result = HMM_M4D(1.0f);

        for (int i = 0; i < active_; i++)
            result = HMM_MulM4(result, active_mirrors_[i].Matrix());

        return result;
    }

    float XYScale(void)
    {
        float result = 1.0f;

        for (int i = active_ - 1; i >= 0; i--)
            result *= active_mirrors_[i].xy_scale_;

        return result;
    }

    float ZScale(void)
    {
        float result = 1.0f;

        for (int i = active_ - 1; i >= 0; i--)
            result *= active_mirrors_[i].z_scale_;

        return result;
    }

    bool Reflective(void)
    {
        if (active_ == 0)
            return false;

        bool result = false;

        for (int i = active_ - 1; i >= 0; i--)
            if (!active_mirrors_[i].draw_mirror_->is_portal)
                result = !result;

        return result;
    }
    bool SegOnPortal(const Seg *seg)
    {
        if (active_ == 0)
            return false;

        if (seg->miniseg)
            return false;

        const DrawMirror *def = active_mirrors_[active_ - 1].draw_mirror_;

        if (def->is_portal)
        {
            if (seg->linedef == def->seg->linedef->portal_pair)
                return true;
        }
        else // mirror
        {
            if (seg->linedef == def->seg->linedef)
                return true;
        }

        return false;
    }

    void PushSubsector(int32_t index, DrawSubsector *subsector)
    {
        active_mirrors_[index].draw_mirror_->draw_subsectors.push_back(subsector);
    }

    void PushThing(int32_t index, DrawThing *thing)
    {
        active_mirrors_[index].draw_mirror_->draw_things.push_back(thing);
    }

    void Push(DrawMirror *mir)
    {
        EPI_ASSERT(mir);
        EPI_ASSERT(mir->seg);

        EPI_ASSERT(active_ < kMaximumMirrors);

        active_mirrors_[active_].draw_mirror_ = mir;
        active_mirrors_[active_].Compute();

        active_++;

        HMM_Mat4 view_matrix = Matrix();

        mir->local_matrix = active_mirrors_[active_ - 1].Matrix();
        mir->view_matrix  = view_matrix;
        mir->reflective   = Reflective();
        mir->xy_scale     = XYScale();
        mir->z_scale      = ZScale();

        ComputeViewSpace(mir, view_matrix);
        ComputeNearPlane(mir, view_matrix);
    }

    void Pop()
    {
        EPI_ASSERT(active_ > 0);

        active_--;
    }

  private:
    class MirrorInfo
    {
      public:
        DrawMirror *draw_mirror_;

        float xc_, xx_, xy_; // x' = xc + x*xx + y*xy
        float yc_, yx_, yy_; // y' = yc + x*yx + y*yy
        float zc_, z_scale_; // z' = zc + z*z_scale

        float xy_scale_;

        BAMAngle tc_;

        void ComputeMirror()
        {
            Seg *seg = draw_mirror_->seg;

            float sdx = seg->vertex_2->X - seg->vertex_1->X;
            float sdy = seg->vertex_2->Y - seg->vertex_1->Y;

            float len_p2 = seg->length * seg->length;

            float A = (sdx * sdx - sdy * sdy) / len_p2;
            float B = (sdx * sdy * 2.0) / len_p2;

            xx_ = A;
            xy_ = B;
            yx_ = B;
            yy_ = -A;

            xc_ = seg->vertex_1->X * (1.0 - A) - seg->vertex_1->Y * B;
            yc_ = seg->vertex_1->Y * (1.0 + A) - seg->vertex_1->X * B;

            tc_ = seg->angle << 1;

            zc_       = 0;
            z_scale_  = 1.0f;
            xy_scale_ = 1.0f;
        }

        float GetAlong(const Line *ld, float x, float y)
        {
            if (fabs(ld->delta_x) >= fabs(ld->delta_y))
                return (x - ld->vertex_1->X) / ld->delta_x;
            else
                return (y - ld->vertex_1->Y) / ld->delta_y;
        }

        void ComputePortal()
        {
            Seg  *seg   = draw_mirror_->seg;
            Line *other = seg->linedef->portal_pair;

            EPI_ASSERT(other);

            float ax1 = seg->vertex_1->X;
            float ay1 = seg->vertex_1->Y;

            float ax2 = seg->vertex_2->X;
            float ay2 = seg->vertex_2->Y;

            // find corresponding coords on partner line
            float along1 = GetAlong(seg->linedef, ax1, ay1);
            float along2 = GetAlong(seg->linedef, ax2, ay2);

            float bx1 = other->vertex_2->X - other->delta_x * along1;
            float by1 = other->vertex_2->Y - other->delta_y * along1;

            float bx2 = other->vertex_2->X - other->delta_x * along2;
            float by2 = other->vertex_2->Y - other->delta_y * along2;

            // compute rotation angle
            tc_ = kBAMAngle180 + PointToAngle(0, 0, other->delta_x, other->delta_y) - seg->angle;

            xx_ = epi::BAMCos(tc_);
            xy_ = epi::BAMSin(tc_);
            yx_ = -epi::BAMSin(tc_);
            yy_ = epi::BAMCos(tc_);

            // scaling
            float a_len = seg->length;
            float b_len = PointToDistance(bx1, by1, bx2, by2);

            xy_scale_ = a_len / HMM_MAX(1, b_len);

            xx_ *= xy_scale_;
            xy_ *= xy_scale_;
            yx_ *= xy_scale_;
            yy_ *= xy_scale_;

            // translation
            xc_ = ax1 - bx1 * xx_ - by1 * xy_;
            yc_ = ay1 - bx1 * yx_ - by1 * yy_;

            // heights
            float a_h = (seg->front_sector->interpolated_ceiling_height - seg->front_sector->interpolated_floor_height);
            float b_h =
                (other->front_sector->interpolated_ceiling_height - other->front_sector->interpolated_floor_height);

            z_scale_ = a_h / HMM_MAX(1, b_h);
            zc_      = seg->front_sector->interpolated_floor_height -
                  other->front_sector->interpolated_floor_height * z_scale_;
        }

        void Compute()
        {
            if (draw_mirror_->is_portal)
                ComputePortal();
            else
                ComputeMirror();
        }

        HMM_Mat4 Matrix() const
        {
            HMM_Mat4 m = {};

            m.Elements[0][0] = xx_;
            m.Elements[1][0] = xy_;
            m.Elements[3][0] = xc_;

            m.Elements[0][1] = yx_;
            m.Elements[1][1] = yy_;
            m.Elements[3][1] = yc_;

            m.Elements[2][2] = z_scale_;
            m.Elements[3][2] = zc_;

            m.Elements[3][3] = 1.0f;

            return m;
        }

        void Transform(float &x, float &y)
        {
            float tx = x, ty = y;

            x = xc_ + tx * xx_ + ty * xy_;
            y = yc_ + tx * yx_ + ty * yy_;
        }

        void Turn(BAMAngle &ang)
        {
            ang = (draw_mirror_->is_portal) ? (ang - tc_) : (tc_ - ang);
        }
    };

    void ComputeViewSpace(DrawMirror *mir, const HMM_Mat4 &view_matrix)
    {
        float xx = view_matrix.Elements[0][0];
        float xy = view_matrix.Elements[1][0];
        float yx = view_matrix.Elements[0][1];
        float yy = view_matrix.Elements[1][1];

        float zs = view_matrix.Elements[2][2];

        float xc = view_matrix.Elements[3][0];
        float yc = view_matrix.Elements[3][1];
        float zc = view_matrix.Elements[3][2];

        float determinant = xx * yy - xy * yx;

        HMM_Vec2 right   = {{view_sine, -view_cosine}};
        HMM_Vec2 forward = {{view_cosine, view_sine}};

        mir->view_position = {{view_x, view_y, view_z}};
        mir->view_plane    = view_forward;

        if (epi::AlmostEquals(determinant, 0.0f) || epi::AlmostEquals(zs, 0.0f))
        {
            mir->sprite_right   = right;
            mir->sprite_forward = forward;
            return;
        }

        float ixx = yy / determinant;
        float ixy = -xy / determinant;
        float iyx = -yx / determinant;
        float iyy = xx / determinant;

        HMM_Vec2 inverse_right   = {{ixx * right.X + ixy * right.Y, iyx * right.X + iyy * right.Y}};
        HMM_Vec2 inverse_forward = {{ixx * forward.X + ixy * forward.Y, iyx * forward.X + iyy * forward.Y}};

        mir->sprite_right   = HMM_NormV2(inverse_right);
        mir->sprite_forward = HMM_NormV2(inverse_forward);

        float ox = view_x - xc;
        float oy = view_y - yc;

        mir->view_position = {{ixx * ox + ixy * oy, iyx * ox + iyy * oy, (view_z - zc) / zs}};

        mir->view_plane = {{xx * view_forward.X + yx * view_forward.Y, xy * view_forward.X + yy * view_forward.Y,
                            zs * view_forward.Z}};
    }

    void ComputeNearPlane(DrawMirror *mir, const HMM_Mat4 &view_matrix)
    {
        mir->near_plane = {{0.0f, 0.0f, 0.0f, 0.0f}};

        if (active_ == 0)
            return;

        MirrorInfo &inner = active_mirrors_[active_ - 1];

        HMM_Vec2 v1, v2;

        v1 = {{inner.draw_mirror_->seg->vertex_1->X, inner.draw_mirror_->seg->vertex_1->Y}};
        v2 = {{inner.draw_mirror_->seg->vertex_2->X, inner.draw_mirror_->seg->vertex_2->Y}};

        for (int k = active_ - 2; k >= 0; k--)
        {
            if (!active_mirrors_[k].draw_mirror_->is_portal)
            {
                HMM_Vec2 tmp;
                tmp = v1;
                v1  = v2;
                v2  = tmp;
            }

            active_mirrors_[k].Transform(v1.X, v1.Y);
            active_mirrors_[k].Transform(v2.X, v2.Y);
        }

        GLdouble p[4];

        ClipPlaneHorizontalLine(p, v2, v1);

        HMM_Vec4 plane = HMM_V4((float)p[0], (float)p[1], (float)p[2], (float)p[3]);

        for (int32_t e = 0; e < 4; e++)
            mir->near_plane.Elements[e] = HMM_DotV4(view_matrix.Columns[e], plane);
    }

    int32_t active_ = 0;

    MirrorInfo active_mirrors_[kMaximumMirrors];
};

extern MirrorSet active_mirror_set;
