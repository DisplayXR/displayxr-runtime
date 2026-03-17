// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Manual OpenGL function pointer loading via wglGetProcAddress
 *
 * Loads the minimal set of GL 3.3+ functions needed for rendering.
 * No external glad/GLEW dependency.
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <GL/gl.h>

// GL types not in gl.h
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;

// GL constants
#define GL_FRAGMENT_SHADER                0x8B30
#define GL_VERTEX_SHADER                  0x8B31
#define GL_COMPILE_STATUS                 0x8B81
#define GL_LINK_STATUS                    0x8B82
#define GL_INFO_LOG_LENGTH                0x8B84
#define GL_ARRAY_BUFFER                   0x8892
#define GL_ELEMENT_ARRAY_BUFFER           0x8893
#define GL_STATIC_DRAW                    0x88E4
#define GL_FRAMEBUFFER                    0x8D40
#define GL_RENDERBUFFER                   0x8D41
#define GL_COLOR_ATTACHMENT0              0x8CE0
#define GL_DEPTH_ATTACHMENT               0x8D00
#define GL_DEPTH_COMPONENT24              0x81A6
#define GL_FRAMEBUFFER_COMPLETE           0x8CD5
#define GL_SRGB8_ALPHA8                   0x8C43
#define GL_CLAMP_TO_EDGE                  0x812F
#define GL_READ_FRAMEBUFFER               0x8CA8
#define GL_DRAW_FRAMEBUFFER               0x8CA9
#define GL_RGBA8                          0x8058
#define GL_TEXTURE_WRAP_S                 0x2802
#define GL_TEXTURE_WRAP_T                 0x2803
#define GL_TEXTURE0                       0x84C0
#ifndef GL_LINEAR_MIPMAP_LINEAR
#define GL_LINEAR_MIPMAP_LINEAR           0x2703
#endif

// WGL constants
#define WGL_CONTEXT_MAJOR_VERSION_ARB     0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB     0x2092
#define WGL_CONTEXT_FLAGS_ARB             0x2094
#define WGL_CONTEXT_PROFILE_MASK_ARB      0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB  0x00000001

