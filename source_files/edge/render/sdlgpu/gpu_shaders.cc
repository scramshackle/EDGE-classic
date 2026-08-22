#include "gpu_shaders.h"

#include "epi.h"
#include "i_system.h"
#include "shaders/model_oit_spirv.h"
#include "shaders/model_spirv.h"
#include "shaders/movie_spirv.h"
#include "shaders/world_oit_spirv.h"
#include "shaders/world_spirv.h"

static SDL_GPUShader *world_vertex_shader   = nullptr;
static SDL_GPUShader *world_oit_fragment_shader = nullptr;
static SDL_GPUShader *world_fragment_shader = nullptr;
static SDL_GPUShader *movie_vertex_shader    = nullptr;
static SDL_GPUShader *movie_fragment_shader  = nullptr;
static SDL_GPUShader *model_vertex_shader    = nullptr;
static SDL_GPUShader *model_fragment_shader  = nullptr;
static SDL_GPUShader *model_oit_fragment_shader = nullptr;

static SDL_GPUShader *CreateShader(SDL_GPUDevice *device, SDL_GPUShaderStage stage, const uint32_t *code,
                                   size_t code_size, uint32_t num_samplers, uint32_t num_uniform_buffers,
                                   const char *name, uint32_t num_storage_buffers = 0)
{
    SDL_GPUShaderCreateInfo info;
    EPI_CLEAR_MEMORY(&info, SDL_GPUShaderCreateInfo, 1);

    info.code                 = (const uint8_t *)code;
    info.code_size            = code_size;
    info.entrypoint           = "main";
    info.format               = SDL_GPU_SHADERFORMAT_SPIRV;
    info.stage                = stage;
    info.num_samplers         = num_samplers;
    info.num_storage_textures = 0;
    info.num_storage_buffers  = num_storage_buffers;
    info.num_uniform_buffers  = num_uniform_buffers;

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_GPU_SHADER_CREATE_NAME_STRING, name);
    info.props = props;

    SDL_GPUShader *shader = SDL_CreateGPUShader(device, &info);

    SDL_DestroyProperties(props);

    if (!shader)
        LogPrint("GpuShaders: SDL_CreateGPUShader failed for '%s': %s\n", name, SDL_GetError());

    return shader;
}

bool CreateWorldShaders(SDL_GPUDevice *device)
{
    if (world_vertex_shader && world_fragment_shader)
        return true;

    world_vertex_shader =
        CreateShader(device, SDL_GPU_SHADERSTAGE_VERTEX, kWorldVertexShaderSpirv, sizeof(kWorldVertexShaderSpirv),
                     kWorldVertexShaderSamplerCount, kWorldVertexShaderUniformBufferCount, "world.vert");

    world_fragment_shader =
        CreateShader(device, SDL_GPU_SHADERSTAGE_FRAGMENT, kWorldFragmentShaderSpirv, sizeof(kWorldFragmentShaderSpirv),
                     kWorldFragmentShaderSamplerCount, kWorldFragmentShaderUniformBufferCount, "world.frag",
                     kWorldFragmentShaderStorageBufferCount);

    world_oit_fragment_shader = CreateShader(
        device, SDL_GPU_SHADERSTAGE_FRAGMENT, kWorldOitFragmentShaderSpirv, sizeof(kWorldOitFragmentShaderSpirv),
        kWorldOitFragmentShaderSamplerCount, kWorldOitFragmentShaderUniformBufferCount, "world_oit.frag",
        kWorldOitFragmentShaderStorageBufferCount);

    if (!world_vertex_shader || !world_fragment_shader || !world_oit_fragment_shader)
    {
        DestroyWorldShaders(device);
        return false;
    }

    return true;
}

void DestroyWorldShaders(SDL_GPUDevice *device)
{
    if (world_vertex_shader)
    {
        SDL_ReleaseGPUShader(device, world_vertex_shader);
        world_vertex_shader = nullptr;
    }

    if (world_fragment_shader)
    {
        SDL_ReleaseGPUShader(device, world_fragment_shader);
        world_fragment_shader = nullptr;
    }

    if (world_oit_fragment_shader)
    {
        SDL_ReleaseGPUShader(device, world_oit_fragment_shader);
        world_oit_fragment_shader = nullptr;
    }
}

