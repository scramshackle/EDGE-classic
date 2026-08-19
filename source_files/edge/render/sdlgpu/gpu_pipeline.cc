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

enum GpuPipelineShaderKind
{
    kGpuPipelineShaderWorld = 0,
    kGpuPipelineShaderModel,
    kGpuPipelineShaderLight,
    kGpuPipelineShaderOit,
    kGpuPipelineShaderModelOit
};

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
    case GL_ONE:
        source_color = SDL_GPU_BLENDFACTOR_ONE;
        source_alpha = SDL_GPU_BLENDFACTOR_ONE;
        break;
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
                                               GpuPrimitiveType primitive, GpuPipelineShaderKind shader_kind)
{
    bool model = (shader_kind == kGpuPipelineShaderModel || shader_kind == kGpuPipelineShaderModelOit);

    SDL_GPUVertexBufferDescription buffer_description[4];
    EPI_CLEAR_MEMORY(buffer_description, SDL_GPUVertexBufferDescription, 4);

    SDL_GPUVertexAttribute attributes[4];
    EPI_CLEAR_MEMORY(attributes, SDL_GPUVertexAttribute, 4);

    uint32_t buffer_count    = 1;
    uint32_t attribute_count = 3;

    if (model)
    {
        buffer_count    = 4;
        attribute_count = 4;

        buffer_description[0].slot       = kGpuModelBufferSlotPositionFrame1;
        buffer_description[0].pitch      = (uint32_t)(3 * sizeof(float));
        buffer_description[0].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

        buffer_description[1].slot       = kGpuModelBufferSlotPositionFrame2;
        buffer_description[1].pitch      = (uint32_t)(3 * sizeof(float));
        buffer_description[1].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

        buffer_description[2].slot       = kGpuModelBufferSlotTextureCoordinates;
        buffer_description[2].pitch      = (uint32_t)(2 * sizeof(float));
        buffer_description[2].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

        buffer_description[3].slot       = kGpuModelBufferSlotColor;
        buffer_description[3].pitch      = (uint32_t)(6 * sizeof(float));
        buffer_description[3].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

        attributes[0].location    = kGpuAttributeModelPositionFrame1;
        attributes[0].buffer_slot = kGpuModelBufferSlotPositionFrame1;
        attributes[0].format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        attributes[0].offset      = 0;

        attributes[1].location    = kGpuAttributeModelPositionFrame2;
        attributes[1].buffer_slot = kGpuModelBufferSlotPositionFrame2;
        attributes[1].format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        attributes[1].offset      = 0;

        attributes[2].location    = kGpuAttributeModelTextureCoordinates;
        attributes[2].buffer_slot = kGpuModelBufferSlotTextureCoordinates;
        attributes[2].format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        attributes[2].offset      = 0;

        attributes[3].location    = kGpuAttributeModelColor;
        attributes[3].buffer_slot = kGpuModelBufferSlotColor;
        attributes[3].format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        attributes[3].offset      = 0;
    }
    else
    {
        buffer_description[0].slot       = 0;
        buffer_description[0].pitch      = (uint32_t)sizeof(RendererVertex);
        buffer_description[0].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

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
    }

    SDL_GPUColorTargetDescription color_target[2];
    EPI_CLEAR_MEMORY(color_target, SDL_GPUColorTargetDescription, 2);

    color_target[0].format = pipeline_color_format;

    uint32_t color_target_count = 1;

    if (shader_kind == kGpuPipelineShaderOit || shader_kind == kGpuPipelineShaderModelOit)
    {
        color_target[0].format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        color_target[1].format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;

        SetupBlendState(&color_target[0].blend_state, GL_ONE, GL_ONE);
        SetupBlendState(&color_target[1].blend_state, GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);

        color_target_count = 2;
    }
    else if (pipeline_flags & kGpuPipelineBlend)
        SetupBlendState(&color_target[0].blend_state, source_blend, destination_blend);

    if (pipeline_flags & kGpuPipelineNoColorWrite)
    {
        for (uint32_t i = 0; i < color_target_count; i++)
        {
            color_target[i].blend_state.enable_color_write_mask = true;
            color_target[i].blend_state.color_write_mask        = 0;
        }
    }

    SDL_GPUGraphicsPipelineCreateInfo info;
    EPI_CLEAR_MEMORY(&info, SDL_GPUGraphicsPipelineCreateInfo, 1);

    if (shader_kind == kGpuPipelineShaderOit)
    {
        info.vertex_shader   = WorldVertexShader();
        info.fragment_shader = WorldOitFragmentShader();
    }
    else if (shader_kind == kGpuPipelineShaderModelOit)
    {
        info.vertex_shader   = ModelVertexShader();
        info.fragment_shader = ModelOitFragmentShader();
    }
    else if (shader_kind == kGpuPipelineShaderModel)
    {
        info.vertex_shader   = ModelVertexShader();
        info.fragment_shader = ModelFragmentShader();
    }
    else if (shader_kind == kGpuPipelineShaderLight)
    {
        info.vertex_shader   = LightVertexShader();
        info.fragment_shader = LightFragmentShader();
    }
    else
    {
        info.vertex_shader   = WorldVertexShader();
        info.fragment_shader = WorldFragmentShader();
    }

    info.vertex_input_state.vertex_buffer_descriptions = buffer_description;
    info.vertex_input_state.num_vertex_buffers         = buffer_count;
    info.vertex_input_state.vertex_attributes          = attributes;
    info.vertex_input_state.num_vertex_attributes      = attribute_count;

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

    uint32_t stencil_flags = pipeline_flags & (kGpuPipelineStencilWrite | kGpuPipelineStencilTest |
                                              kGpuPipelineStencilIncrement | kGpuPipelineStencilDecrement);

    if (stencil_flags)
    {
        SDL_GPUStencilOpState stencil_state;
        EPI_CLEAR_MEMORY(&stencil_state, SDL_GPUStencilOpState, 1);

        stencil_state.compare_op =
            (pipeline_flags & kGpuPipelineStencilTest) ? SDL_GPU_COMPAREOP_EQUAL : SDL_GPU_COMPAREOP_ALWAYS;

        stencil_state.fail_op       = SDL_GPU_STENCILOP_KEEP;
        stencil_state.depth_fail_op = SDL_GPU_STENCILOP_KEEP;

        if (pipeline_flags & kGpuPipelineStencilWrite)
            stencil_state.pass_op = SDL_GPU_STENCILOP_REPLACE;
        else if (pipeline_flags & kGpuPipelineStencilIncrement)
            stencil_state.pass_op = SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP;
        else if (pipeline_flags & kGpuPipelineStencilDecrement)
            stencil_state.pass_op = SDL_GPU_STENCILOP_DECREMENT_AND_CLAMP;
        else
            stencil_state.pass_op = SDL_GPU_STENCILOP_KEEP;

        info.depth_stencil_state.write_mask =
            (stencil_state.pass_op == SDL_GPU_STENCILOP_KEEP) ? 0x00 : 0xFF;

        info.depth_stencil_state.enable_stencil_test = true;
        info.depth_stencil_state.compare_mask        = 0xFF;
        info.depth_stencil_state.front_stencil_state = stencil_state;
        info.depth_stencil_state.back_stencil_state  = stencil_state;
    }

    info.target_info.color_target_descriptions = color_target;
    info.target_info.num_color_targets         = color_target_count;
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

    SDL_GPUGraphicsPipeline *pipeline =
        CreatePipeline(pipeline_flags, source_blend, destination_blend, primitive, kGpuPipelineShaderWorld);

    pipelines[key] = pipeline;

    return pipeline;
}

