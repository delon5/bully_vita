/* fake_vitagl.c -- a stand-in for the parts of vitaGL the texture cache drives.
 *
 * It models the behaviour the cache actually depends on: texture names are
 * recycled, re-specifying a texture frees what it held before, an upload the
 * driver rejects allocates nothing, and cube map faces live in a buffer of
 * their own. Running out of memory aborts, because that is the failure this
 * whole feature exists to prevent.
 *
 * Memory is split across pools the way vitaGL splits it, and allocation prefers
 * CDRAM and falls back to RAM. That is not a detail: on hardware CDRAM ran to
 * zero while the total still read 101 MB free, and a cache watching the total
 * evicted nothing at all. A single-pool fake cannot see that.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <stdlib.h>
#include "fake_vitagl.h"

size_t fake_slot_bytes[FAKE_SLOTS];
void *fake_slot_data[FAKE_SLOTS];
uint32_t fake_slot_content[FAKE_SLOTS];
int fake_slot_alive[FAKE_SLOTS];
GLuint fake_bound;
size_t fake_free_memory;   // the total, across every pool
size_t fake_pool_free[FAKE_POOLS];
size_t fake_pool_start[FAKE_POOLS];
// How much of each texture came out of each pool. Needed so that freeing gives
// the memory back where it came from: an approximation that refilled CDRAM last
// made eviction look like it could not recover CDRAM at all.
static size_t fake_slot_take[FAKE_SLOTS][FAKE_POOLS];
size_t fake_low_water;
int fake_reject_next_upload;

void fake_reset(size_t free_memory) {
  memset(fake_slot_bytes, 0, sizeof(fake_slot_bytes));
  memset(fake_slot_content, 0, sizeof(fake_slot_content));
  memset(fake_slot_alive, 0, sizeof(fake_slot_alive));
  memset(fake_slot_take, 0, sizeof(fake_slot_take));
  fake_bound = 0;
  fake_free_memory = free_memory;
  // All of it in CDRAM unless a test asks for a split, so the existing tests
  // keep meaning what they meant.
  fake_pool_free[0] = free_memory;
  for (int i = 1; i < FAKE_POOLS; i++)
    fake_pool_free[i] = 0;
  memcpy(fake_pool_start, fake_pool_free, sizeof(fake_pool_start));
  fake_low_water = free_memory;
  fake_reject_next_upload = 0;
}

void fake_set_pools(size_t cdram, size_t ram, size_t phycont) {
  fake_pool_free[0] = cdram;
  fake_pool_free[1] = ram;
  fake_pool_free[2] = phycont;
  memcpy(fake_pool_start, fake_pool_free, sizeof(fake_pool_start));
  fake_free_memory = cdram + ram + phycont;
  fake_low_water = fake_free_memory;
}

uint32_t fake_fingerprint_of(uint32_t tag) { return tag; }
// What a draw would sample. The bytes in the driver's buffer are the truth,
// not what was passed to the upload call: a cache that evicts a texture and
// writes those bytes back must be indistinguishable from one that never
// touched it.
uint32_t fake_sampled(GLuint id) {
  if (fake_slot_data[id])
    return *(uint32_t *)fake_slot_data[id];
  return fake_slot_content[id];
}

// The placeholder vitaGL leaves when handed a NULL pointer: filled with 0xFF.
#define PLACEHOLDER_CONTENT 0xFFFFFFFFu
// A cube map reads back as itself, never as one of its faces.
#define CUBE_CONTENT 0xCBCBCBCBu

static uint32_t fingerprint(const void *data, size_t size) {
  if (!data || size < 4)
    return PLACEHOLDER_CONTENT;
  return *(const uint32_t *)data;
}

static void set_slot(GLuint id, size_t size, uint32_t content) {
  fake_free_memory += fake_slot_bytes[id];
  for (int pool = 0; pool < FAKE_POOLS; pool++) {
    fake_pool_free[pool] += fake_slot_take[id][pool];
    fake_slot_take[id][pool] = 0;
  }
  free(fake_slot_data[id]);
  fake_slot_data[id] = NULL;
  fake_slot_bytes[id] = size;
  assert(fake_free_memory >= size && "driver ran out of memory: this is the crash being fixed");
  fake_free_memory -= size;
  // CDRAM first, then RAM, then whatever is left -- vitaGL's own order.
  size_t left = size;
  for (int pool = 0; pool < FAKE_POOLS && left; pool++) {
    size_t take = fake_pool_free[pool] < left ? fake_pool_free[pool] : left;
    fake_pool_free[pool] -= take;
    fake_slot_take[id][pool] = take;
    left -= take;
  }
  if (fake_free_memory < fake_low_water)
    fake_low_water = fake_free_memory;
  fake_slot_content[id] = content;
}

void glGenTextures(GLsizei n, GLuint *res) {
  for (GLsizei i = 0; i < n; i++) {
    GLuint id = 0;
    // vitaGL hands back the lowest free slot, so names really are recycled.
    for (GLuint slot = 1; slot < FAKE_SLOTS; slot++)
      if (!fake_slot_alive[slot]) { id = slot; break; }
    assert(id && "ran out of texture names");
    fake_slot_alive[id] = 1;
    free(fake_slot_data[id]);
    fake_slot_data[id] = NULL;
    fake_slot_bytes[id] = 0;
    fake_slot_content[id] = 0;
    res[i] = id;
  }
}

void glDeleteTextures(GLsizei n, const GLuint *ids) {
  for (GLsizei i = 0; i < n; i++) {
    set_slot(ids[i], 0, 0);
    fake_slot_alive[ids[i]] = 0;
  }
}

void glBindTexture(GLenum target, GLuint texture) { fake_bound = texture; }
void glActiveTexture(GLenum texture) { (void)texture; }
void glTexParameteri(GLenum target, GLenum pname, GLint param) { }
void glTexParameterf(GLenum target, GLenum pname, GLfloat param) { }
void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                     GLsizei height, GLenum format, GLenum type, const GLvoid *pixels) { }
void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture,
                            GLint level) { }

void glTexImage2D(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLsizei height,
                  GLint border, GLenum format, GLenum type, const GLvoid *data) {
  assert(fake_slot_alive[fake_bound]);
  if (fake_reject_next_upload) { fake_reject_next_upload = 0; set_slot(fake_bound, 0, 0); return; }
  size_t face = (size_t)((width + 7) & ~7) * height * 4;
  // A cube map is six faces in one allocation behind a single name, and it is
  // reachable through vglGetTexDataPointer just like a flat texture is. Getting
  // this right is what makes a cache that mistakes one for a 2D texture fail.
  if (target != GL_TEXTURE_2D)
    set_slot(fake_bound, face * 6, CUBE_CONTENT);
  else
    set_slot(fake_bound, face, fingerprint(data, (size_t)width * height * 4));
}

void glCompressedTexImage2D(GLenum target, GLint level, GLenum internalFormat, GLsizei width,
                            GLsizei height, GLint border, GLsizei imageSize, const GLvoid *data) {
  assert(fake_slot_alive[fake_bound]);
  if (fake_reject_next_upload) { fake_reject_next_upload = 0; set_slot(fake_bound, 0, 0); return; }
  if (target != GL_TEXTURE_2D) {
    set_slot(fake_bound, (size_t)imageSize * 6, CUBE_CONTENT);
    return;
  }
  if (level == 0)
    set_slot(fake_bound, imageSize, fingerprint(data, imageSize));
  else
    set_slot(fake_bound, fake_slot_bytes[fake_bound] + imageSize, fake_slot_content[fake_bound]);
}

// Non-NULL exactly when the driver holds data for the bound texture, which is
// how the cache tells an accepted upload from a rejected one. It has to be a
// real buffer of the slot's size now: the cache reads the texture back through
// this pointer when it evicts, and writes back through it when it restores,
// the way vitaGL hands out its own swizzled copy.
void *vglGetTexDataPointer(GLenum target) {
  if (!fake_slot_bytes[fake_bound])
    return NULL;
  if (!fake_slot_data[fake_bound]) {
    fake_slot_data[fake_bound] = malloc(fake_slot_bytes[fake_bound]);
    // Fill with the slot's content marker so a round trip through the cache is
    // checkable: what comes back must be what went in.
    if (fake_slot_data[fake_bound]) {
      memset(fake_slot_data[fake_bound], 0, fake_slot_bytes[fake_bound]);
      if (fake_slot_bytes[fake_bound] >= sizeof(uint32_t))
        *(uint32_t *)fake_slot_data[fake_bound] = fake_slot_content[fake_bound];
    }
  }
  return fake_slot_data[fake_bound];
}

// Mirrors the real one, including the trap: VGL_MEM_ALL is the enum terminator
// and asking for it reports no memory at all rather than the total.
size_t vglMemFree(vglMemType type) {
  if (type >= VGL_MEM_ALL)
    return 0;
  return (int)type < FAKE_POOLS ? fake_pool_free[type] : 0;
}