// Function pointer types
typedef GLuint (APIENTRY *PFNGLCREATESHADERPROC)(GLenum type);
typedef void (APIENTRY *PFNGLSHADERSOURCEPROC)(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length);
typedef void (APIENTRY *PFNGLCOMPILESHADERPROC)(GLuint shader);
typedef void (APIENTRY *PFNGLGETSHADERIVPROC)(GLuint shader, GLenum pname, GLint* params);
typedef void (APIENTRY *PFNGLGETSHADERINFOLOGPROC)(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
typedef GLuint (APIENTRY *PFNGLCREATEPROGRAMPROC)(void);
typedef void (APIENTRY *PFNGLATTACHSHADERPROC)(GLuint program, GLuint shader);
typedef void (APIENTRY *PFNGLLINKPROGRAMPROC)(GLuint program);
typedef void (APIENTRY *PFNGLGETPROGRAMIVPROC)(GLuint program, GLenum pname, GLint* params);
typedef void (APIENTRY *PFNGLGETPROGRAMINFOLOGPROC)(GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
typedef void (APIENTRY *PFNGLUSEPROGRAMPROC)(GLuint program);
typedef void (APIENTRY *PFNGLDELETESHADERPROC)(GLuint shader);
typedef void (APIENTRY *PFNGLDELETEPROGRAMPROC)(GLuint program);
typedef GLint (APIENTRY *PFNGLGETUNIFORMLOCATIONPROC)(GLuint program, const GLchar* name);
typedef void (APIENTRY *PFNGLUNIFORMMATRIX4FVPROC)(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
typedef void (APIENTRY *PFNGLUNIFORM4FPROC)(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
typedef void (APIENTRY *PFNGLGENVERTEXARRAYSPROC)(GLsizei n, GLuint* arrays);
typedef void (APIENTRY *PFNGLBINDVERTEXARRAYPROC)(GLuint array);
typedef void (APIENTRY *PFNGLDELETEVERTEXARRAYSPROC)(GLsizei n, const GLuint* arrays);
typedef void (APIENTRY *PFNGLGENBUFFERSPROC)(GLsizei n, GLuint* buffers);
typedef void (APIENTRY *PFNGLBINDBUFFERPROC)(GLenum target, GLuint buffer);
typedef void (APIENTRY *PFNGLBUFFERDATAPROC)(GLenum target, GLsizeiptr size, const void* data, GLenum usage);
typedef void (APIENTRY *PFNGLDELETEBUFFERSPROC)(GLsizei n, const GLuint* buffers);
typedef void (APIENTRY *PFNGLENABLEVERTEXATTRIBARRAYPROC)(GLuint index);
typedef void (APIENTRY *PFNGLVERTEXATTRIBPOINTERPROC)(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer);
typedef void (APIENTRY *PFNGLGENFRAMEBUFFERSPROC)(GLsizei n, GLuint* framebuffers);
typedef void (APIENTRY *PFNGLBINDFRAMEBUFFERPROC)(GLenum target, GLuint framebuffer);
typedef void (APIENTRY *PFNGLFRAMEBUFFERTEXTURE2DPROC)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
typedef GLenum (APIENTRY *PFNGLCHECKFRAMEBUFFERSTATUSPROC)(GLenum target);
typedef void (APIENTRY *PFNGLDELETEFRAMEBUFFERSPROC)(GLsizei n, const GLuint* framebuffers);
typedef void (APIENTRY *PFNGLGENRENDERBUFFERSPROC)(GLsizei n, GLuint* renderbuffers);
typedef void (APIENTRY *PFNGLBINDRENDERBUFFERPROC)(GLenum target, GLuint renderbuffer);
typedef void (APIENTRY *PFNGLRENDERBUFFERSTORAGEPROC)(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
typedef void (APIENTRY *PFNGLFRAMEBUFFERRENDERBUFFERPROC)(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
typedef void (APIENTRY *PFNGLDELETERENDERBUFFERSPROC)(GLsizei n, const GLuint* renderbuffers);
typedef void (APIENTRY *PFNGLBLITFRAMEBUFFERPROC)(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
    GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);
typedef void (APIENTRY *PFNGLACTIVETEXTUREPROC)(GLenum texture);
typedef void (APIENTRY *PFNGLUNIFORM1IPROC)(GLint location, GLint v0);
typedef void (APIENTRY *PFNGLGENERATEMIPMAPPROC)(GLenum target);

typedef HGLRC (APIENTRY *PFNWGLCREATECONTEXTATTRIBSARBPROC)(HDC hDC, HGLRC hShareContext, const int* attribList);

// Global function pointers
extern PFNGLCREATESHADERPROC glCreateShader_;
extern PFNGLSHADERSOURCEPROC glShaderSource_;
extern PFNGLCOMPILESHADERPROC glCompileShader_;
extern PFNGLGETSHADERIVPROC glGetShaderiv_;
extern PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog_;
extern PFNGLCREATEPROGRAMPROC glCreateProgram_;
extern PFNGLATTACHSHADERPROC glAttachShader_;
extern PFNGLLINKPROGRAMPROC glLinkProgram_;
extern PFNGLGETPROGRAMIVPROC glGetProgramiv_;
extern PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog_;
extern PFNGLUSEPROGRAMPROC glUseProgram_;
extern PFNGLDELETESHADERPROC glDeleteShader_;
extern PFNGLDELETEPROGRAMPROC glDeleteProgram_;
extern PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation_;
extern PFNGLUNIFORMMATRIX4FVPROC glUniformMatrix4fv_;
extern PFNGLUNIFORM4FPROC glUniform4f_;
extern PFNGLGENVERTEXARRAYSPROC glGenVertexArrays_;
extern PFNGLBINDVERTEXARRAYPROC glBindVertexArray_;
extern PFNGLDELETEVERTEXARRAYSPROC glDeleteVertexArrays_;
extern PFNGLGENBUFFERSPROC glGenBuffers_;
extern PFNGLBINDBUFFERPROC glBindBuffer_;
extern PFNGLBUFFERDATAPROC glBufferData_;
extern PFNGLDELETEBUFFERSPROC glDeleteBuffers_;
extern PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray_;
extern PFNGLVERTEXATTRIBPOINTERPROC glVertexAttribPointer_;
extern PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers_;
extern PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer_;
extern PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D_;
extern PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus_;
extern PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers_;
extern PFNGLGENRENDERBUFFERSPROC glGenRenderbuffers_;
extern PFNGLBINDRENDERBUFFERPROC glBindRenderbuffer_;
extern PFNGLRENDERBUFFERSTORAGEPROC glRenderbufferStorage_;
extern PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbuffer_;
extern PFNGLDELETERENDERBUFFERSPROC glDeleteRenderbuffers_;
extern PFNGLBLITFRAMEBUFFERPROC glBlitFramebuffer_;
extern PFNGLACTIVETEXTUREPROC glActiveTexture_;
extern PFNGLUNIFORM1IPROC glUniform1i_;
extern PFNGLGENERATEMIPMAPPROC glGenerateMipmap_;

extern PFNWGLCREATECONTEXTATTRIBSARBPROC wglCreateContextAttribsARB_;

// Load all GL function pointers. Must be called after a GL context is current.
bool LoadGLFunctions();
