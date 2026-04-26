// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  OpenGL host-readback path for the 'I' key atlas capture.
 *
 * Binds `srcTex` to a private FBO and uses glReadPixels to pull RGBA8 into
 * host memory. Then flips Y (GL origin is bottom-left; PNGs and the rest of
 * the capture pipeline are top-left) and writes a PNG via stb_image_write.
 *
 * On Windows we resolve the FBO entry points via wglGetProcAddress so the
 * helper doesn't depend on the cube_handle_gl_win app's gl_functions.cpp
 * loader. On macOS, OpenGL is part of the system framework — symbols are
 * available directly. The caller must have a current GL context bound.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>
  #include <GL/gl.h>
  // FBO function-pointer types.
  #include <stddef.h>
  #ifndef GL_FRAMEBUFFER
  #define GL_FRAMEBUFFER 0x8D40
  #endif
  #ifndef GL_COLOR_ATTACHMENT0
  #define GL_COLOR_ATTACHMENT0 0x8CE0
  #endif
  #ifndef GL_FRAMEBUFFER_COMPLETE
  #define GL_FRAMEBUFFER_COMPLETE 0x8CD5
  #endif
  typedef void   (APIENTRY *PFN_glGenFramebuffers)(GLsizei, GLuint*);
  typedef void   (APIENTRY *PFN_glBindFramebuffer)(GLenum, GLuint);
  typedef void   (APIENTRY *PFN_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
  typedef void   (APIENTRY *PFN_glDeleteFramebuffers)(GLsizei, const GLuint*);
  typedef GLenum (APIENTRY *PFN_glCheckFramebufferStatus)(GLenum);
#elif defined(__APPLE__)
  #include <OpenGL/gl3.h>     // FBO is core in 3.0+
#else
  #include <GL/gl.h>
  #include <GL/glext.h>
#endif

#include "stb_image_write.h"
#include "atlas_capture.h"

namespace dxr_capture {

namespace {

#if defined(_WIN32)
PFN_glGenFramebuffers        p_glGenFramebuffers        = nullptr;
PFN_glBindFramebuffer        p_glBindFramebuffer        = nullptr;
PFN_glFramebufferTexture2D   p_glFramebufferTexture2D   = nullptr;
PFN_glDeleteFramebuffers     p_glDeleteFramebuffers     = nullptr;
PFN_glCheckFramebufferStatus p_glCheckFramebufferStatus = nullptr;

bool LoadFboEntries() {
    if (p_glGenFramebuffers) return true;  // already loaded
    p_glGenFramebuffers = (PFN_glGenFramebuffers)
        wglGetProcAddress("glGenFramebuffers");
    p_glBindFramebuffer = (PFN_glBindFramebuffer)
        wglGetProcAddress("glBindFramebuffer");
    p_glFramebufferTexture2D = (PFN_glFramebufferTexture2D)
        wglGetProcAddress("glFramebufferTexture2D");
    p_glDeleteFramebuffers = (PFN_glDeleteFramebuffers)
        wglGetProcAddress("glDeleteFramebuffers");
    p_glCheckFramebufferStatus = (PFN_glCheckFramebufferStatus)
        wglGetProcAddress("glCheckFramebufferStatus");
    return p_glGenFramebuffers && p_glBindFramebuffer &&
           p_glFramebufferTexture2D && p_glDeleteFramebuffers &&
           p_glCheckFramebufferStatus;
}

inline void   GenFbo(GLsizei n, GLuint* o)                                { p_glGenFramebuffers(n, o); }
inline void   BindFbo(GLenum t, GLuint id)                                { p_glBindFramebuffer(t, id); }
inline void   FboTex2D(GLenum t, GLenum a, GLenum tt, GLuint id, GLint l) { p_glFramebufferTexture2D(t, a, tt, id, l); }
inline void   DelFbo(GLsizei n, const GLuint* o)                          { p_glDeleteFramebuffers(n, o); }
inline GLenum CheckFbo(GLenum t)                                          { return p_glCheckFramebufferStatus(t); }
#else
inline void   GenFbo(GLsizei n, GLuint* o)                                { glGenFramebuffers(n, o); }
inline void   BindFbo(GLenum t, GLuint id)                                { glBindFramebuffer(t, id); }
inline void   FboTex2D(GLenum t, GLenum a, GLenum tt, GLuint id, GLint l) { glFramebufferTexture2D(t, a, tt, id, l); }
inline void   DelFbo(GLsizei n, const GLuint* o)                          { glDeleteFramebuffers(n, o); }
inline GLenum CheckFbo(GLenum t)                                          { return glCheckFramebufferStatus(t); }
inline bool   LoadFboEntries() { return true; }
#endif

}  // namespace

bool CaptureAtlasRegionGL(uint32_t srcTex,
                          uint32_t srcImageWidth,
                          uint32_t srcImageHeight,
                          uint32_t rectX,
                          uint32_t rectY,
                          uint32_t rectW,
                          uint32_t rectH,
                          const std::string& outPath) {
    if (srcTex == 0) return false;
    if (rectW == 0 || rectH == 0) return false;
    if ((uint64_t)rectX + rectW > srcImageWidth) return false;
    if ((uint64_t)rectY + rectH > srcImageHeight) return false;
    if (!LoadFboEntries()) return false;

    // Make sure prior rendering to `srcTex` has actually committed before we
    // read it back — the app's draw commands are still pipelined at this
    // point and glReadPixels alone is not enough on every driver.
    glFinish();

    // Save the previously bound FBO so we don't clobber the caller's state.
    GLint prevReadFbo = 0;
    glGetIntegerv(0x8CAA /* GL_READ_FRAMEBUFFER_BINDING */, &prevReadFbo);

    GLuint fbo = 0;
    GenFbo(1, &fbo);
    BindFbo(0x8CA8 /* GL_READ_FRAMEBUFFER */, fbo);

    // Swapchain textures may be GL_TEXTURE_2D (Win + GL native compositor)
    // or GL_TEXTURE_RECTANGLE (macOS Metal compositor, IOSurface-backed).
    // Try TEXTURE_2D first; if the FBO is incomplete (because the texture
    // was created as RECTANGLE), detach and try RECTANGLE. This avoids the
    // ambiguity of probing via `glBindTexture(GL_TEXTURE_2D, …)` which
    // silently fails for rectangle textures and returns stale bindings.
    while (glGetError() != GL_NO_ERROR) { /* clear */ }
    FboTex2D(0x8CA8 /* GL_READ_FRAMEBUFFER */,
             GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, srcTex, 0);
    GLenum errAfter2D = glGetError();
    GLenum status = CheckFbo(0x8CA8 /* GL_READ_FRAMEBUFFER */);
    GLenum chosenTarget = GL_TEXTURE_2D;
#ifdef GL_TEXTURE_RECTANGLE
    if (status != GL_FRAMEBUFFER_COMPLETE || errAfter2D != GL_NO_ERROR) {
        FboTex2D(0x8CA8 /* GL_READ_FRAMEBUFFER */,
                 GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        while (glGetError() != GL_NO_ERROR) { /* clear */ }
        FboTex2D(0x8CA8 /* GL_READ_FRAMEBUFFER */,
                 GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, srcTex, 0);
        status = CheckFbo(0x8CA8 /* GL_READ_FRAMEBUFFER */);
        chosenTarget = GL_TEXTURE_RECTANGLE;
    }
#endif
    (void)chosenTarget; (void)errAfter2D;
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        BindFbo(0x8CA8 /* GL_READ_FRAMEBUFFER */, (GLuint)prevReadFbo);
        DelFbo(1, &fbo);
        return false;
    }

    // glReadBuffer must be set explicitly for a custom FBO — the default
    // GL_COLOR_ATTACHMENT0 is only guaranteed in 3.0+ core, and even then
    // some drivers return zeroed pixels if it's not set.
    glReadBuffer(GL_COLOR_ATTACHMENT0);

    // OpenXR-GL convention (XR_KHR_opengl_enable): the swapchain image's
    // origin is GL bottom-left, so the app's `vpY=0` viewport renders at
    // the bottom of the GL texture. The atlas region the caller wants is
    // anchored at GL (rectX, rectY) directly — no flip-into-image-space.
    // We still flip the readback rows below so the PNG is top-down.
    GLint glY = (GLint)rectY;
    (void)srcImageHeight;

    // Tight pack alignment.
    GLint prevAlign = 0;
    glGetIntegerv(GL_PACK_ALIGNMENT, &prevAlign);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    std::vector<uint8_t> rgba((size_t)rectW * rectH * 4u);
    glReadPixels((GLint)rectX, glY, (GLsizei)rectW, (GLsizei)rectH,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

    glPixelStorei(GL_PACK_ALIGNMENT, prevAlign);
    BindFbo(0x8CA8 /* GL_READ_FRAMEBUFFER */, (GLuint)prevReadFbo);
    DelFbo(1, &fbo);

    // Flip Y so the PNG matches the on-screen / atlas top-left orientation.
    std::vector<uint8_t> flipped((size_t)rectW * rectH * 4u);
    const size_t rowBytes = (size_t)rectW * 4u;
    for (uint32_t y = 0; y < rectH; y++) {
        std::memcpy(flipped.data() + (size_t)y * rowBytes,
                    rgba.data() + (size_t)(rectH - 1 - y) * rowBytes,
                    rowBytes);
    }

    int ok = stbi_write_png(outPath.c_str(), (int)rectW, (int)rectH, 4,
                            flipped.data(), (int)rowBytes);
    return ok != 0;
}

}  // namespace dxr_capture