SDL_GPUShader *WorldVertexShader()
{
    return world_vertex_shader;
}

SDL_GPUShader *WorldFragmentShader()
{
    return world_fragment_shader;
}

SDL_GPUShader *WorldOitFragmentShader()
{
    return world_oit_fragment_shader;
}





bool CreateModelShaders(SDL_GPUDevice *device)
{
    if (model_vertex_shader && model_fragment_shader)
        return true;

    model_vertex_shader =
        CreateShader(device, SDL_GPU_SHADERSTAGE_VERTEX, kModelVertexShaderSpirv, sizeof(kModelVertexShaderSpirv),
                     kModelVertexShaderSamplerCount, kModelVertexShaderUniformBufferCount, "model.vert");

    model_fragment_shader =
        CreateShader(device, SDL_GPU_SHADERSTAGE_FRAGMENT, kModelFragmentShaderSpirv, sizeof(kModelFragmentShaderSpirv),
                     kModelFragmentShaderSamplerCount, kModelFragmentShaderUniformBufferCount, "model.frag", kModelFragmentShaderStorageBufferCount);

    model_oit_fragment_shader = CreateShader(
        device, SDL_GPU_SHADERSTAGE_FRAGMENT, kModelOitFragmentShaderSpirv, sizeof(kModelOitFragmentShaderSpirv),
        kModelOitFragmentShaderSamplerCount, kModelOitFragmentShaderUniformBufferCount, "model_oit.frag",
        kModelOitFragmentShaderStorageBufferCount);

    if (!model_vertex_shader || !model_fragment_shader || !model_oit_fragment_shader)
    {
        DestroyModelShaders(device);
        return false;
    }

    return true;
}

void DestroyModelShaders(SDL_GPUDevice *device)
{
    if (model_vertex_shader)
    {
        SDL_ReleaseGPUShader(device, model_vertex_shader);
        model_vertex_shader = nullptr;
    }

    if (model_fragment_shader)
    {
        SDL_ReleaseGPUShader(device, model_fragment_shader);
        model_fragment_shader = nullptr;
    }

    if (model_oit_fragment_shader)
    {
        SDL_ReleaseGPUShader(device, model_oit_fragment_shader);
        model_oit_fragment_shader = nullptr;
    }
}

SDL_GPUShader *ModelVertexShader()
{
    return model_vertex_shader;
}

SDL_GPUShader *ModelFragmentShader()
{
    return model_fragment_shader;
}

SDL_GPUShader *ModelOitFragmentShader()
{
    return model_oit_fragment_shader;
}

bool CreateMovieShaders(SDL_GPUDevice *device)
{
    if (movie_vertex_shader && movie_fragment_shader)
        return true;

    movie_vertex_shader =
        CreateShader(device, SDL_GPU_SHADERSTAGE_VERTEX, kMovieVertexShaderSpirv, sizeof(kMovieVertexShaderSpirv),
                     kMovieVertexShaderSamplerCount, kMovieVertexShaderUniformBufferCount, "movie.vert");

    movie_fragment_shader =
        CreateShader(device, SDL_GPU_SHADERSTAGE_FRAGMENT, kMovieFragmentShaderSpirv, sizeof(kMovieFragmentShaderSpirv),
                     kMovieFragmentShaderSamplerCount, kMovieFragmentShaderUniformBufferCount, "movie.frag");

    if (!movie_vertex_shader || !movie_fragment_shader)
    {
        DestroyMovieShaders(device);
        return false;
    }

    return true;
}

void DestroyMovieShaders(SDL_GPUDevice *device)
{
    if (movie_vertex_shader)
    {
        SDL_ReleaseGPUShader(device, movie_vertex_shader);
        movie_vertex_shader = nullptr;
    }

    if (movie_fragment_shader)
    {
        SDL_ReleaseGPUShader(device, movie_fragment_shader);
        movie_fragment_shader = nullptr;
    }
}

SDL_GPUShader *MovieVertexShader(void)
{
    return movie_vertex_shader;
}

SDL_GPUShader *MovieFragmentShader(void)
{
    return movie_fragment_shader;
}
