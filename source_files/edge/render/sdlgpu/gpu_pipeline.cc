#include "gpu_pipeline.h"

#include <stddef.h>

#include <unordered_map>

#include "epi.h"
#include "gpu_shaders.h"
#include "i_system.h"
#include "r_units.h"

static_assert(sizeof(RendererVertex) == 32, "RendererVertex size");
static_assert(offsetof(RendererVertex, rgba) == 0, "RendererVertex::rgba offset");
static_assert(offsetof(RendererVertex, position) == 4, "RendererVertex::position offset");
static_assert(offsetof(RendererVertex, texture_coordinates) == 16, "RendererVertex::texture_coordinates offset");

static std::unordered_map<uint32_t, SDL_GPUGraphicsPipeline *> pipelines;

static SDL_GPUDevice       *pipeline_device        = nullptr;
static SDL_GPUTextureFormat pipeline_color_format  = SDL_GPU_TEXTUREFORMAT_INVALID;
static SDL_GPUTextureFormat pipeline_depth_format  = SDL_GPU_TEXTUREFORMAT_INVALID;

static uint32_t EncodeBlendFlags(uint32_t pipeline_flags, GLenum source_blend, GLenum destination_blend)
{
    if (!(pipeline_flags & kGpuPipelineBlend))
        return pipeline_flags;

    switch (source_blend)
    {
    case GL_SRC_ALPHA:
        pipeline_flags |= kGpuPipelineBlendSourceSourceAlpha;
        break;
    case GL_ONE_MINUS_DST_COLOR:
        pipeline_flags |= kGpuPipelineBlendSourceOneMinusDestinationColor;
        break;
    case GL_DST_COLOR:
        pipeline_flags |= kGpuPipelineBlendSourceDestinationColor;
        break;
    case GL_ZERO:
        pipeline_flags |= kGpuPipelineBlendSourceZero;
        break;
    }

    switch (destination_blend)
    {
    case GL_ONE:
        pipeline_flags |= kGpuPipelineBlendDestinationOne;
        break;
    case GL_ONE_MINUS_SRC_ALPHA:
        pipeline_flags |= kGpuPipelineBlendDestinationOneMinusSourceAlpha;
        break;
    case GL_SRC_COLOR:
        pipeline_flags |= kGpuPipelineBlendDestinationSourceColor;
        break;
    case GL_ZERO:
        pipeline_flags |= kGpuPipelineBlendDestinationZero;
        break;
    }

    return pipeline_flags;
}

static void SetupBlendState(SDL_GPUColorTargetBlendState *blend, GLenum source_blend, GLenum destination_blend)
{
    SDL_GPUBlendFactor source_color      = SDL_GPU_BLENDFACTOR_ZERO;
    SDL_GPUBlendFactor source_alpha      = SDL_GPU_BLENDFACTOR_ZERO;
    SDL_GPUBlendFactor destination_color = SDL_GPU_BLENDFACTOR_ZERO;
    SDL_GPUBlendFactor destination_alpha = SDL_GPU_BLENDFACTOR_ZERO;

    switch (source_blend)
    {
    case GL_SRC_ALPHA:
        source_color = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        source_alpha = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        break;
    case GL_ONE_MINUS_DST_COLOR:
        source_color = SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
        source_alpha = SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
        break;
    case GL_DST_COLOR:
        source_color = SDL_GPU_BLENDFACTOR_DST_COLOR;
        source_alpha = SDL_GPU_BLENDFACTOR_DST_ALPHA;
        break;
    case GL_ZERO:
        source_color = SDL_GPU_BLENDFACTOR_ZERO;
        source_alpha = SDL_GPU_BLENDFACTOR_ZERO;
        break;
    }

    switch (destination_blend)
    {
    case GL_ONE:
        destination_color = SDL_GPU_BLENDFACTOR_ONE;
        destination_alpha = SDL_GPU_BLENDFACTOR_ONE;
        break;
    case GL_ONE_MINUS_SRC_ALPHA:
        destination_color = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        destination_alpha = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case GL_SRC_COLOR:
        destination_color = SDL_GPU_BLENDFACTOR_SRC_COLOR;
        destination_alpha = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        break;
    case GL_ZERO:
        destination_color = SDL_GPU_BLENDFACTOR_ZERO;
        destination_alpha = SDL_GPU_BLENDFACTOR_ZERO;
        break;
    }

    blend->enable_blend           = true;
    blend->src_color_blendfactor  = source_color;
    blend->dst_color_blendfactor  = destination_color;
    blend->color_blend_op         = SDL_GPU_BLENDOP_ADD;
    blend->src_alpha_blendfactor  = source_alpha;
    blend->dst_alpha_blendfactor  = destination_alpha;
    blend->alpha_blend_op         = SDL_GPU_BLENDOP_ADD;
}