SDL_GPUGraphicsPipeline *GetModelPipeline(uint32_t pipeline_flags, GLenum source_blend, GLenum destination_blend)
{
    if (!CreateModelShaders(pipeline_device))
        FatalError("GpuPipeline: model shader creation failed\n");

    pipeline_flags = EncodeBlendFlags(pipeline_flags, source_blend, destination_blend);

    uint32_t key = pipeline_flags | (1u << 24);

    auto itr = pipelines.find(key);

    if (itr != pipelines.end())
        return itr->second;

    SDL_GPUGraphicsPipeline *pipeline = CreatePipeline(pipeline_flags, source_blend, destination_blend,
                                                       kGpuPrimitiveTriangleList, kGpuPipelineShaderModel);

    pipelines[key] = pipeline;

    return pipeline;
}

SDL_GPUGraphicsPipeline *GetOitPipeline(uint32_t pipeline_flags, GpuPrimitiveType primitive)
{
    pipeline_flags &= ~(uint32_t)(kGpuPipelineBlend | kGpuPipelineBlendSourceSourceAlpha |
                                  kGpuPipelineBlendSourceOneMinusDestinationColor |
                                  kGpuPipelineBlendSourceDestinationColor | kGpuPipelineBlendSourceZero |
                                  kGpuPipelineBlendDestinationOne | kGpuPipelineBlendDestinationOneMinusSourceAlpha |
                                  kGpuPipelineBlendDestinationSourceColor | kGpuPipelineBlendDestinationZero);

    uint32_t key = pipeline_flags | ((uint32_t)primitive << 16) | (3u << 24);

    auto itr = pipelines.find(key);

    if (itr != pipelines.end())
        return itr->second;

    SDL_GPUGraphicsPipeline *pipeline = CreatePipeline(pipeline_flags, GL_ONE, GL_ONE, primitive, kGpuPipelineShaderOit);

    pipelines[key] = pipeline;

    return pipeline;
}

