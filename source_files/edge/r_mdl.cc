//----------------------------------------------------------------------------
//  MDL Models
//----------------------------------------------------------------------------
//
//  Copyright (c) 2023-2024 The EDGE Team.
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
//----------------------------------------------------------------------------
//
//  Based on "qfiles.h" and "anorms.h" from the GPL'd quake 2 source
//  release.  Copyright (C) 1997-2001 Id Software, Inc.
//
//  Based on MDL loading and rendering code (C) 2004 David Henry.
//
//----------------------------------------------------------------------------

#include <stddef.h>
#include <string.h>

#include <unordered_map>
#include <vector>

#include "ddf_types.h"
#include "dm_state.h" // EDGE_IMAGE_IS_SKY
#include "epi.h"
#include "epi_endian.h"
#include "epi_simd.h"
#include "epi_str_compare.h"
#include "g_game.h" //current_map
#include "i_defs_gl.h"
#include "im_data.h"
#include "n_network.h"
#include "p_blockmap.h"
#include "p_tick.h"
#include "r_colormap.h"
#include "r_effects.h"
#include "r_gldefs.h"
#include "r_image.h"
#include "r_mdcommon.h"
#include "r_mdl.h"
#include "r_mdmesh.h"
#include "r_mirror.h"
#include "r_misc.h"
#include "r_modes.h"
#include "r_shader.h"
#include "r_state.h"
#include "r_texgl.h"
#include "r_units.h"

// clamp cache used by runits to avoid an extremely expensive gl tex param
// lookup
extern std::unordered_map<GLuint, GLint> texture_clamp_t;

extern float ApproximateDistance(float dx, float dy, float dz);

extern ConsoleVariable draw_culling;
extern ConsoleVariable cull_fog_color;
extern ConsoleVariable fliplevels;
extern bool            need_to_draw_sky;

/*============== MDL FORMAT DEFINITIONS ====================*/

// format uses float pointing values, but to allow for endianness
// conversions they are represented here as unsigned integers.

// struct member naming deviates from the style guide to reflect
// MDL format documentation

static constexpr const char *kMDLIdentifier = "IDPO";
static constexpr uint8_t     kMDLVersion    = 6;

struct RawMDLHeader
{
    char ident[4];

    int32_t version;

    uint32_t scale_x;
    uint32_t scale_y;
    uint32_t scale_z;

    uint32_t trans_x;
    uint32_t trans_y;
    uint32_t trans_z;

    uint32_t boundingradius;

    uint32_t eyepos_x;
    uint32_t eyepos_y;
    uint32_t eyepos_z;

    int32_t num_skins;

    int32_t skin_width;
    int32_t skin_height;

    int32_t num_verts; // per frame
    int32_t num_tris;
    int32_t num_frames;

    int32_t  synctype;
    int32_t  flags;
    uint32_t size;
};

struct RawMDLTextureCoordinate
{
    int32_t onseam;
    int32_t s;
    int32_t t;
};

struct RawMDLTriangle
{
    int32_t facesfront;
    int32_t vertex[3];
};

struct RawMDLVertex
{
    uint8_t x, y, z;
    uint8_t light_normal;
};

struct RawMDLSimpleFrame
{
    RawMDLVertex  bboxmin;
    RawMDLVertex  bboxmax;
    char          name[16];
    RawMDLVertex *verts;
};

struct RawMDLFrame
{
    int32_t           type;
    RawMDLSimpleFrame frame;
};

/*============== EDGE REPRESENTATION ====================*/

struct MDLVertex
{
    float x, y, z;

    short normal_idx;
};

struct MDLFrame
{
    MDLVertex *vertices;

    const char *name;

    // list of normals which are used.  Terminated by -1.
    short *used_normals;
};

struct MDLPoint
{
    float skin_s, skin_t;

    // index into frame's vertex array (mdl_frame_c::verts)
    int vert_idx;
};

class MDLModel
{
  public:
    int total_frames_;
    int total_points_;
    int total_triangles_;
    int skin_width_;
    int skin_height_;

    MDLFrame *frames_;
    MDLPoint *points_;
    int      *triangle_indices_;

    int vertices_per_frame_;

    ModelMesh mesh_;

    std::vector<uint32_t> skin_id_list_;

  public:
    MDLModel(int nframes, int npoints, int ntris, int swidth, int sheight)
        : total_frames_(nframes), total_points_(npoints), total_triangles_(ntris), skin_width_(swidth),
          skin_height_(sheight), vertices_per_frame_(0)
    {
        frames_           = new MDLFrame[total_frames_];
        points_           = new MDLPoint[total_points_];
        triangle_indices_ = new int[total_triangles_];
    }

    ~MDLModel()
    {
        delete[] frames_;
        delete[] points_;
        delete[] triangle_indices_;
    }
};

static HMM_Vec3  render_position;
static RGBAColor render_rgba;
static HMM_Vec2  render_texture_coordinates;

static std::vector<RendererVertex> model_vertices;

static void ReserveModelVertices(int count)
{
    if ((int)model_vertices.size() < count)
        model_vertices.resize(count);
}

static inline void StoreModelVertex(int index, RGBAColor rgba)
{
    RendererVertex *dest = &model_vertices[index];

    dest->rgba                   = rgba;
    dest->position               = render_position;
    dest->texture_coordinates[0] = render_texture_coordinates;
}

