#include "gles2_loader.h"

#include <SDL3/SDL_video.h>

#include "epi.h"
#include "i_system.h"

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

#define EDGE_GLES2_LOAD_OPTIONAL(type, name)                                                                           \
    ec_##name = (type)SDL_GL_GetProcAddress(#name);                                                                    \
    if (!ec_##name)                                                                                                    \
        ec_##name = (type)SDL_GL_GetProcAddress(#name "EXT");                                                          \
    if (!ec_##name)                                                                                                    \
        gles2_framebuffer_objects_available = false;

    EDGE_GLES2_GL_FRAMEBUFFER_FUNCTIONS(EDGE_GLES2_LOAD_OPTIONAL)
#undef EDGE_GLES2_LOAD_OPTIONAL
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

    return "#version 110\n";
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
    if (fragment_stage)
    {
        return "#version 100\n"
               "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
               "precision highp float;\n"
               "#else\n"
               "precision mediump float;\n"
               "#endif\n";
    }

    return "#version 100\n";
}

#endif
