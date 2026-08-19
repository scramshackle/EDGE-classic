#include "r_mdmesh.h"

#include "epi.h"
#include "epi_math.h"

HMM_Mat4 ModelBuildTransform(float xy_scale, float z_scale, float bias, const HMM_Vec2 &mouselook_x_matrix,
                             const HMM_Vec2 &mouselook_z_matrix, const HMM_Vec2 &rotation_x_matrix,
                             const HMM_Vec2 &rotation_y_matrix, float x, float y, float z)
{
    float scale_x = xy_scale;
    float scale_y = xy_scale;
    float scale_z = z_scale;

    HMM_Mat4 result = {};

    result.Elements[0][0] = scale_x * mouselook_x_matrix.X * rotation_x_matrix.X;
    result.Elements[0][1] = scale_x * mouselook_x_matrix.X * rotation_y_matrix.X;
    result.Elements[0][2] = scale_x * mouselook_z_matrix.X;
    result.Elements[0][3] = 0.0f;

    result.Elements[1][0] = scale_y * rotation_x_matrix.Y;
    result.Elements[1][1] = scale_y * rotation_y_matrix.Y;
    result.Elements[1][2] = 0.0f;
    result.Elements[1][3] = 0.0f;

    result.Elements[2][0] = scale_z * mouselook_x_matrix.Y * rotation_x_matrix.X;
    result.Elements[2][1] = scale_z * mouselook_x_matrix.Y * rotation_y_matrix.X;
    result.Elements[2][2] = scale_z * mouselook_z_matrix.Y;
    result.Elements[2][3] = 0.0f;

    result.Elements[3][0] = x + bias * scale_z * mouselook_x_matrix.Y * rotation_x_matrix.X;
    result.Elements[3][1] = y + bias * scale_z * mouselook_x_matrix.Y * rotation_y_matrix.X;
    result.Elements[3][2] = z + bias * scale_z * mouselook_z_matrix.Y;
    result.Elements[3][3] = 1.0f;

    return result;
}

static bool ModelMeshPointsMatch(const ModelMeshVertex &a, const ModelMeshVertex &b)
{
    return a.vert_idx == b.vert_idx && epi::AlmostEquals(a.skin_s, b.skin_s) && epi::AlmostEquals(a.skin_t, b.skin_t);
}

void ModelMeshBuild(ModelMesh &mesh, const ModelMeshVertex *points, int total_points, const int *triangle_points,
                    int total_triangles, int vertices_per_frame)
{
    mesh.vertices_.clear();
    mesh.indices_.clear();
    mesh.submeshes_.clear();

    if (!points || !triangle_points || total_triangles <= 0 || vertices_per_frame <= 0)
        return;

    mesh.vertices_.reserve((size_t)total_points);
    mesh.indices_.reserve((size_t)total_triangles * 3);

    std::vector<std::vector<int>> buckets;
    buckets.resize((size_t)vertices_per_frame);

    ModelMeshSubmesh current;

    current.first_vertex = 0;
    current.vertex_count = 0;
    current.first_index  = 0;
    current.index_count  = 0;

    int resolved[3];

    for (int t = 0; t < total_triangles; t++)
    {
        int needed = 0;

        for (int k = 0; k < 3; k++)
        {
            int point_index = triangle_points[t * 3 + k];

            if (point_index < 0 || point_index >= total_points)
                return;

            const ModelMeshVertex &point = points[point_index];

            if (point.vert_idx < 0 || point.vert_idx >= vertices_per_frame)
                return;

            resolved[k] = -1;

            const std::vector<int> &bucket = buckets[(size_t)point.vert_idx];

            for (size_t b = 0; b < bucket.size(); b++)
            {
                if (ModelMeshPointsMatch(mesh.vertices_[(size_t)bucket[b]], point))
                {
                    resolved[k] = bucket[b] - current.first_vertex;
                    break;
                }
            }

            if (resolved[k] < 0)
            {
                bool already_counted = false;

                for (int prior = 0; prior < k; prior++)
                {
                    int prior_point = triangle_points[t * 3 + prior];

                    if (resolved[prior] < 0 && ModelMeshPointsMatch(points[prior_point], point))
                    {
                        already_counted = true;
                        break;
                    }
                }

                if (!already_counted)
                    needed++;
            }
        }

        if (current.vertex_count > 0 && current.vertex_count + needed > kModelMeshMaximumVertices)
        {
            mesh.submeshes_.push_back(current);

            for (size_t b = 0; b < buckets.size(); b++)
                buckets[b].clear();

            current.first_vertex = (int)mesh.vertices_.size();
            current.vertex_count = 0;
            current.first_index  = (int)mesh.indices_.size();
            current.index_count  = 0;

            for (int k = 0; k < 3; k++)
                resolved[k] = -1;
        }

        for (int k = 0; k < 3; k++)
        {
            if (resolved[k] < 0)
            {
                const ModelMeshVertex &point = points[triangle_points[t * 3 + k]];

                const std::vector<int> &bucket = buckets[(size_t)point.vert_idx];

                for (size_t b = 0; b < bucket.size(); b++)
                {
                    if (ModelMeshPointsMatch(mesh.vertices_[(size_t)bucket[b]], point))
                    {
                        resolved[k] = bucket[b] - current.first_vertex;
                        break;
                    }
                }
            }

            if (resolved[k] < 0)
            {
                const ModelMeshVertex &point = points[triangle_points[t * 3 + k]];

                resolved[k] = current.vertex_count;

                mesh.vertices_.push_back(point);
                buckets[(size_t)point.vert_idx].push_back((int)mesh.vertices_.size() - 1);

                current.vertex_count++;
            }

            mesh.indices_.push_back((uint16_t)resolved[k]);
            current.index_count++;
        }
    }

    if (current.index_count > 0)
        mesh.submeshes_.push_back(current);
}
