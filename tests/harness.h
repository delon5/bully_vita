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

unsigned fake_store_files(void);
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

#endif