static void MDLBuildMesh(MDLModel *md)
{
    std::vector<ModelMeshVertex> points;
    std::vector<int>             triangle_points;

    points.resize((size_t)md->total_points_);
    triangle_points.resize((size_t)md->total_points_);

    for (int p = 0; p < md->total_points_; p++)
    {
        points[(size_t)p].vert_idx = md->points_[p].vert_idx;
        points[(size_t)p].skin_s   = md->points_[p].skin_s;
        points[(size_t)p].skin_t   = md->points_[p].skin_t;

        triangle_points[(size_t)p] = p;
    }

    ModelMeshBuild(md->mesh_, points.data(), md->total_points_, triangle_points.data(), md->total_triangles_,
                   md->vertices_per_frame_);

    LogDebug("  mesh: %d vertices, %d indices, %d submeshes\n", md->mesh_.TotalVertices(), md->mesh_.TotalIndices(),
             (int)md->mesh_.submeshes_.size());
}

static void MDLUploadMesh(MDLModel *md)
{
    ModelMesh &mesh = md->mesh_;

    int vertex_count = mesh.TotalVertices();

    if (vertex_count <= 0 || md->total_frames_ <= 0)
        return;

    std::vector<float> frame_positions;
    std::vector<float> texture_coordinates;

    frame_positions.resize((size_t)vertex_count * (size_t)md->total_frames_ * 3);
    texture_coordinates.resize((size_t)vertex_count * 2);

    for (int f = 0; f < md->total_frames_; f++)
    {
        const MDLVertex *frame_vertices = md->frames_[f].vertices;

        float *destination = frame_positions.data() + (size_t)f * (size_t)vertex_count * 3;

        for (int v = 0; v < vertex_count; v++)
        {
            const MDLVertex *vert = &frame_vertices[mesh.vertices_[(size_t)v].vert_idx];

            destination[v * 3 + 0] = vert->x;
            destination[v * 3 + 1] = vert->y;
            destination[v * 3 + 2] = vert->z;
        }
    }

    for (int v = 0; v < vertex_count; v++)
    {
        texture_coordinates[(size_t)v * 2 + 0] = mesh.vertices_[(size_t)v].skin_s;
        texture_coordinates[(size_t)v * 2 + 1] = mesh.vertices_[(size_t)v].skin_t;
    }

    ModelMeshData data;

    data.frame_positions     = frame_positions.data();
    data.texture_coordinates = texture_coordinates.data();
    data.frame_count         = md->total_frames_;
    data.vertex_count        = vertex_count;

    mesh.gpu_handle_ = render_state->CreateModelMesh(data, mesh.indices_.data(), mesh.TotalIndices());

    if (mesh.gpu_handle_ != 0)
        mesh.colors_.resize((size_t)vertex_count * 6);

    LogDebug("  mesh gpu handle: %u\n", mesh.gpu_handle_);
}

/*============== LOADING CODE ====================*/

static const char *CopyFrameName(RawMDLSimpleFrame *frm)
{
    char *str = new char[20];

    memcpy(str, frm->name, 16);

    // ensure it is NUL terminated
    str[16] = 0;

    return str;
}

static short *CreateNormalList(uint8_t *which_normals)
{
    int count = 0;
    int i;

    for (i = 0; i < kTotalMDFormatNormals; i++)
        if (which_normals[i])
            count++;

    short *n_list = new short[count + 1];

    count = 0;

    for (i = 0; i < kTotalMDFormatNormals; i++)
        if (which_normals[i])
            n_list[count++] = i;

    n_list[count] = -1;

    return n_list;
}

