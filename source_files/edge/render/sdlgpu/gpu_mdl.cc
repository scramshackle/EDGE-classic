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

#include <unordered_map>
#include <vector>

#include "ddf_types.h"
#include "dm_state.h" // EDGE_IMAGE_IS_SKY
#include "epi.h"
#include "epi_endian.h"
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
    const int      *triangle_indices_;

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

static void ShadeNormals(AbstractShader *shader, MDLCoordinateData *data, bool skip_calc)
{
    short *n_list = data->used_normals_;

    for (; *n_list >= 0; n_list++)
    {
        short n  = *n_list;
        float nx = 0;
        float ny = 0;
        float nz = 0;

        if (!skip_calc)
        {
            float nx1 = md_normals[n].X;
            float ny1 = md_normals[n].Y;
            float nz1 = md_normals[n].Z;

            float nx2 = nx1 * data->mouselook_x_vector_.X + nz1 * data->mouselook_x_vector_.Y;
            float nz2 = nx1 * data->mouselook_z_vector_.X + nz1 * data->mouselook_z_vector_.Y;
            float ny2 = ny1;

            nx = nx2 * data->rotation_vector_x_.X + ny2 * data->rotation_vector_x_.Y;
            ny = nx2 * data->rotation_vector_y_.X + ny2 * data->rotation_vector_y_.Y;
            nz = nz2;
        }

        shader->Corner(data->normal_colors_ + n, nx, ny, nz, data->map_object_, data->is_weapon);
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

static void UpdateMulticols(MDLCoordinateData *data)
{
    short *n_list = data->used_normals_;

    for (; *n_list >= 0; n_list++)
    {
        ColorMixer *col = &data->normal_colors_[*n_list];

        col->modulate_red_ -= 256;
        col->modulate_green_ -= 256;
        col->modulate_blue_ -= 256;
    }
}

static inline void ModelCoordFunc(MDLCoordinateData *data, int v_idx)
{
    const MDLModel *md = data->model_;

    const MDLFrame *frame1 = data->frame1_;
    const MDLFrame *frame2 = data->frame2_;
    const int      *tri    = data->triangle_indices_;

    EPI_ASSERT(*tri + v_idx >= 0);
    EPI_ASSERT(*tri + v_idx < md->total_points_);

    const MDLPoint *point = &md->points_[*tri + v_idx];

    const MDLVertex *vert1 = &frame1->vertices[point->vert_idx];
    const MDLVertex *vert2 = &frame2->vertices[point->vert_idx];

    float x1 = HMM_Lerp(vert1->x, data->lerp_, vert2->x);
    float y1 = HMM_Lerp(vert1->y, data->lerp_, vert2->y);
    float z1 = HMM_Lerp(vert1->z, data->lerp_, vert2->z) + data->bias_;

    if (render_mirror_set.Reflective())
        y1 = -y1;

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
    EPI_UNUSED(md);
    EPI_UNUSED(is_weapon);
    EPI_UNUSED(frame1);
    EPI_UNUSED(frame2);
    EPI_UNUSED(lerp);
    EPI_UNUSED(x);
    EPI_UNUSED(y);
    EPI_UNUSED(z);
    EPI_UNUSED(mo);
    EPI_UNUSED(props);
    EPI_UNUSED(scale);
    EPI_UNUSED(aspect);
    EPI_UNUSED(bias);
    EPI_UNUSED(rotation);
}

void MDLRenderModel2D(MDLModel *md, int frame, float x, float y, float xscale, float yscale,
                      const MapObjectDefinition *info)
{
    EPI_UNUSED(md);
    EPI_UNUSED(frame);
    EPI_UNUSED(x);
    EPI_UNUSED(y);
    EPI_UNUSED(xscale);
    EPI_UNUSED(yscale);
    EPI_UNUSED(info);
}
