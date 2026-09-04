/* texture_cache.c -- LRU eviction for the game's textures
 *
 * The game was written for phones, where there is enough RAM to simply never
 * let go of a texture once it has been uploaded. On the Vita that behaviour
 * exhausts vitaGL's memory pools after a while and the game dies, so we keep
 * track of every texture the game uploads and drop the ones it has stopped
 * drawing with once we go over a budget that fits in the Vita's video memory.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <vitaGL.h>

#include <stdint.h>
#include <string.h>

#include "main.h"
#include "config.h"
#include "texture_cache.h"

// vitaGL hands out texture names as indices into a fixed slot table, so a flat
// array keyed by name is all the bookkeeping we need. Names outside the range
// are left untracked, which only costs us the ability to evict them.
#define MAX_TEXTURES 16384
#define MAX_TEXTURE_UNITS 16

typedef struct {
  uint32_t size; // bytes we believe vitaGL has allocated for this texture
  uint32_t last_frame; // frame counter at the time of the last bind
  uint8_t tracked;
  uint8_t pinned; // render target or streamed-into texture, never evict
  uint8_t evicted;
} TextureInfo;

typedef struct {
  GLuint id;
  uint32_t last_frame;
} EvictionCandidate;

static TextureInfo textures[MAX_TEXTURES];
static GLuint bound_textures[MAX_TEXTURE_UNITS];
static int active_unit = 0;
static uint32_t frame_counter = 1;
static size_t tracked_bytes = 0;

static TextureInfo *texture_info(GLuint id) {
  if (id == 0 || id >= MAX_TEXTURES)
    return NULL;
  return &textures[id];
}

static void texture_forget(GLuint id) {
  TextureInfo *info = texture_info(id);
  if (!info)
    return;
  if (info->tracked)
    tracked_bytes -= info->size;
  memset(info, 0, sizeof(TextureInfo));
}

static void texture_touch(GLuint id) {
  TextureInfo *info = texture_info(id);
  if (info && info->tracked)
    info->last_frame = frame_counter;
}

static void texture_pin(GLuint id) {
  TextureInfo *info = texture_info(id);
  if (info)
    info->pinned = 1;
}

// Records the memory an upload just cost us. The base level replaces whatever
// the texture held before, additional mipmap levels grow it.
static void texture_uploaded(GLuint id, uint32_t size, int base_level) {
  TextureInfo *info = texture_info(id);
  if (!info)
    return;

  if (base_level) {
    if (info->tracked)
      tracked_bytes -= info->size;
    info->size = size;
  } else {
    info->size += size;
  }

  info->tracked = 1;
  info->evicted = 0;
  info->last_frame = frame_counter;
  tracked_bytes += size;
}

// vitaGL rounds texture rows up to a multiple of 8 texels. Both this and the
// bytes per pixel below only have to be close enough to drive the budget, they
// are not meant to match vitaGL's allocation to the byte.
static uint32_t uncompressed_size(GLsizei width, GLsizei height, GLint internalformat, GLenum type) {
  uint32_t bpp;

  switch (type) {
    case GL_UNSIGNED_SHORT_5_6_5:
    case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1:
      bpp = 2;
      break;
    default:
      switch (internalformat) {
        case GL_RED:
        case GL_ALPHA:
        case GL_LUMINANCE:
          bpp = 1;
          break;
        case GL_RG:
        case GL_LUMINANCE_ALPHA:
          bpp = 2;
          break;
        case GL_RGB:
        case GL_BGR:
          bpp = 3;
          break;
        default:
          bpp = 4;
          break;
      }
      break;
  }

  return (uint32_t)((width + 7) & ~7) * (uint32_t)height * bpp;
}

// Replacing a texture with a 1x1 one frees vitaGL's allocation while keeping
// the name valid and the texture complete, so a draw call that still refers to
// it renders white instead of reading memory we have handed back. Deleting the
// name is not an option: vitaGL recycles names, so the game would end up
// drawing with somebody else's texture.
static void evict_texture(GLuint id) {
  TextureInfo *info = texture_info(id);

  glBindTexture(GL_TEXTURE_2D, id);
  // The placeholder has no mipmaps to sample from, so make sure the sampler
  // does not go looking for them. The game resets the filters whenever it
  // uploads a texture, so this only lasts as long as the placeholder does.
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

  tracked_bytes -= info->size;
  info->size = 0;
  info->evicted = 1;
}

// Evicts least recently used textures until we are under target_bytes, leaving
// anything bound within the last min_idle_frames frames alone.
static void evict_textures(size_t target_bytes, uint32_t min_idle_frames) {
  EvictionCandidate candidates[TEXTURE_EVICTIONS_PER_FRAME];
  int num_candidates = 0;

  // Collect the least recently used textures in a single pass, keeping the list
  // sorted oldest first so that the newest entry is the one that falls out when
  // it is full.
  for (GLuint id = 1; id < MAX_TEXTURES; id++) {
    TextureInfo *info = &textures[id];
    if (!info->tracked || info->pinned || info->evicted || info->size == 0)
      continue;
    if (frame_counter - info->last_frame < min_idle_frames)
      continue;

    int i;
    if (num_candidates < TEXTURE_EVICTIONS_PER_FRAME) {
      i = num_candidates++;
    } else {
      if (info->last_frame >= candidates[TEXTURE_EVICTIONS_PER_FRAME - 1].last_frame)
        continue;
      i = TEXTURE_EVICTIONS_PER_FRAME - 1;
    }

    for (; i > 0 && candidates[i - 1].last_frame > info->last_frame; i--)
      candidates[i] = candidates[i - 1];
    candidates[i].id = id;
    candidates[i].last_frame = info->last_frame;
  }

  if (num_candidates == 0)
    return;

  GLuint previous = bound_textures[active_unit];

  int evicted = 0;
  for (int i = 0; i < num_candidates && tracked_bytes > target_bytes; i++) {
    evict_texture(candidates[i].id);
    evicted++;
  }

  glBindTexture(GL_TEXTURE_2D, previous);

  debugPrintf("texture cache: evicted %d textures, %d KB still in use\n",
              evicted, (int)(tracked_bytes / 1024));
}

void texture_cache_tick(void) {
  frame_counter++;

  const size_t budget = (size_t)TEXTURE_BUDGET_MB * 1024 * 1024;

  if (vglMemFree(VGL_MEM_ALL) < (size_t)TEXTURE_RESERVE_MB * 1024 * 1024) {
    // vitaGL is nearly out of memory, which is where the crash used to come
    // from. Free as much as we safely can and take textures that only just fell
    // out of use along with the rest.
    evict_textures(budget / 2, TEXTURE_IDLE_FRAMES_URGENT);
  } else if (tracked_bytes > budget) {
    // Steady state: trim back below the budget, but only touch textures the
    // game has left alone long enough that they cannot be part of what it is
    // currently drawing. If the working set genuinely does not fit we stay over
    // budget rather than evict textures we would immediately need again.
    evict_textures(budget - budget / 8, TEXTURE_IDLE_FRAMES);
  }
}

void glActiveTextureHook(GLenum texture) {
  int unit = texture - GL_TEXTURE0;
  if (unit >= 0 && unit < MAX_TEXTURE_UNITS)
    active_unit = unit;
  glActiveTexture(texture);
}

void glBindTextureHook(GLenum target, GLuint texture) {
  bound_textures[active_unit] = texture;
  texture_touch(texture);
  glBindTexture(target, texture);
}

void glGenTexturesHook(GLsizei n, GLuint *res) {
  glGenTextures(n, res);
  // vitaGL reuses the names of deleted textures, so drop whatever we knew about
  // the previous occupant of each slot.
  for (GLsizei i = 0; i < n; i++)
    texture_forget(res[i]);
}

void glDeleteTexturesHook(GLsizei n, const GLuint *ids) {
  for (GLsizei i = 0; i < n; i++)
    texture_forget(ids[i]);
  glDeleteTextures(n, ids);
}

void glFramebufferTexture2DHook(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
  // The game renders into this one, its contents are not something we could
  // ever restore by reuploading.
  texture_pin(texture);
  glFramebufferTexture2D(target, attachment, textarget, texture, level);
}

void glTexImage2DHook(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *data) {
  if (level == 0) {
    glTexImage2D(target, level, internalformat, width, height, border, format, type, data);
    texture_uploaded(bound_textures[active_unit], uncompressed_size(width, height, internalformat, type), 1);
  }
}

void glCompressedTexImage2DHook(GLenum target, GLint level, GLenum format, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data) {
  // mips for PVRTC textures break when they're under 1 block in size
  if (level == 0 || ((width >= 4 && height >= 4) || (format != 0x8C01 && format != 0x8C02))) {
    glCompressedTexImage2D(target, level, format, width, height, border, imageSize, data);
    texture_uploaded(bound_textures[active_unit], imageSize, level == 0);
  }
}

void glTexSubImage2DHook(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels) {
  // A texture the game keeps writing into is one it is actively using, and its
  // contents do not come from a file we could have it load again.
  texture_pin(bound_textures[active_unit]);
  glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
}