MDLModel *MDLLoad(epi::File *f, float &radius)
{
    radius = 1;
    RawMDLHeader header;

    /* read header */
    f->Read(&header, sizeof(RawMDLHeader));

    int version = AlignedLittleEndianS32(header.version);

    LogDebug("MODEL IDENT: [%c%c%c%c] VERSION: %d", header.ident[0], header.ident[1], header.ident[2], header.ident[3],
             version);

    if (epi::StringPrefixCompare(header.ident, kMDLIdentifier) != 0)
    {
        FatalError("MDL_LoadModel: lump is not an MDL model!");
    }

    if (version != kMDLVersion)
    {
        FatalError("MDL_LoadModel: strange version!");
    }

    int num_frames = AlignedLittleEndianS32(header.num_frames);
    int num_tris   = AlignedLittleEndianS32(header.num_tris);
    int num_verts  = AlignedLittleEndianS32(header.num_verts);
    int swidth     = AlignedLittleEndianS32(header.skin_width);
    int sheight    = AlignedLittleEndianS32(header.skin_height);
    int num_points = num_tris * 3;

    MDLModel *md = new MDLModel(num_frames, num_points, num_tris, swidth, sheight);

    /* PARSE SKINS */

    for (int i = 0; i < AlignedLittleEndianS32(header.num_skins); i++)
    {
        int      group  = 0;
        uint8_t *pixels = new uint8_t[sheight * swidth];

        // Check for single vs. group skins; error if group skin found
        f->Read(&group, sizeof(int));
        if (AlignedLittleEndianS32(group))
        {
            FatalError("MDL_LoadModel: Group skins unsupported!\n");
        }

        f->Read(pixels, sheight * swidth * sizeof(uint8_t));
        ImageData *tmp_img = new ImageData(swidth, sheight, 3);
        // Expand 8 bits paletted image to RGB
        for (int j = 0; j < swidth * sheight; ++j)
        {
            for (int k = 0; k < 3; ++k)
            {
                tmp_img->pixels_[(j * 3) + k] = md_colormap[pixels[j]][k];
            }
        }
        delete[] pixels;
        md->skin_id_list_.push_back(UploadTexture(tmp_img, kUploadMipMap | kUploadSmooth));
        delete tmp_img;
    }

    /* PARSE TEXCOORDS */
    RawMDLTextureCoordinate *texcoords = new RawMDLTextureCoordinate[num_verts];
    f->Read(texcoords, num_verts * sizeof(RawMDLTextureCoordinate));

    /* PARSE TRIANGLES */

    RawMDLTriangle *tris = new RawMDLTriangle[num_tris];
    f->Read(tris, num_tris * sizeof(RawMDLTriangle));

    /* PARSE FRAMES */

    RawMDLFrame *frames = new RawMDLFrame[num_frames];

    for (int fr = 0; fr < num_frames; fr++)
    {
        frames[fr].frame.verts = new RawMDLVertex[num_verts];
        f->Read(&frames[fr].type, sizeof(int32_t));
        f->Read(&frames[fr].frame.bboxmin, sizeof(RawMDLVertex));
        f->Read(&frames[fr].frame.bboxmax, sizeof(RawMDLVertex));
        f->Read(frames[fr].frame.name, 16 * sizeof(char));
        f->Read(frames[fr].frame.verts, num_verts * sizeof(RawMDLVertex));
    }

    LogDebug("  frames:%d  points:%d  tris: %d\n", num_frames, num_points, num_tris);

    md->vertices_per_frame_ = num_verts;

    LogDebug("  vertices_per_frame_:%d\n", md->vertices_per_frame_);

    // convert glcmds into tris and points
    int      *tri   = md->triangle_indices_;
    MDLPoint *point = md->points_;

    for (int i = 0; i < num_tris; i++)
    {
        EPI_ASSERT(tri < md->triangle_indices_ + md->total_triangles_);
        EPI_ASSERT(point < md->points_ + md->total_points_);

        *tri = point - md->points_;

        tri++;

        for (int j = 0; j < 3; j++, point++)
        {
            RawMDLTriangle raw_tri = tris[i];
            point->vert_idx        = AlignedLittleEndianS32(raw_tri.vertex[j]);
            float s                = (float)AlignedLittleEndianS16(texcoords[point->vert_idx].s);
            float t                = (float)AlignedLittleEndianS16(texcoords[point->vert_idx].t);
            if (!AlignedLittleEndianS32(raw_tri.facesfront) &&
                AlignedLittleEndianS32(texcoords[point->vert_idx].onseam))
                s += (float)swidth * 0.5f;
            point->skin_s = (s + 0.5f) / (float)swidth;
            point->skin_t = (t + 0.5f) / (float)sheight;
            EPI_ASSERT(point->vert_idx >= 0);
            EPI_ASSERT(point->vert_idx < md->vertices_per_frame_);
        }
    }

    EPI_ASSERT(tri == md->triangle_indices_ + md->total_triangles_);
    EPI_ASSERT(point == md->points_ + md->total_points_);

    MDLBuildMesh(md);

    /* PARSE FRAMES */

    uint8_t which_normals[kTotalMDFormatNormals];

    uint32_t raw_scale[3];
    uint32_t raw_translate[3];

    raw_scale[0]     = AlignedLittleEndianU32(header.scale_x);
    raw_scale[1]     = AlignedLittleEndianU32(header.scale_y);
    raw_scale[2]     = AlignedLittleEndianU32(header.scale_z);
    raw_translate[0] = AlignedLittleEndianU32(header.trans_x);
    raw_translate[1] = AlignedLittleEndianU32(header.trans_y);
    raw_translate[2] = AlignedLittleEndianU32(header.trans_z);

    float *f_ptr = (float *)raw_scale;
    float  scale[3];
    float  translate[3];

    scale[0] = f_ptr[0];
    scale[1] = f_ptr[1];
    scale[2] = f_ptr[2];

    f_ptr        = (float *)raw_translate;
    translate[0] = f_ptr[0];
    translate[1] = f_ptr[1];
    translate[2] = f_ptr[2];

    for (int i = 0; i < num_frames; i++)
    {
        RawMDLFrame raw_frame = frames[i];

        md->frames_[i].name = CopyFrameName(&raw_frame.frame);

        RawMDLVertex *raw_verts = frames[i].frame.verts;

        md->frames_[i].vertices = new MDLVertex[md->vertices_per_frame_];

        EPI_CLEAR_MEMORY(which_normals, uint8_t, kTotalMDFormatNormals);

        for (int v = 0; v < md->vertices_per_frame_; v++)
        {
            RawMDLVertex *raw_V  = raw_verts + v;
            MDLVertex    *good_V = md->frames_[i].vertices + v;

            good_V->x = (int)raw_V->x * scale[0] + translate[0];
            good_V->y = (int)raw_V->y * scale[1] + translate[1];
            good_V->z = (int)raw_V->z * scale[2] + translate[2];

            good_V->normal_idx = raw_V->light_normal;

            EPI_ASSERT(good_V->normal_idx >= 0);
            // EPI_ASSERT(good_V->normal_idx < kTotalMDFormatNormals);
            //  Dasho: Maybe try to salvage bad MDL models?
            if (good_V->normal_idx >= kTotalMDFormatNormals)
            {
                LogDebug("Vert %d of Frame %d has an invalid normal index: %d\n", v, i, good_V->normal_idx);
                good_V->normal_idx = (good_V->normal_idx % kTotalMDFormatNormals);
            }

            which_normals[good_V->normal_idx] = 1;

            HMM_Vec3 vr = {{good_V->x, good_V->y, good_V->z}};
            float    r  = HMM_Len(vr);

            if (r > radius)
            {
                radius = r;
            }
        }

        md->frames_[i].used_normals = CreateNormalList(which_normals);
    }

    delete[] texcoords;
    delete[] tris;
    delete[] frames;

    MDLUploadMesh(md);

    return md;
}

