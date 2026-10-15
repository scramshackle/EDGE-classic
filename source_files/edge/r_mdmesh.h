#pragma once

#include <stdint.h>

#include <span>
#include <vector>

#include "epi_vector.h"

constexpr int kModelMeshMaximumVertices = 65536;

struct ModelMeshVertex
{
    int   vert_idx;
    float skin_s, skin_t;
};

struct ModelMeshSubmesh
{
    int first_vertex;
    int vertex_count;
    int first_index;
    int index_count;
};

class ModelMesh
{
  public:
    std::vector<ModelMeshVertex>  vertices_;
    std::vector<uint16_t>         indices_;
    std::vector<ModelMeshSubmesh> submeshes_;

    uint32_t gpu_handle_ = 0;

    int TotalVertices() const
    {
        return (int)vertices_.size();
    }

    int TotalIndices() const
    {
        return (int)indices_.size();
    }
};

void ModelMeshBuild(ModelMesh &mesh, std::span<const ModelMeshVertex> points, std::span<const int> triangle_points,
                    int vertices_per_frame);

epi::Mat4 ModelBuildTransform(float xy_scale, float z_scale, float bias, const epi::Vec2 &mouselook_x_matrix,
                              const epi::Vec2 &mouselook_z_matrix, const epi::Vec2 &rotation_x_matrix,
                              const epi::Vec2 &rotation_y_matrix, float x, float y, float z);
