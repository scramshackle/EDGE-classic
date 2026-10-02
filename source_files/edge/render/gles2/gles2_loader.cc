#include "gles2_loader.h"

#include <SDL3/SDL_video.h>
#include <stdio.h>

#include "epi.h"
#include "i_system.h"
#include "r_lightgrid.h"
#include "stb_sprintf.h"

static const char *Gles2ShaderDefines()
{
    static char defines[128];

    stbsp_snprintf(defines, sizeof(defines), "#define EDGE_LIGHT_MAX_PER_CLUSTER %d\n#define EDGE_LIGHT_MAX_GLOWS %d\n",
                   kLightGridMaximumPerCluster, kLightGridMaximumGlows);

    return defines;
}


Gles2DrawElementsInstancedFunction gles2_draw_elements_instanced = nullptr;
Gles2VertexAttribDivisorFunction   gles2_vertex_attrib_divisor   = nullptr;

static bool LoadInstancingEntryPoints(const char *suffix)
{
    char name[64];

    stbsp_snprintf(name, sizeof(name), "glDrawElementsInstanced%s", suffix);
    gles2_draw_elements_instanced = (Gles2DrawElementsInstancedFunction)SDL_GL_GetProcAddress(name);

    stbsp_snprintf(name, sizeof(name), "glVertexAttribDivisor%s", suffix);
    gles2_vertex_attrib_divisor = (Gles2VertexAttribDivisorFunction)SDL_GL_GetProcAddress(name);

    return gles2_draw_elements_instanced && gles2_vertex_attrib_divisor;
}

static void LoadInstancing()
{
    int major = 0;
    int minor = 0;

    const char *version = (const char *)glGetString(GL_VERSION);

#ifdef EDGE_GLES2_DESKTOP_GL
    if (version)
        sscanf(version, "%d.%d", &major, &minor);

    if ((major > 3 || (major == 3 && minor >= 3)) && LoadInstancingEntryPoints(""))
        return;

    if (SDL_GL_ExtensionSupported("GL_ARB_instanced_arrays") && LoadInstancingEntryPoints("ARB"))
        return;

    FatalError("OpenGL: instanced rendering is required (OpenGL 3.3 or GL_ARB_instanced_arrays)\n");
#else
    if (version)
        sscanf(version, "OpenGL ES %d.%d", &major, &minor);

    if (major >= 3 && LoadInstancingEntryPoints(""))
        return;

    if (SDL_GL_ExtensionSupported("GL_ANGLE_instanced_arrays") && LoadInstancingEntryPoints("ANGLE"))
        return;

    if (SDL_GL_ExtensionSupported("GL_EXT_instanced_arrays") && LoadInstancingEntryPoints("EXT"))
        return;

    FatalError("OpenGL ES: instanced rendering is required (OpenGL ES 3.0, ANGLE_instanced_arrays or "
               "EXT_instanced_arrays)\n");
#endif
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

    LoadInstancing();
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
    LoadInstancing();
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