short MDLFindFrame(MDLModel *md, const char *name)
{
    EPI_ASSERT(strlen(name) > 0);

    for (int f = 0; f < md->total_frames_; f++)
    {
        MDLFrame *frame = &md->frames_[f];

        if (DDFCompareName(name, frame->name) == 0)
            return f;
    }

    return -1; // NOT FOUND
}

/*============== MODEL RENDERING ====================*/

class MDLCoordinateData
{
  public:
    MapObject *map_object_;

    MDLModel *model_;

    const MDLFrame *frame1_;
    const MDLFrame *frame2_;

    float lerp_;
    float x_, y_, z_;

    bool is_weapon;
    bool is_fuzzy_;

    // scaling
    float xy_scale_;
    float z_scale_;
    float bias_;

    // fuzzy info
    float    fuzz_multiplier_;
    HMM_Vec2 fuzz_add_;

    // mlook vectors
    HMM_Vec2 mouselook_x_vector_;
    HMM_Vec2 mouselook_z_vector_;

    // rotation vectors
    HMM_Vec2 rotation_vector_x_;
    HMM_Vec2 rotation_vector_y_;

    ColorMixer normal_colors_[kTotalMDFormatNormals];

    float rotated_x_[kTotalMDFormatNormals];
    float rotated_y_[kTotalMDFormatNormals];
    float rotated_z_[kTotalMDFormatNormals];

    int rotated_count_;

    short *used_normals_;

    bool is_additive_;

  public:
    void CalculatePosition(HMM_Vec3 &pos, float x1, float y1, float z1) const
    {
        x1 *= xy_scale_;
        y1 *= xy_scale_;
        z1 *= z_scale_;

        float x2 = x1 * mouselook_x_vector_.X + z1 * mouselook_x_vector_.Y;
        float z2 = x1 * mouselook_z_vector_.X + z1 * mouselook_z_vector_.Y;
        float y2 = y1;

        pos.X = x_ + x2 * rotation_vector_x_.X + y2 * rotation_vector_x_.Y;
        pos.Y = y_ + x2 * rotation_vector_y_.X + y2 * rotation_vector_y_.Y;
        pos.Z = z_ + z2;
    }
};

static void InitializeNormalColors(MDLCoordinateData *data)
{
    short *n_list = data->used_normals_;

    for (; *n_list >= 0; n_list++)
    {
        data->normal_colors_[*n_list].Clear();
    }
}

static void RotateUsedNormals(MDLCoordinateData *data)
{
    int count = 0;

    for (short *n_list = data->used_normals_; *n_list >= 0; n_list++)
        count++;

    data->rotated_count_ = count;

    epi::SimdF32x4 mouselook_x_x = epi::SplatF32x4(data->mouselook_x_vector_.X);
    epi::SimdF32x4 mouselook_x_y = epi::SplatF32x4(data->mouselook_x_vector_.Y);
    epi::SimdF32x4 mouselook_z_x = epi::SplatF32x4(data->mouselook_z_vector_.X);
    epi::SimdF32x4 mouselook_z_y = epi::SplatF32x4(data->mouselook_z_vector_.Y);
    epi::SimdF32x4 rotation_x_x  = epi::SplatF32x4(data->rotation_vector_x_.X);
    epi::SimdF32x4 rotation_x_y  = epi::SplatF32x4(data->rotation_vector_x_.Y);
    epi::SimdF32x4 rotation_y_x  = epi::SplatF32x4(data->rotation_vector_y_.X);
    epi::SimdF32x4 rotation_y_y  = epi::SplatF32x4(data->rotation_vector_y_.Y);

    const short *list = data->used_normals_;

    int i = 0;

    for (; i + 4 <= count; i += 4)
    {
        int n0 = list[i];
        int n1 = list[i + 1];
        int n2 = list[i + 2];
        int n3 = list[i + 3];

        epi::SimdF32x4 nx1 =
            epi::SetF32x4(md_normals[n0].X, md_normals[n1].X, md_normals[n2].X, md_normals[n3].X);
        epi::SimdF32x4 ny1 =
            epi::SetF32x4(md_normals[n0].Y, md_normals[n1].Y, md_normals[n2].Y, md_normals[n3].Y);
        epi::SimdF32x4 nz1 =
            epi::SetF32x4(md_normals[n0].Z, md_normals[n1].Z, md_normals[n2].Z, md_normals[n3].Z);

        epi::SimdF32x4 nx2 = epi::AddF32x4(epi::MulF32x4(nx1, mouselook_x_x), epi::MulF32x4(nz1, mouselook_x_y));
        epi::SimdF32x4 nz2 = epi::AddF32x4(epi::MulF32x4(nx1, mouselook_z_x), epi::MulF32x4(nz1, mouselook_z_y));

        epi::StoreF32x4(data->rotated_x_ + i,
                        epi::AddF32x4(epi::MulF32x4(nx2, rotation_x_x), epi::MulF32x4(ny1, rotation_x_y)));
        epi::StoreF32x4(data->rotated_y_ + i,
                        epi::AddF32x4(epi::MulF32x4(nx2, rotation_y_x), epi::MulF32x4(ny1, rotation_y_y)));
        epi::StoreF32x4(data->rotated_z_ + i, nz2);
    }

    for (; i < count; i++)
    {
        int n = list[i];

        float nx1 = md_normals[n].X;
        float ny1 = md_normals[n].Y;
        float nz1 = md_normals[n].Z;

        float nx2 = nx1 * data->mouselook_x_vector_.X + nz1 * data->mouselook_x_vector_.Y;
        float nz2 = nx1 * data->mouselook_z_vector_.X + nz1 * data->mouselook_z_vector_.Y;

        data->rotated_x_[i] = nx2 * data->rotation_vector_x_.X + ny1 * data->rotation_vector_x_.Y;
        data->rotated_y_[i] = nx2 * data->rotation_vector_y_.X + ny1 * data->rotation_vector_y_.Y;
        data->rotated_z_[i] = nz2;
    }
}

