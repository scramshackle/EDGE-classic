#include "gles2_loader.h"

#include <SDL3/SDL_video.h>

#include "epi.h"
#include "i_system.h"
#include "r_lightgrid.h"
#include "stb_sprintf.h"

static const char *Gles2ShaderDefines()
{
    static char defines[128];

    stbsp_snprintf(defines, sizeof(defines), "#define EDGE_LIGHT_MAX_PER_TILE %d\n#define EDGE_LIGHT_MAX_GLOWS %d\n",
                   kLightGridMaximumPerTile, kLightGridMaximumGlows);

    return defines;
}


#ifdef EDGE_GLES2_DESKTOP_GL

#define EDGE_GLES2_DEFINE(type, name) type ec_##name = nullptr;
EDGE_GLES2_GL_FUNCTIONS(EDGE_GLES2_DEFINE)
EDGE_GLES2_GL_FRAMEBUFFER_FUNCTIONS(EDGE_GLES2_DEFINE)
#undef EDGE_GLES2_DEFINE

static bool gles2_framebuffer_objects_available = false;

void Gles2LoadEntryPoints()
{
#define EDGE_GLES2_LOAD(type, name)                                                                                    \
    ec_##name = (type)SDL_GL_GetProcAddress(#name);                                                                    \
    if (!ec_##name)                                                                                                    \
        FatalError("OpenGL: required OpenGL 2.0 entry point %s is unavailable\n", #name);

    EDGE_GLES2_GL_FUNCTIONS(EDGE_GLES2_LOAD)
#undef EDGE_GLES2_LOAD

    gles2_framebuffer_objects_available = true;

#define EDGE_GLES2_LOAD_FRAMEBUFFER(type, name)                                                                        \
    ec_##name = (type)SDL_GL_GetProcAddress(#name);                                                                    \
    if (!ec_##name)                                                                                                    \
        ec_##name = (type)SDL_GL_GetProcAddress(#name "EXT");                                                          \
    if (!ec_##name)                                                                                                    \
        FatalError("OpenGL: framebuffer objects are required but %s is unavailable\n", #name);

    EDGE_GLES2_GL_FRAMEBUFFER_FUNCTIONS(EDGE_GLES2_LOAD_FRAMEBUFFER)
#undef EDGE_GLES2_LOAD_FRAMEBUFFER
}

bool Gles2HasFramebufferObjects()
{
    return gles2_framebuffer_objects_available;
}

int32_t Gles2MaxVaryingVectors()
{
    GLint varying_floats = 0;

    glGetIntegerv(GL_MAX_VARYING_FLOATS, &varying_floats);

    return (int32_t)(varying_floats / 4);
}

const char *Gles2ShaderPreamble(bool fragment_stage)
{
    EPI_UNUSED(fragment_stage);

    static char preamble[256];

    stbsp_snprintf(preamble, sizeof(preamble), "#version 110\n%s", Gles2ShaderDefines());

    return preamble;
}

#else

void Gles2LoadEntryPoints()
{
}

bool Gles2HasFramebufferObjects()
{
    return true;
}

int32_t Gles2MaxVaryingVectors()
{
    GLint varying_vectors = 0;

    glGetIntegerv(GL_MAX_VARYING_VECTORS, &varying_vectors);

    return (int32_t)varying_vectors;
}

const char *Gles2ShaderPreamble(bool fragment_stage)
{
    static char preamble[256];

    if (fragment_stage)
    {
        stbsp_snprintf(preamble, sizeof(preamble),
                       "#version 100\n%s"
                       "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
                       "precision highp float;\n"
                       "#else\n"
                       "precision mediump float;\n"
                       "#endif\n",
                       Gles2ShaderDefines());

        return preamble;
    }

    stbsp_snprintf(preamble, sizeof(preamble), "#version 100\n%s", Gles2ShaderDefines());

    return preamble;
}

#endif
