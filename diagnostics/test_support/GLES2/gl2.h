#pragma once
#include <stddef.h>
#include <stdint.h>
typedef unsigned int GLenum; typedef unsigned int GLuint;typedef int GLint;typedef int GLsizei;typedef long GLsizeiptr;typedef long GLintptr;typedef unsigned char GLboolean; typedef float GLfloat;typedef char GLchar;typedef unsigned int GLbitfield;
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_FLOAT 0x1406
#define GL_FALSE 0
#define GL_TRIANGLES 0x0004
#define GL_ARRAY_BUFFER 0x8892
#define GL_STREAM_DRAW 0x88E0
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_STATIC_DRAW 0x88E4
#define GL_BLEND 0x0BE2
#define GL_ONE 1
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_SCISSOR_TEST 0x0C11
#define GL_DITHER 0x0BD0
#define GL_ALPHA_BITS 0x0D55
#define GL_EXTENSIONS 0x1F03
#define GL_VERSION 0x1F02
#define GL_FUNC_ADD 0x8006
#define GL_DST_ALPHA 0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_ZERO 0
#define GL_TRUE 1
#define GL_NO_ERROR 0
#define GL_OUT_OF_MEMORY 0x0505
#ifdef __cplusplus
extern "C" {
#endif
void glDisable(GLenum);void glScissor(GLint,GLint,GLsizei,GLsizei);void glColorMask(GLboolean,GLboolean,GLboolean,GLboolean);
void glBlendEquation(GLenum);void glBlendFuncSeparate(GLenum,GLenum,GLenum,GLenum);void glUniform1i(GLint,GLint);
const unsigned char* glGetString(GLenum);void glGetIntegerv(GLenum,GLint*);
GLenum glGetError(void);
GLuint glCreateShader(GLenum);void glShaderSource(GLuint,GLsizei,const GLchar*const*,const GLint*);void glCompileShader(GLuint);void glGetShaderiv(GLuint,GLenum,GLint*);void glGetShaderInfoLog(GLuint,GLsizei,GLsizei*,GLchar*);void glDeleteShader(GLuint);GLuint glCreateProgram(void);void glAttachShader(GLuint,GLuint);void glBindAttribLocation(GLuint,GLuint,const GLchar*);void glLinkProgram(GLuint);void glGetProgramiv(GLuint,GLenum,GLint*);void glGetProgramInfoLog(GLuint,GLsizei,GLsizei*,GLchar*);void glDeleteProgram(GLuint);GLboolean glIsProgram(GLuint);GLboolean glIsBuffer(GLuint);void glDeleteBuffers(GLsizei,const GLuint*);void glGenBuffers(GLsizei,GLuint*);void glBindBuffer(GLenum,GLuint);void glBufferData(GLenum,GLsizeiptr,const void*,GLenum);void glBufferSubData(GLenum,GLintptr,GLsizeiptr,const void*);void glViewport(GLint,GLint,GLsizei,GLsizei);GLint glGetAttribLocation(GLuint,const GLchar*);GLint glGetUniformLocation(GLuint,const GLchar*);void glUseProgram(GLuint);void glUniform4f(GLint,GLfloat,GLfloat,GLfloat,GLfloat);void glUniform2f(GLint,GLfloat,GLfloat);void glUniform1f(GLint,GLfloat);void glVertexAttribPointer(GLuint,GLint,GLenum,GLboolean,GLsizei,const void*);void glEnableVertexAttribArray(GLuint);void glDrawArrays(GLenum,GLint,GLsizei);void glClearColor(GLfloat,GLfloat,GLfloat,GLfloat);void glClear(GLbitfield);void glEnable(GLenum);void glBlendFunc(GLenum,GLenum);
#ifdef __cplusplus
}
#endif
