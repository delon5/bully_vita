/* harness.h -- shared scaffolding for the texture cache tests.
 *
 * The cache is pulled in as source rather than linked, so a test can look at
 * the bookkeeping it would otherwise have no way to see.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __HARNESS_H__
#define __HARNESS_H__

#include "fake_vitagl.h"
#include "../loader/texture_cache.c"

#include <assert.h>
#include <stdio.h>
#include <unistd.h>

#define MB (1024u * 1024u)

static unsigned char source_bytes[4 * MB];

static void fill_source(uint32_t tag, size_t size) {
  for (size_t i = 0; i < size / 4; i++)
    ((uint32_t *)source_bytes)[i] = tag;
}

// One compressed upload, the shape a mobile game's world textures take.
static GLuint tex_upload(uint32_t tag, GLsizei width, GLsizei height, GLsizei size) {
  GLuint id;
  glGenTexturesHook(1, &id);
  glBindTextureHook(GL_TEXTURE_2D, id);
  fill_source(tag, size);
  glCompressedTexImage2DHook(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG, width, height,
                             0, size, source_bytes);
  return id;
}

// Re-upload over an existing name, the way a game re-streams a texture.
static void tex_reupload(GLuint id, uint32_t tag, GLsizei width, GLsizei height, GLsizei size) {
  glBindTextureHook(GL_TEXTURE_2D, id);
  fill_source(tag, size);
  glCompressedTexImage2DHook(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG, width, height,
                             0, size, source_bytes);
}

// An uncompressed upload whose format makes the loader's budget estimate and
// vitaGL's real allocation disagree: the estimate keys off the type first and
// falls through to four bytes a pixel for GL_RGBA4 with GL_UNSIGNED_BYTE, where
// the driver allocates two. Anything sized by the estimate reads and writes
// twice the buffer.
static GLuint tex_upload_narrow(uint32_t tag, GLsizei width, GLsizei height) {
  GLuint id;
  glGenTexturesHook(1, &id);
  glBindTextureHook(GL_TEXTURE_2D, id);
  fill_source(tag, (GLsizei)((size_t)width * height * 4));
  glTexImage2DHook(GL_TEXTURE_2D, 0, GL_RGBA4, width, height, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, source_bytes);
  return id;
}

unsigned fake_store_files(void);

// Cuts one stored texture short, as losing power mid-write does.
int fake_tear_one_store_file(long keep_bytes);

// Rewrites one 32-bit word of a stored texture's header.
int fake_scramble_store_word(int word_index);
const char *fake_last_scrambled_path(void);
extern size_t fake_heap_used;
unsigned fake_store_writes(void);

// Backups are written on the uploading thread now, so a record is on disk by
// the time the upload call returns and there is nothing to wait for. Kept so
// the tests still read as "upload, let it settle, then assert".
static void drain(void) {
}

static void frames(int count) {
  for (int i = 0; i < count; i++)
    texture_cache_tick();
}

// Walk around an area, drawing everything in it, for a number of frames.
static void wander(const GLuint *ids, int count, int frame_count) {
  for (int f = 0; f < frame_count; f++) {
    for (int i = 0; i < count; i++)
      glBindTextureHook(GL_TEXTURE_2D, ids[i]);
    texture_cache_tick();
  }
}

static void harness_start(size_t driver_memory) {
  fake_reset(driver_memory);
  texture_cache_init();
}

// The store outlives a run now, so a block that reasons about what is in it has
// to say where it is starting from rather than inherit whatever the block
// before it spilled.
static void harness_start_empty(size_t driver_memory) {
  purge_store();
  harness_start(driver_memory);
}

#endif