SDL_GPUGraphicsPipeline *GetModelOitPipeline(uint32_t pipeline_flags)
{
    if (!CreateModelShaders(pipeline_device))
        FatalError("GpuPipeline: model shader creation failed\n");

    pipeline_flags &= ~(uint32_t)(kGpuPipelineBlend | kGpuPipelineBlendSourceSourceAlpha |
                                  kGpuPipelineBlendSourceOneMinusDestinationColor |
                                  kGpuPipelineBlendSourceDestinationColor | kGpuPipelineBlendSourceZero |
                                  kGpuPipelineBlendDestinationOne | kGpuPipelineBlendDestinationOneMinusSourceAlpha |
                                  kGpuPipelineBlendDestinationSourceColor | kGpuPipelineBlendDestinationZero);

    uint32_t key = pipeline_flags | (4u << 24);

    auto itr = pipelines.find(key);

    if (itr != pipelines.end())
        return itr->second;

    SDL_GPUGraphicsPipeline *pipeline =
        CreatePipeline(pipeline_flags, GL_ONE, GL_ONE, kGpuPrimitiveTriangleList, kGpuPipelineShaderModelOit);

    pipelines[key] = pipeline;

    return pipeline;
}

SDL_GPUGraphicsPipeline *GetLightPipeline(uint32_t pipeline_flags, GLenum source_blend, GLenum destination_blend)
{
    if (!CreateLightShaders(pipeline_device))
        FatalError("GpuPipeline: light shader creation failed\n");

    pipeline_flags = EncodeBlendFlags(pipeline_flags, source_blend, destination_blend);

    uint32_t key = pipeline_flags | (2u << 24);

    auto itr = pipelines.find(key);

    if (itr != pipelines.end())
        return itr->second;

    SDL_GPUGraphicsPipeline *pipeline = CreatePipeline(pipeline_flags, source_blend, destination_blend,
                                                       kGpuPrimitiveTriangleList, kGpuPipelineShaderLight);

    pipelines[key] = pipeline;

    return pipeline;
}
