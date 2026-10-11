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

    epi::Vec2 sprite_right;
    epi::Vec2 sprite_forward;

    epi::Vec3 view_position;
    epi::Vec3 view_plane;
};

extern MirrorViewState mirror_view;

void ResetMirrorView(void);

void InstallMirrorNearPlane(const DrawMirror *mir);

void RenderMirror(DrawMirror *mir);

void MirrorRenderStatsBeginFrame(void);
void MirrorRenderStatsRead(int *count, int *top_count, uint64_t *top_us);

inline void ClipPlaneHorizontalLine(GLdouble *p, const epi::Vec2 &s, const epi::Vec2 &e)
{
    p[0] = e.y - s.y;
    p[1] = s.x - e.x;
    p[2] = 0.0f;
    p[3] = e.x * s.y - s.x * e.y;
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

    LineSide *GetLineSide(int32_t index)
    {
        return active_mirrors_[index].draw_mirror_->line_side;
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

    epi::Mat4 Matrix(void)
    {
        epi::Mat4 result = epi::IdentityMatrix();

        for (int i = 0; i < active_; i++)
            result = epi::MultiplyMatrices(result, active_mirrors_[i].Matrix());

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
    bool LineSideOnPortal(const LineSide *line_side)
    {
        if (active_ == 0)
            return false;

        const DrawMirror *def = active_mirrors_[active_ - 1].draw_mirror_;

        if (def->is_portal)
        {
            if (line_side->linedef == def->line_side->linedef->portal_pair)
                return true;
        }
        else // mirror
        {
            if (line_side->linedef == def->line_side->linedef)
                return true;
        }

        return false;
    }

    void PushSector(int32_t index, DrawSector *sector)
    {
        active_mirrors_[index].draw_mirror_->draw_sectors.push_back(sector);
    }

    void PushThing(int32_t index, DrawThing *thing)
    {
        active_mirrors_[index].draw_mirror_->draw_things.push_back(thing);
    }

    void PushMirror(int32_t index, DrawMirror *mirror)
    {
        active_mirrors_[index].draw_mirror_->draw_mirrors.push_back(mirror);
    }

    void Push(DrawMirror *mir)
    {
        EPI_ASSERT(mir);
        EPI_ASSERT(mir->line_side);

        EPI_ASSERT(active_ < kMaximumMirrors);

        active_mirrors_[active_].draw_mirror_ = mir;
        active_mirrors_[active_].Compute();

        active_++;

        epi::Mat4 view_matrix = Matrix();

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
            LineSide *line_side = draw_mirror_->line_side;

            float sdx = line_side->vertex_2->x - line_side->vertex_1->x;
            float sdy = line_side->vertex_2->y - line_side->vertex_1->y;

            float len_p2 = line_side->length * line_side->length;

            float A = (sdx * sdx - sdy * sdy) / len_p2;
            float B = (sdx * sdy * 2.0) / len_p2;

            xx_ = A;
            xy_ = B;
            yx_ = B;
            yy_ = -A;

            xc_ = line_side->vertex_1->x * (1.0 - A) - line_side->vertex_1->y * B;
            yc_ = line_side->vertex_1->y * (1.0 + A) - line_side->vertex_1->x * B;

            tc_ = line_side->angle << 1;

            zc_       = 0;
            z_scale_  = 1.0f;
            xy_scale_ = 1.0f;
        }

        float GetAlong(const Line *ld, float x, float y)
        {
            if (fabs(ld->delta_x) >= fabs(ld->delta_y))
                return (x - ld->vertex_1->x) / ld->delta_x;
            else
                return (y - ld->vertex_1->y) / ld->delta_y;
        }

        void ComputePortal()
        {
            LineSide *line_side = draw_mirror_->line_side;
            Line     *other     = line_side->linedef->portal_pair;

            EPI_ASSERT(other);

            float ax1 = line_side->vertex_1->x;
            float ay1 = line_side->vertex_1->y;

            float ax2 = line_side->vertex_2->x;
            float ay2 = line_side->vertex_2->y;

            // find corresponding coords on partner line
            float along1 = GetAlong(line_side->linedef, ax1, ay1);
            float along2 = GetAlong(line_side->linedef, ax2, ay2);

            float bx1 = other->vertex_2->x - other->delta_x * along1;
            float by1 = other->vertex_2->y - other->delta_y * along1;

            float bx2 = other->vertex_2->x - other->delta_x * along2;
            float by2 = other->vertex_2->y - other->delta_y * along2;

            // compute rotation angle
            tc_ = kBAMAngle180 + PointToAngle(0, 0, other->delta_x, other->delta_y) - line_side->angle;

            xx_ = epi::BAMCos(tc_);
            xy_ = epi::BAMSin(tc_);
            yx_ = -epi::BAMSin(tc_);
            yy_ = epi::BAMCos(tc_);

            // scaling
            float a_len = line_side->length;
            float b_len = PointToDistance(bx1, by1, bx2, by2);

            xy_scale_ = a_len / epi::Max(1.0f, b_len);

            xx_ *= xy_scale_;
            xy_ *= xy_scale_;
            yx_ *= xy_scale_;
            yy_ *= xy_scale_;

            // translation
            xc_ = ax1 - bx1 * xx_ - by1 * xy_;
            yc_ = ay1 - bx1 * yx_ - by1 * yy_;

            // heights
            float a_h = (line_side->front_sector->interpolated_ceiling_height -
                         line_side->front_sector->interpolated_floor_height);
            float b_h =
                (other->front_sector->interpolated_ceiling_height - other->front_sector->interpolated_floor_height);

            z_scale_ = a_h / epi::Max(1.0f, b_h);
            zc_      = line_side->front_sector->interpolated_floor_height -
                  other->front_sector->interpolated_floor_height * z_scale_;
        }

        void Compute()
        {
            if (draw_mirror_->is_portal)
                ComputePortal();
            else
                ComputeMirror();
        }

        epi::Mat4 Matrix() const
        {
            epi::Mat4 m = {};

            m.elements[0][0] = xx_;
            m.elements[1][0] = xy_;
            m.elements[3][0] = xc_;

            m.elements[0][1] = yx_;
            m.elements[1][1] = yy_;
            m.elements[3][1] = yc_;

            m.elements[2][2] = z_scale_;
            m.elements[3][2] = zc_;

            m.elements[3][3] = 1.0f;

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

    void ComputeViewSpace(DrawMirror *mir, const epi::Mat4 &view_matrix)
    {
        float xx = view_matrix.elements[0][0];
        float xy = view_matrix.elements[1][0];
        float yx = view_matrix.elements[0][1];
        float yy = view_matrix.elements[1][1];

        float zs = view_matrix.elements[2][2];

        float xc = view_matrix.elements[3][0];
        float yc = view_matrix.elements[3][1];
        float zc = view_matrix.elements[3][2];

        float determinant = xx * yy - xy * yx;

        epi::Vec2 right   = {view_sine, -view_cosine};
        epi::Vec2 forward = {view_cosine, view_sine};

        mir->view_position = {view_x, view_y, view_z};
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

        epi::Vec2 inverse_right   = {ixx * right.x + ixy * right.y, iyx * right.x + iyy * right.y};
        epi::Vec2 inverse_forward = {ixx * forward.x + ixy * forward.y, iyx * forward.x + iyy * forward.y};

        mir->sprite_right   = epi::NormalizeVector(inverse_right);
        mir->sprite_forward = epi::NormalizeVector(inverse_forward);

        float ox = view_x - xc;
        float oy = view_y - yc;

        mir->view_position = {ixx * ox + ixy * oy, iyx * ox + iyy * oy, (view_z - zc) / zs};

        mir->view_plane = {xx * view_forward.x + yx * view_forward.y, xy * view_forward.x + yy * view_forward.y,
                           zs * view_forward.z};
    }

    void ComputeNearPlane(DrawMirror *mir, const epi::Mat4 &view_matrix)
    {
        mir->near_plane = {};

        if (active_ == 0)
            return;

        MirrorInfo &inner = active_mirrors_[active_ - 1];

        epi::Vec2 v1, v2;

        v1 = {inner.draw_mirror_->line_side->vertex_1->x, inner.draw_mirror_->line_side->vertex_1->y};
        v2 = {inner.draw_mirror_->line_side->vertex_2->x, inner.draw_mirror_->line_side->vertex_2->y};

        for (int k = active_ - 2; k >= 0; k--)
        {
            if (!active_mirrors_[k].draw_mirror_->is_portal)
            {
                epi::Vec2 tmp;
                tmp = v1;
                v1  = v2;
                v2  = tmp;
            }

            active_mirrors_[k].Transform(v1.x, v1.y);
            active_mirrors_[k].Transform(v2.x, v2.y);
        }

        GLdouble p[4];

        ClipPlaneHorizontalLine(p, v2, v1);

        epi::Vec4 plane = epi::Vec4{(float)p[0], (float)p[1], (float)p[2], (float)p[3]};

        mir->near_plane.x = epi::DotProduct(epi::MatrixColumn(view_matrix, 0), plane);
        mir->near_plane.y = epi::DotProduct(epi::MatrixColumn(view_matrix, 1), plane);
        mir->near_plane.z = epi::DotProduct(epi::MatrixColumn(view_matrix, 2), plane);
        mir->near_plane.w = epi::DotProduct(epi::MatrixColumn(view_matrix, 3), plane);
    }

    int32_t active_ = 0;

    MirrorInfo active_mirrors_[kMaximumMirrors];
};

extern MirrorSet active_mirror_set;
