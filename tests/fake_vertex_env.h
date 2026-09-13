/* fake_vertex_env.h -- the loader and GL calls the vertex cache reaches for.
 *
 * Included ahead of vertex_cache.c so that the cache compiles against these
 * rather than the real ones. The tests drive VertexBufferES_Lock and the sweep
 * directly, so nothing here needs to hook anything or talk to a GPU.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __FAKE_VERTEX_ENV_H__
#define __FAKE_VERTEX_ENV_H__

#include <stdint.h>
#include <psp2/io/stat.h>

// The device layout with m_data widened to a host pointer. Everything below
// 0x28 keeps its real offset; m_uploaded moves past the eight-byte pointer
// rather than sitting inside it, which is where 0x2c would land here.
#define VB_OFFSETS_OVERRIDDEN 1
#define VB_COUNT 0x0c
#define VB_DECLARATION 0x10
#define VB_GLBUFFER 0x24
#define VB_DATA 0x28
#define VB_UPLOADED (0x28 + sizeof(void *))
#include <vitaGL.h>

// so_util.h and main.h, reduced to what vertex_cache.c uses. Defining the
// include guards keeps the real headers out.
#define __SO_UTIL_H__
#define __MAIN_H__

typedef struct { int unused; } so_module;
static so_module bully_mod;

static uintptr_t so_symbol(so_module *mod, const char *name) {
  (void)mod; (void)name;
  return 0; // vertex_cache_init is not what these tests exercise
}
static void hook_addr(uintptr_t addr, uintptr_t dst) { (void)addr; (void)dst; }
static int traceLog(char *text, ...) { (void)text; return 0; }

// vertex_cache_init only calls this to look for its disable file. Answering
// "no such file" is the configuration the tests want anyway.
int sceIoGetstat(const char *file, SceIoStat *out) {
  (void)file; (void)out;
  return -1;
}

// The buffer object never reaches a driver here; what the sweep decides is what
// is under test. glMapBuffer returning NULL is the honest answer for a fake --
// the restore path then zeroes rather than copying, which is exactly what the
// loader documents for a readback it cannot do.
static unsigned fake_deleted_buffers;

static void fake_glBindBuffer(GLenum target, GLuint id) { (void)target; (void)id; }
static void fake_glDeleteBuffers(GLsizei n, const GLuint *ids) {
  (void)ids;
  fake_deleted_buffers += (unsigned)n;
}
static void *fake_glMapBuffer(GLenum target, GLenum access) {
  (void)target; (void)access;
  return NULL;
}
static GLboolean fake_glUnmapBuffer(GLenum target) { (void)target; return GL_TRUE; }

#define glBindBuffer fake_glBindBuffer
#define glDeleteBuffers fake_glDeleteBuffers
#define glMapBuffer fake_glMapBuffer
#define glUnmapBuffer fake_glUnmapBuffer

#endif