static void ShadeNormals(AbstractShader *shader, MDLCoordinateData *data, bool skip_calc)
{
    short *n_list = data->used_normals_;

    for (int i = 0; *n_list >= 0; n_list++, i++)
    {
        short n = *n_list;

        if (skip_calc)
        {
            shader->Corner(data->normal_colors_ + n, 0.0f, 0.0f, 0.0f, data->map_object_, data->is_weapon);
            continue;
        }

        shader->Corner(data->normal_colors_ + n, data->rotated_x_[i], data->rotated_y_[i], data->rotated_z_[i],
                       data->map_object_, data->is_weapon);
    }
}

static void MDLDynamicLightCallback(MapObject *mo, void *dataptr)
{
    MDLCoordinateData *data = (MDLCoordinateData *)dataptr;

    // dynamic lights do not light themselves up!
    if (mo == data->map_object_)
        return;

    EPI_ASSERT(mo->dynamic_light_.shader);

    ShadeNormals(mo->dynamic_light_.shader, data, false);
}

static int MDLMulticolorMaximumRGB(MDLCoordinateData *data, bool additive)
{
    int result = 0;

    short *n_list = data->used_normals_;

    for (; *n_list >= 0; n_list++)
    {
        ColorMixer *col = &data->normal_colors_[*n_list];

        int mx = additive ? col->add_MAX() : col->mod_MAX();

        result = HMM_MAX(result, mx);
    }

    return result;
}

static inline void ModelCoordFunc(MDLCoordinateData *data, const ModelMeshVertex *point)
{
    const MDLFrame *frame1 = data->frame1_;
    const MDLFrame *frame2 = data->frame2_;

    const MDLVertex *vert1 = &frame1->vertices[point->vert_idx];
    const MDLVertex *vert2 = &frame2->vertices[point->vert_idx];

    float x1 = HMM_Lerp(vert1->x, data->lerp_, vert2->x);
    float y1 = HMM_Lerp(vert1->y, data->lerp_, vert2->y);
    float z1 = HMM_Lerp(vert1->z, data->lerp_, vert2->z) + data->bias_;

    data->CalculatePosition(render_position, x1, y1, z1);

    if (data->is_fuzzy_)
    {
        render_texture_coordinates.X = point->skin_s * data->fuzz_multiplier_ + data->fuzz_add_.X;
        render_texture_coordinates.Y = point->skin_t * data->fuzz_multiplier_ + data->fuzz_add_.Y;

        render_rgba = kRGBABlack;
        return;
    }

    render_texture_coordinates = {{point->skin_s, point->skin_t}};

    ColorMixer *col = &data->normal_colors_[(data->lerp_ < 0.5) ? vert1->normal_idx : vert2->normal_idx];

    if (!data->is_additive_)
    {
        render_rgba = epi::MakeRGBAClamped(col->modulate_red_ * render_view_red_multiplier,
                                           col->modulate_green_ * render_view_green_multiplier,
                                           col->modulate_blue_ * render_view_blue_multiplier);
    }
    else
    {
        render_rgba = epi::MakeRGBAClamped(col->add_red_ * render_view_red_multiplier,
                                           col->add_green_ * render_view_green_multiplier,
                                           col->add_blue_ * render_view_blue_multiplier);
    }
}