static SDL_GPUGraphicsPipeline *CreatePipeline(uint32_t pipeline_flags, GLenum source_blend, GLenum destination_blend,
                                               GpuPrimitiveType primitive)
{
    SDL_GPUVertexBufferDescription buffer_description;
    EPI_CLEAR_MEMORY(&buffer_description, SDL_GPUVertexBufferDescription, 1);

    buffer_description.slot       = 0;
    buffer_description.pitch      = (uint32_t)sizeof(RendererVertex);
    buffer_description.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexAttribute attributes[3];
    EPI_CLEAR_MEMORY(attributes, SDL_GPUVertexAttribute, 3);

    attributes[0].location    = kGpuAttributePosition;
    attributes[0].buffer_slot = 0;
    attributes[0].format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    attributes[0].offset      = (uint32_t)offsetof(RendererVertex, position);

    attributes[1].location    = kGpuAttributeTextureCoords;
    attributes[1].buffer_slot = 0;
    attributes[1].format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    attributes[1].offset      = (uint32_t)offsetof(RendererVertex, texture_coordinates);

    attributes[2].location    = kGpuAttributeColor;
    attributes[2].buffer_slot = 0;
    attributes[2].format      = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM;
    attributes[2].offset      = (uint32_t)offsetof(RendererVertex, rgba);

    SDL_GPUColorTargetDescription color_target;
    EPI_CLEAR_MEMORY(&color_target, SDL_GPUColorTargetDescription, 1);

    color_target.format = pipeline_color_format;

    if (pipeline_flags & kGpuPipelineBlend)
        SetupBlendState(&color_target.blend_state, source_blend, destination_blend);

    SDL_GPUGraphicsPipelineCreateInfo info;
    EPI_CLEAR_MEMORY(&info, SDL_GPUGraphicsPipelineCreateInfo, 1);

    info.vertex_shader   = WorldVertexShader();
    info.fragment_shader = WorldFragmentShader();

    info.vertex_input_state.vertex_buffer_descriptions = &buffer_description;
    info.vertex_input_state.num_vertex_buffers         = 1;
    info.vertex_input_state.vertex_attributes          = attributes;
    info.vertex_input_state.num_vertex_attributes      = 3;

    switch (primitive)
    {
    case kGpuPrimitiveTriangleStrip:
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
        break;
    case kGpuPrimitiveLineList:
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_LINELIST;
        break;
    default:
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        break;
    }

    info.rasterizer_state.fill_mode  = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;

    if (pipeline_flags & kGpuPipelineCullBack)
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
    else if (pipeline_flags & kGpuPipelineCullFront)
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_FRONT;
    else
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;

    info.rasterizer_state.enable_depth_clip = true;

    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;

    info.depth_stencil_state.enable_depth_test = true;

    if (!(pipeline_flags & kGpuPipelineDepthTest))
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
    else if (pipeline_flags & kGpuPipelineDepthGreater)
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER;
    else
        info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;

    info.depth_stencil_state.enable_depth_write = (pipeline_flags & kGpuPipelineDepthWrite) ? true : false;

    info.target_info.color_target_descriptions = &color_target;
    info.target_info.num_color_targets         = 1;
    info.target_info.depth_stencil_format      = pipeline_depth_format;
    info.target_info.has_depth_stencil_target  = true;

    SDL_GPUGraphicsPipeline *pipeline = SDL_CreateGPUGraphicsPipeline(pipeline_device, &info);

    if (!pipeline)
        FatalError("GpuPipeline: SDL_CreateGPUGraphicsPipeline failed: %s\n", SDL_GetError());

    return pipeline;
}

static SDL_GPUGraphicsPipeline *movie_pipeline = nullptr;

