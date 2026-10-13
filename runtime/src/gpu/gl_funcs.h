// Funciones de OpenGL 3.3 core cargadas en tiempo de ejecución (SDL_GL_GetProcAddress), igual en Linux y Windows.
#pragma once
#include <GL/glcorearb.h>  // runtime/third_party (Khronos)

#define CARS_GL_FUNCS(X)                                                                   \
    X(PFNGLGETERRORPROC, glGetError)                                                       \
    X(PFNGLGETSTRINGPROC, glGetString)                                                     \
    X(PFNGLENABLEPROC, glEnable)                                                           \
    X(PFNGLDISABLEPROC, glDisable)                                                         \
    X(PFNGLVIEWPORTPROC, glViewport)                                                       \
    X(PFNGLSCISSORPROC, glScissor)                                                         \
    X(PFNGLCLEARCOLORPROC, glClearColor)                                                   \
    X(PFNGLCLEARDEPTHPROC, glClearDepth)                                                   \
    X(PFNGLCLEARPROC, glClear)                                                             \
    X(PFNGLCOLORMASKPROC, glColorMask)                                                     \
    X(PFNGLDEPTHMASKPROC, glDepthMask)                                                     \
    X(PFNGLDEPTHFUNCPROC, glDepthFunc)                                                     \
    X(PFNGLSTENCILFUNCPROC, glStencilFunc)                                                 \
    X(PFNGLSTENCILOPPROC, glStencilOp)                                                     \
    X(PFNGLSTENCILMASKPROC, glStencilMask)                                                 \
    X(PFNGLCLEARSTENCILPROC, glClearStencil)                                               \
    X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate)                                     \
    X(PFNGLBLENDEQUATIONSEPARATEPROC, glBlendEquationSeparate)                             \
    X(PFNGLBLENDCOLORPROC, glBlendColor)                                                   \
    X(PFNGLCULLFACEPROC, glCullFace)                                                       \
    X(PFNGLFRONTFACEPROC, glFrontFace)                                                     \
    X(PFNGLPIXELSTOREIPROC, glPixelStorei)                                                 \
    X(PFNGLREADPIXELSPROC, glReadPixels)                                                   \
    X(PFNGLGENTEXTURESPROC, glGenTextures)                                                 \
    X(PFNGLDELETETEXTURESPROC, glDeleteTextures)                                           \
    X(PFNGLBINDTEXTUREPROC, glBindTexture)                                                 \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture)                                             \
    X(PFNGLTEXIMAGE2DPROC, glTexImage2D)                                                   \
    X(PFNGLTEXSUBIMAGE2DPROC, glTexSubImage2D)                                             \
    X(PFNGLTEXPARAMETERIPROC, glTexParameteri)                                             \
    X(PFNGLTEXPARAMETERFPROC, glTexParameterf)                                             \
    X(PFNGLGETFLOATVPROC, glGetFloatv)                                                     \
    X(PFNGLGENERATEMIPMAPPROC, glGenerateMipmap)                                           \
    X(PFNGLGENBUFFERSPROC, glGenBuffers)                                                   \
    X(PFNGLBINDBUFFERPROC, glBindBuffer)                                                   \
    X(PFNGLBUFFERDATAPROC, glBufferData)                                                   \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays)                                         \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)                                         \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)                         \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)                                 \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays)                                                   \
    X(PFNGLCREATESHADERPROC, glCreateShader)                                               \
    X(PFNGLSHADERSOURCEPROC, glShaderSource)                                               \
    X(PFNGLCOMPILESHADERPROC, glCompileShader)                                             \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv)                                                 \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                                       \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                                             \
    X(PFNGLATTACHSHADERPROC, glAttachShader)                                               \
    X(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation)                                   \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram)                                                 \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                                               \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                                     \
    X(PFNGLUSEPROGRAMPROC, glUseProgram)                                                   \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)                                   \
    X(PFNGLUNIFORM1IPROC, glUniform1i)                                                     \
    X(PFNGLUNIFORM1FPROC, glUniform1f)                                                     \
    X(PFNGLUNIFORM2FPROC, glUniform2f)                                                     \
    X(PFNGLUNIFORM3IPROC, glUniform3i)                                                     \
    X(PFNGLUNIFORM3FPROC, glUniform3f)                                                     \
    X(PFNGLUNIFORM4FPROC, glUniform4f)                                                     \
    X(PFNGLUNIFORM3FVPROC, glUniform3fv)                                                   \
    X(PFNGLUNIFORM4FVPROC, glUniform4fv)                                                   \
    X(PFNGLUNIFORM1IVPROC, glUniform1iv)                                                   \
    X(PFNGLUNIFORM1FVPROC, glUniform1fv)                                                   \
    X(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv)                                       \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers)                                         \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)                                         \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D)                               \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer)                         \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus)                           \
    X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers)                                       \
    X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer)                                       \
    X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage)                                 \
    X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer)

#define CARS_GL_DECLARE(type, name) extern type name;
CARS_GL_FUNCS(CARS_GL_DECLARE)
#undef CARS_GL_DECLARE

bool gl_load_functions();
