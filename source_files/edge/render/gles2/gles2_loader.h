#pragma once

#include <stdint.h>

#ifdef EDGE_GLES2_DESKTOP_GL

#include <SDL3/SDL_opengl.h>

#define EDGE_GLES2_GL_FUNCTIONS(X)                                                                                     \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture)                                                                          \
    X(PFNGLATTACHSHADERPROC, glAttachShader)                                                                            \
    X(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation)                                                                \
    X(PFNGLBINDBUFFERPROC, glBindBuffer)                                                                                \
    X(PFNGLBUFFERDATAPROC, glBufferData)                                                                                \
    X(PFNGLBUFFERSUBDATAPROC, glBufferSubData)                                                                          \
    X(PFNGLCOMPILESHADERPROC, glCompileShader)                                                                          \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                                                                          \
    X(PFNGLCREATESHADERPROC, glCreateShader)                                                                            \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers)                                                                          \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram)                                                                          \
    X(PFNGLDELETESHADERPROC, glDeleteShader)                                                                            \
    X(PFNGLDETACHSHADERPROC, glDetachShader)                                                                            \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)                                                      \
    X(PFNGLGENBUFFERSPROC, glGenBuffers)                                                                                \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                                                                  \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                                                                            \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                                                                    \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv)                                                                              \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)                                                                \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram)                                                                              \
    X(PFNGLSHADERSOURCEPROC, glShaderSource)                                                                            \
    X(PFNGLUNIFORM1FPROC, glUniform1f)                                                                                  \
    X(PFNGLUNIFORM1IPROC, glUniform1i)                                                                                  \
    X(PFNGLUNIFORM2FPROC, glUniform2f)                                                                                  \
    X(PFNGLUNIFORM4FPROC, glUniform4f)                                                                                  \
    X(PFNGLUNIFORM4FVPROC, glUniform4fv)                                                                                \
    X(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv)                                                                    \
    X(PFNGLUSEPROGRAMPROC, glUseProgram)                                                                                \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)

#define EDGE_GLES2_GL_FRAMEBUFFER_FUNCTIONS(X)                                                                         \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)                                                                     \
    X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer)                                                                   \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus)                                                       \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers)                                                               \
    X(PFNGLDELETERENDERBUFFERSPROC, glDeleteRenderbuffers)                                                             \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer)                                                     \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D)                                                           \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers)                                                                     \
    X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers)                                                                   \
    X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage)

#define EDGE_GLES2_DECLARE(type, name) extern type ec_##name;
EDGE_GLES2_GL_FUNCTIONS(EDGE_GLES2_DECLARE)
EDGE_GLES2_GL_FRAMEBUFFER_FUNCTIONS(EDGE_GLES2_DECLARE)
#undef EDGE_GLES2_DECLARE

#define glActiveTexture            ec_glActiveTexture
#define glAttachShader             ec_glAttachShader
#define glBindAttribLocation       ec_glBindAttribLocation
#define glBindBuffer               ec_glBindBuffer
#define glBufferData               ec_glBufferData
#define glBufferSubData            ec_glBufferSubData
#define glCompileShader            ec_glCompileShader
#define glCreateProgram            ec_glCreateProgram
#define glCreateShader             ec_glCreateShader
#define glDeleteBuffers            ec_glDeleteBuffers
#define glDeleteProgram            ec_glDeleteProgram
#define glDeleteShader             ec_glDeleteShader
#define glDetachShader             ec_glDetachShader
#define glEnableVertexAttribArray  ec_glEnableVertexAttribArray
#define glGenBuffers               ec_glGenBuffers
#define glGetProgramInfoLog        ec_glGetProgramInfoLog
#define glGetProgramiv             ec_glGetProgramiv
#define glGetShaderInfoLog         ec_glGetShaderInfoLog
#define glGetShaderiv              ec_glGetShaderiv
#define glGetUniformLocation       ec_glGetUniformLocation
#define glLinkProgram              ec_glLinkProgram
#define glShaderSource             ec_glShaderSource
#define glUniform1f                ec_glUniform1f
#define glUniform1i                ec_glUniform1i
#define glUniform2f                ec_glUniform2f
#define glUniform4f                ec_glUniform4f
#define glUniform4fv               ec_glUniform4fv
#define glUniformMatrix4fv         ec_glUniformMatrix4fv
#define glUseProgram               ec_glUseProgram
#define glVertexAttribPointer      ec_glVertexAttribPointer

#define glBindFramebuffer          ec_glBindFramebuffer
#define glBindRenderbuffer         ec_glBindRenderbuffer
#define glCheckFramebufferStatus   ec_glCheckFramebufferStatus
#define glDeleteFramebuffers       ec_glDeleteFramebuffers
#define glDeleteRenderbuffers      ec_glDeleteRenderbuffers
#define glFramebufferRenderbuffer  ec_glFramebufferRenderbuffer
#define glFramebufferTexture2D     ec_glFramebufferTexture2D
#define glGenFramebuffers          ec_glGenFramebuffers
#define glGenRenderbuffers         ec_glGenRenderbuffers
#define glRenderbufferStorage      ec_glRenderbufferStorage

#else

#include <GLES2/gl2.h>

#endif

void Gles2LoadEntryPoints();

bool Gles2HasFramebufferObjects();

int32_t Gles2MaxVaryingVectors();

const char *Gles2ShaderPreamble(bool fragment_stage);