void MDLRenderModel(MDLModel *md, bool is_weapon, int frame1, int frame2, float lerp, float x, float y, float z,
                    MapObject *mo, RegionProperties *props, float scale, float aspect, float bias, int rotation)
{
    if (md->mesh_.gpu_handle_ == 0)
        return;

    // check if frames are valid
    if (frame1 < 0 || frame1 >= md->total_frames_)
    {
        LogDebug("Render model: bad frame %d\n", frame1);
        return;
    }
    if (frame2 < 0 || frame2 >= md->total_frames_)
    {
        LogDebug("Render model: bad frame %d\n", frame1);
        return;
    }

    MDLCoordinateData data;

    data.is_fuzzy_ = (mo->flags_ & kMapObjectFlagFuzzy) ? true : false;

    float trans = mo->visibility_;

    if (is_weapon && data.is_fuzzy_ && mo->player_ && mo->player_->powers_[kPowerTypePartInvisTranslucent] > 0)
    {
        data.is_fuzzy_ = false;
        trans *= 0.3f;
    }

    if (trans <= 0)
        return;

    BlendingMode blending = kBlendingNone;

    if (mo->hyper_flags_ & kHyperFlagNoZBufferUpdate)
        blending = (BlendingMode)(blending | kBlendingNoZBuffer);

    if (mirror_view.reflective)
    {
        if (fliplevels.d_)
            blending = (BlendingMode)(blending | kBlendingCullBack);
        else
            blending = (BlendingMode)(blending | kBlendingCullFront);
    }
    else
    {
        if (fliplevels.d_)
            blending = (BlendingMode)(blending | kBlendingCullFront);
        else
            blending = (BlendingMode)(blending | kBlendingCullBack);
    }

    data.map_object_ = mo;
    data.model_      = md;

    data.frame1_ = &md->frames_[frame1];
    data.frame2_ = &md->frames_[frame2];

    data.lerp_ = lerp;

    data.x_ = x;
    data.y_ = y;
    data.z_ = z;

    data.is_weapon = is_weapon;

    data.xy_scale_ = scale * aspect;
    data.z_scale_  = scale;
    data.bias_     = bias;

    bool tilt = is_weapon || (mo->flags_ & kMapObjectFlagMissile) || (mo->hyper_flags_ & kHyperFlagForceModelTilt);

    if (!console_active && !paused && !menu_active && !rts_menu_active &&
        (is_weapon || (!time_stop_active && !erraticism_active)))
    {
        BAMAngle ang;
        if (is_weapon)
        {
            BAMAngleToMatrix(tilt ? ~epi::BAMInterpolate(mo->old_vertical_angle_, mo->vertical_angle_, fractional_tic)
                                  : 0,
                             &data.mouselook_x_vector_, &data.mouselook_z_vector_);
            ang = epi::BAMInterpolate(mo->old_angle_, mo->angle_, fractional_tic) + rotation;
        }
        else
        {
            BAMAngleToMatrix(tilt ? ~mo->vertical_angle_ : 0, &data.mouselook_x_vector_, &data.mouselook_z_vector_);
            ang = mo->angle_ + rotation;
        }
        BAMAngleToMatrix(~ang, &data.rotation_vector_x_, &data.rotation_vector_y_);
    }
    else
    {
        BAMAngleToMatrix(tilt ? ~mo->vertical_angle_ : 0, &data.mouselook_x_vector_, &data.mouselook_z_vector_);
        BAMAngle ang = mo->angle_ + rotation;
        BAMAngleToMatrix(~ang, &data.rotation_vector_x_, &data.rotation_vector_y_);
    }

    data.used_normals_ = (lerp < 0.5) ? data.frame1_->used_normals : data.frame2_->used_normals;

    InitializeNormalColors(&data);

    GLuint skin_tex = 0;

    if (data.is_fuzzy_)
    {
        skin_tex = ImageCache(fuzz_image, false);

        data.fuzz_multiplier_ = 0.8;
        data.fuzz_add_        = {{0, 0}};

        if (!data.is_weapon && !view_is_zoomed)
        {
            float dist = ApproximateDistance(mo->x - view_x, mo->y - view_y, mo->z - view_z);

            data.fuzz_multiplier_ = 70.0 / HMM_Clamp(35, dist, 700);
        }

        FuzzAdjust(&data.fuzz_add_, mo);

        trans = 1.0f;

        blending = (BlendingMode)(blending | (kBlendingAlpha | kBlendingMasked));
        blending = (BlendingMode)(blending & ~kBlendingLess);
    }
    else /* (! data.is_fuzzy) */
    {
        int mdlSkin = 0;

        if (is_weapon == true)
            mdlSkin = mo->player_->weapons_[mo->player_->ready_weapon_].model_skin;
        else
            mdlSkin = mo->model_skin_;

        mdlSkin--; // ddf MODEL_SKIN starts at 1 not 0

        if (mdlSkin > -1)
            skin_tex = md->skin_id_list_[mdlSkin];
        else
            skin_tex = md->skin_id_list_[0]; // Just use skin 0?

        if (skin_tex == 0)
            FatalError("MDL Frame %s missing skins?\n", md->frames_[frame1].name);

        AbstractShader *shader =
            GetColormapShader(props, mo->info_->force_fullbright_ ? 255 : mo->state_->bright, mo->subsector_->sector);

        ShadeNormals(shader, &data, true);

        if (use_dynamic_lights && render_view_extra_light < 250)
        {
            RotateUsedNormals(&data);

            float r = mo->radius_;

            DynamicLightIterator(mo->x - r, mo->y - r, mo->z, mo->x + r, mo->y + r, mo->z + mo->height_,
                                 MDLDynamicLightCallback, &data);

            SectorGlowIterator(mo->subsector_->sector, mo->x - r, mo->y - r, mo->z, mo->x + r, mo->y + r,
                               mo->z + mo->height_, MDLDynamicLightCallback, &data);
        }
    }

    /* draw the model */

    ModelMesh &mesh = md->mesh_;

    int total_vertices = mesh.TotalVertices();

    float normal_color_table[kTotalMDFormatNormals][6];

    if (data.is_fuzzy_)
    {
        EPI_CLEAR_MEMORY(&normal_color_table[0][0], float, kTotalMDFormatNormals * 6);
    }
    else
    {
        for (short *n_list = data.used_normals_; *n_list >= 0; n_list++)
        {
            const ColorMixer *col = &data.normal_colors_[*n_list];

            float *entry = normal_color_table[*n_list];

            entry[0] = col->modulate_red_ * render_view_red_multiplier / 255.0f;
            entry[1] = col->modulate_green_ * render_view_green_multiplier / 255.0f;
            entry[2] = col->modulate_blue_ * render_view_blue_multiplier / 255.0f;

            entry[3] = col->add_red_ * render_view_red_multiplier / 255.0f;
            entry[4] = col->add_green_ * render_view_green_multiplier / 255.0f;
            entry[5] = col->add_blue_ * render_view_blue_multiplier / 255.0f;
        }
    }

    bool use_frame2 = (lerp >= 0.5f);

    const MDLVertex *normal_frame = use_frame2 ? data.frame2_->vertices : data.frame1_->vertices;

    for (int v = 0; v < total_vertices; v++)
    {
        int normal_idx = normal_frame[mesh.vertices_[(size_t)v].vert_idx].normal_idx;

        memcpy(&mesh.colors_[(size_t)v * 6], normal_color_table[normal_idx], 6 * sizeof(float));
    }

    render_state->UpdateModelColors(mesh.gpu_handle_, mesh.colors_.data(), total_vertices);

    int num_pass = (!data.is_fuzzy_ && MDLMulticolorMaximumRGB(&data, true) > 0) ? 2 : 1;

    RGBAColor fc_to_use = mo->subsector_->sector->properties.fog_color;
    float     fd_to_use = mo->subsector_->sector->properties.fog_density;
    // check for DDFLEVL fog
    if (fc_to_use == kRGBANoValue)
    {
        if (EDGE_IMAGE_IS_SKY(mo->subsector_->sector->ceiling))
        {
            fc_to_use = current_map->outdoor_fog_color_;
            fd_to_use = 0.01f * current_map->outdoor_fog_density_;
        }
        else
        {
            fc_to_use = current_map->indoor_fog_color_;
            fd_to_use = 0.01f * current_map->indoor_fog_density_;
        }
    }
    if (!draw_culling.d_ && fc_to_use != kRGBANoValue && !epi::AlmostEquals(fd_to_use, 0.0f))
    {
        render_state->ClearColor(fc_to_use);
        render_state->FogMode(GL_EXP);
        render_state->FogColor(fc_to_use);
        render_state->FogDensity(std::log1p(fd_to_use));
        render_state->Enable(GL_FOG);
    }
    else if (draw_culling.d_)
    {
        RGBAColor fogColor;
        if (need_to_draw_sky)
        {
            switch (cull_fog_color.d_)
            {
            case 0:
                fogColor = culling_fog_color;
                break;
            case 1:
                // Not pure white, but 1.0f felt like a little much - Dasho
                fogColor = kRGBASilver;
                break;
            case 2:
                fogColor = MakeRGBAConstant(0x404040FF); // Find a constant to call this
                break;
            case 3:
                fogColor = kRGBABlack;
                break;
            default:
                fogColor = culling_fog_color;
                break;
            }
        }
        else
        {
            fogColor = kRGBABlack;
        }
        render_state->ClearColor(fogColor);
        render_state->FogMode(GL_LINEAR);
        render_state->FogColor(fogColor);
        render_state->FogStart(renderer_far_clip.f_ - 750.0f);
        render_state->FogEnd(renderer_far_clip.f_ - 250.0f);
        render_state->Enable(GL_FOG);
    }
    else
        render_state->Disable(GL_FOG);

    for (int pass = 0; pass < num_pass; pass++)
    {
        if (pass == 1)
        {
            blending = (BlendingMode)(blending & ~kBlendingAlpha);
            blending = (BlendingMode)(blending | kBlendingAdd);
            render_state->Disable(GL_FOG);
        }

        data.is_additive_ = (pass > 0);

        render_state->PolygonOffset(0, -pass);

        if (blending & kBlendingLess)
        {
            render_state->Enable(GL_ALPHA_TEST);
        }
        else if (blending & kBlendingMasked)
        {
            render_state->Enable(GL_ALPHA_TEST);
            render_state->AlphaFunction(GL_GREATER, 0);
        }
        else
            render_state->Disable(GL_ALPHA_TEST);

        if (blending & kBlendingAdd)
        {
            render_state->Enable(GL_BLEND);
            render_state->BlendFunction(GL_SRC_ALPHA, GL_ONE);
        }
        else if (blending & kBlendingAlpha)
        {
            render_state->Enable(GL_BLEND);
            render_state->BlendFunction(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
        else
            render_state->Disable(GL_BLEND);

        if (blending & (kBlendingCullBack | kBlendingCullFront))
        {
            render_state->Enable(GL_CULL_FACE);
            render_state->CullFace((blending & kBlendingCullFront) ? GL_FRONT : GL_BACK);
        }
        else
            render_state->Disable(GL_CULL_FACE);

        render_state->DepthMask((blending & kBlendingNoZBuffer) ? false : true);

        if (blending & kBlendingLess)
        {
            // NOTE: assumes alpha is constant over whole model
            render_state->AlphaFunction(GL_GREATER, trans * 0.66f);
        }

        render_state->ActiveTexture(GL_TEXTURE1);
        render_state->Disable(GL_TEXTURE_2D);
        render_state->ActiveTexture(GL_TEXTURE0);
        render_state->Enable(GL_TEXTURE_2D);
        render_state->BindTexture(skin_tex);

        GLint old_clamp = kDummyClamp;

        if (blending & kBlendingClampY)
        {
            auto existing = texture_clamp_t.find(skin_tex);
            if (existing != texture_clamp_t.end())
            {
                old_clamp = existing->second;
            }

            render_state->TextureWrapT(GL_CLAMP_TO_EDGE);
        }

        render_state->SetPipeline(0);

        ModelDrawInfo info;

        info.handle = mesh.gpu_handle_;

        info.frame1 = frame1;
        info.frame2 = frame2;
        info.lerp   = lerp;

        info.transform =
            ModelBuildTransform(data.xy_scale_, data.z_scale_, data.bias_, data.mouselook_x_vector_,
                                data.mouselook_z_vector_, data.rotation_vector_x_, data.rotation_vector_y_, data.x_,
                                data.y_, data.z_);

        info.alpha         = trans;
        info.additive_pass = data.is_additive_;

        if (blending & kBlendingLess)
            info.alpha_test = trans * 0.66f;
        else if (blending & kBlendingMasked)
            info.alpha_test = 1.0f / 255.0f;
        else
            info.alpha_test = 0.0f;

        if (data.is_fuzzy_)
        {
            info.texture_scale  = {{data.fuzz_multiplier_, data.fuzz_multiplier_}};
            info.texture_offset = data.fuzz_add_;
        }

        for (size_t s = 0; s < mesh.submeshes_.size(); s++)
        {
            const ModelMeshSubmesh &submesh = mesh.submeshes_[s];

            info.first_vertex = submesh.first_vertex;
            info.vertex_count = submesh.vertex_count;
            info.first_index  = submesh.first_index;
            info.index_count  = submesh.index_count;

            render_state->DrawModel(info);
        }

        // restore the clamping mode
        if (old_clamp != kDummyClamp)
        {
            render_state->TextureWrapT(old_clamp);
        }
    }
}

void MDLRenderModel2D(MDLModel *md, int frame, float x, float y, float xscale, float yscale,
                      const MapObjectDefinition *info)
{
    // check if frame is valid
    if (frame < 0 || frame >= md->total_frames_)
        return;

    GLuint skin_tex = md->skin_id_list_[0]; // Just use skin 0?

    if (skin_tex == 0)
        FatalError("MDL Frame %s missing skins?\n", md->frames_[frame].name);

    xscale = yscale * info->model_scale_ * info->model_aspect_;
    yscale = yscale * info->model_scale_;

    render_state->ActiveTexture(GL_TEXTURE1);
    render_state->Disable(GL_TEXTURE_2D);
    render_state->ActiveTexture(GL_TEXTURE0);
    render_state->Enable(GL_TEXTURE_2D);
    render_state->BindTexture(skin_tex);

    render_state->Enable(GL_BLEND);
    render_state->Enable(GL_CULL_FACE);

    RGBAColor model_rgba = (info->flags_ & kMapObjectFlagFuzzy) ? epi::MakeRGBA(0, 0, 0, 128) : kRGBAWhite;

    const ModelMesh &mesh = md->mesh_;

    int total_vertices = mesh.TotalVertices();

    ReserveModelVertices(total_vertices);

    const MDLFrame *frame_ptr = &md->frames_[frame];

    for (int v = 0; v < total_vertices; v++)
    {
        const ModelMeshVertex *point = &mesh.vertices_[(size_t)v];
        const MDLVertex       *vert  = &frame_ptr->vertices[point->vert_idx];

        render_texture_coordinates = {{point->skin_s, point->skin_t}};

        float dx = vert->x * xscale;
        float dy = vert->y * xscale;
        float dz = (vert->z + info->model_bias_) * yscale;

        render_position = {{x + dy, y + dz, dx / 256.0f}};

        StoreModelVertex(v, model_rgba);
    }

    render_state->SetPipeline(0);

    render_state->SetModelIndices(mesh.indices_.data(), mesh.TotalIndices());

    for (size_t s = 0; s < mesh.submeshes_.size(); s++)
    {
        const ModelMeshSubmesh &submesh = mesh.submeshes_[s];

        render_state->SetVertexArrays(model_vertices.data() + submesh.first_vertex, submesh.vertex_count);
        render_state->DrawModelIndexed(submesh.first_index, submesh.index_count);
    }

    render_state->Disable(GL_BLEND);
    render_state->Disable(GL_TEXTURE_2D);
    render_state->Disable(GL_CULL_FACE);
}