SDL_GPUGraphicsPipeline *GetMoviePipeline(void)
{
    if (movie_pipeline)
        return movie_pipeline;

    if (!CreateMovieShaders(pipeline_device))
        FatalError("GpuPipeline: movie shader creation failed\n");

    SDL_GPUVertexBufferDescription buffer_description;
    EPI_CLEAR_MEMORY(&buffer_description, SDL_GPUVertexBufferDescription, 1);

    buffer_description.slot       = 0;
    buffer_description.pitch      = (uint32_t)sizeof(RendererVertex);
    buffer_description.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexAttribute attributes[3];
    EPI_CLEAR_MEMORY(attributes, SDL_GPUVertexAttribute, 3);

    attributes[0].location    = kGpuAttributePosition;
    attributes[0].buffer_slot = 0;
    attributes[0].format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    attributes[0].offset      = (uint32_t)offsetof(RendererVertex, position);

    attributes[1].location    = kGpuAttributeTextureCoords;
    attributes[1].buffer_slot = 0;
    attributes[1].format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    attributes[1].offset      = (uint32_t)offsetof(RendererVertex, texture_coordinates);

    attributes[2].location    = kGpuAttributeColor;
    attributes[2].buffer_slot = 0;
    attributes[2].format      = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM;
    attributes[2].offset      = (uint32_t)offsetof(RendererVertex, rgba);

    SDL_GPUColorTargetDescription color_target;
    EPI_CLEAR_MEMORY(&color_target, SDL_GPUColorTargetDescription, 1);
    color_target.format = pipeline_color_format;

    SDL_GPUGraphicsPipelineCreateInfo info;
    EPI_CLEAR_MEMORY(&info, SDL_GPUGraphicsPipelineCreateInfo, 1);

    info.vertex_shader   = MovieVertexShader();
    info.fragment_shader = MovieFragmentShader();

    info.vertex_input_state.vertex_buffer_descriptions = &buffer_description;
    info.vertex_input_state.num_vertex_buffers         = 1;
    info.vertex_input_state.vertex_attributes          = attributes;
    info.vertex_input_state.num_vertex_attributes      = 3;

    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

    info.rasterizer_state.fill_mode         = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.front_face        = SDL_GPU_FRONTFACE_CLOCKWISE;
    info.rasterizer_state.cull_mode         = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.enable_depth_clip = true;

    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;

    info.depth_stencil_state.enable_depth_test  = true;
    info.depth_stencil_state.compare_op         = SDL_GPU_COMPAREOP_ALWAYS;
    info.depth_stencil_state.enable_depth_write = false;

    info.target_info.color_target_descriptions = &color_target;
    info.target_info.num_color_targets         = 1;
    info.target_info.depth_stencil_format      = pipeline_depth_format;
    info.target_info.has_depth_stencil_target  = true;

    movie_pipeline = SDL_CreateGPUGraphicsPipeline(pipeline_device, &info);

    if (!movie_pipeline)
        FatalError("GpuPipeline: movie pipeline creation failed: %s\n", SDL_GetError());

    return movie_pipeline;
}

bool InitPipelines(SDL_GPUDevice *device, SDL_GPUTextureFormat color_format, SDL_GPUTextureFormat depth_format)
{
    pipeline_device       = device;
    pipeline_color_format = color_format;
    pipeline_depth_format = depth_format;

    return device != nullptr;
}

void ShutdownPipelines(SDL_GPUDevice *device)
{
    for (auto itr = pipelines.begin(); itr != pipelines.end(); itr++)
    {
        if (itr->second)
            SDL_ReleaseGPUGraphicsPipeline(device, itr->second);
    }

    pipelines.clear();

    if (movie_pipeline)
    {
        SDL_ReleaseGPUGraphicsPipeline(device, movie_pipeline);
        movie_pipeline = nullptr;
    }

    DestroyMovieShaders(device);

    pipeline_device = nullptr;
}

SDL_GPUGraphicsPipeline *GetPipeline(uint32_t pipeline_flags, GLenum source_blend, GLenum destination_blend,
                                     GpuPrimitiveType primitive)
{
    pipeline_flags = EncodeBlendFlags(pipeline_flags, source_blend, destination_blend);

    uint32_t key = pipeline_flags | ((uint32_t)primitive << 16);

    auto itr = pipelines.find(key);

    if (itr != pipelines.end())
        return itr->second;

    SDL_GPUGraphicsPipeline *pipeline = CreatePipeline(pipeline_flags, source_blend, destination_blend, primitive);

    pipelines[key] = pipeline;

    return pipeline;
}
