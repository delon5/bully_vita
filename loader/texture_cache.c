/* texture_cache.c -- reloadable LRU texture cache
 *
 * The game was written for phones, where there is enough RAM to never let go of
 * a texture once it has been uploaded. On the Vita that exhausts vitaGL's memory
 * pools and kills the game after a long session.
 *
 * So we drop the textures the game has stopped drawing with. The game has no
 * idea we did and will never upload them again, which means dropping one has to
 * be reversible: every upload is written to its own file under
 * ux0:data/Bully/textures, evicting frees the GPU allocation, and the next time
 * the game binds the texture we read the file back and replay the original
 * upload. Walking out of an area and back into it costs a disk read rather than
 * a wall of white.
 *
 * Each file is named by a key derived from the texture's own bytes rather than
 * by the name vitaGL handed out, so the store is still valid on the next run:
 * the second time the game loads a texture it is already there and nothing is
 * written.
 *
 * Replay works because vitaGL's upload path is a pure function of the arguments
 * plus the texture's own descriptor: PVRTC is memcpy'd into the GPU buffer
 * verbatim, the other compressed formats go through deterministic swizzlers, and
 * a mip chain is one contiguous buffer grown by a closed-form size formula. The
 * same calls in the same order therefore rebuild the same bytes.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <vitaGL.h>

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "config.h"
#include "texture_cache.h"

// vitaGL hands out texture names as indices into a fixed slot table, so a flat
// array keyed by name is all the bookkeeping we need. Names outside the range
// are left untracked, which only costs us the ability to evict them.
#define MAX_TEXTURES 16384
#define MAX_TEXTURE_UNITS 16

// Stamped into every record so a torn write, or a record left over from an
// upload the game has since replaced, can never be replayed into a texture.
#define BACKUP_MAGIC 0x31435442

typedef struct {
  uint32_t magic;
  uint32_t id;
  uint32_t serial; // which generation of this texture name the file belongs to
  uint32_t bytes; // texture data following this header
  uint32_t checksum;
} BackupRecord;

// What a level was uploaded as. Enough to hand the same call back to vitaGL
// when the texture is restored; the pixels themselves are not kept, they are
// taken out of GPU memory at the moment of eviction.
#define MAX_BACKUP_LEVELS 12
typedef struct {
  uint16_t width, height;
  uint32_t size;
} LevelInfo;

typedef struct {
  uint32_t size; // bytes we believe vitaGL has allocated for this texture
  uint32_t resident_size; // what size will be again once it is restored
  uint32_t last_frame; // frame counter at the time of the last bind

  uint32_t serial;
  uint32_t backup_bytes; // bytes of texture data written when it was evicted
  int32_t internalformat, format, type;
  LevelInfo level[MAX_BACKUP_LEVELS];
  int32_t min_filter; // the game's last glTexParameteri, clobbered while evicted
  uint8_t levels;

  uint8_t compressed_upload;
  uint8_t tracked;
  uint8_t pinned; // render target or streamed-into texture, never evict
  uint8_t evicted;
  uint8_t unbacked; // no usable copy on disk, so evicting it would lose it
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
static uint32_t upload_serial = 1;
static uint32_t starved_frames; // frames the lossless pass has failed to keep up
static size_t pool_total; // vitaGL's free memory once it was up, sampled on the first tick
// Counted for the trace: this is the first time any of this has run on hardware.
static uint32_t evicted_count, restored_count, restore_failed_count;


static uint8_t *restore_scratch;
static int backup_ready;
static int cache_enabled = 1;


// vglMemFree refuses VGL_MEM_ALL: it is the enum terminator and the wrapper
// returns 0 for anything greater than or equal to it, so asking for "all" would
// silently report no memory at all and make us think we were always in trouble.
// Ask for each pool instead. The names have changed between vitaGL versions but
// the order has not: 0 is CDRAM, 1 is RAM, 2 is phycont, and 3 is either a
// fourth pool or the newlib entry, which always reports 0.
static size_t vitagl_free_memory(void) {
  size_t total = 0;
  for (int pool = 0; pool < 4; pool++)
    total += vglMemFree((vglMemType)pool);
  return total;
}

// vitaGL reports a rejected upload by quietly not allocating anything, and it
// frees the old data before it tries, so asking what the texture ended up with
// is both the validity check and the out-of-memory check. Anything else would
// mean predicting vitaGL's format and size rules from out here.
static int texture_is_allocated(void) {
  return vglGetTexDataPointer(GL_TEXTURE_2D) != NULL;
}

static uint32_t checksum(const void *data, uint32_t size) {
  // FNV-1a. We are guarding against a truncated or stale record, not against a
  // malicious one, and this is cheap enough to run on every upload.
  const uint8_t *bytes = data;
  uint32_t hash = 0x811c9dc5;
  for (uint32_t i = 0; i < size; i++) {
    hash ^= bytes[i];
    hash *= 0x01000193;
  }
  return hash;
}

static TextureInfo *texture_info(GLuint id) {
  if (id == 0 || id >= MAX_TEXTURES)
    return NULL;
  return &textures[id];
}

// A texture can only be evicted if we can put it back, which means all of its
// records have actually reached the card.
static int is_restorable(const TextureInfo *info) {
  return backup_ready && !info->unbacked && info->levels > 0;
}

/*
 * Cache file allocator
 */

// One file per evicted texture, named by the texture and which generation of
// it this is. Spread over 256 subdirectories, since a single directory holding
// thousands of entries is slow to open on a memory card.
//
// This used to be keyed by a hash of the texture's contents so the store could
// be reused on a later run. That was worth doing when every upload was written;
// now that only evicted textures are, there is little to reuse and the hash
// cost a directory lookup per upload -- which is what made a second run no
// faster than the first.
static void texture_path(char *out, size_t out_size, GLuint id, uint32_t serial) {
  snprintf(out, out_size, "%s/%02x/%u_%u.tex", TEXTURE_CACHE_DIR, (unsigned)(id & 0xff), id, serial);
}

static void texture_dir(char *out, size_t out_size, GLuint id) {
  snprintf(out, out_size, "%s/%02x", TEXTURE_CACHE_DIR, (unsigned)(id & 0xff));
}

static void backup_release(TextureInfo *info) {
  // The file stays. It is keyed by content, so it is still valid for whatever
  // uploads those exact bytes next -- this run or a later one.
  info->levels = 0;
  info->backup_bytes = 0;
}

static void texture_forget(GLuint id) {
  TextureInfo *info = texture_info(id);
  if (!info)
    return;
  if (info->tracked)
    tracked_bytes -= info->size;
  backup_release(info);
  memset(info, 0, sizeof(TextureInfo));
  info->min_filter = GL_LINEAR;
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

/*
 * Backing store
 */


void texture_cache_init(void) {
  SceIoStat stat;
  cache_enabled = sceIoGetstat(TEXTURE_CACHE_DISABLE_PATH, &stat) < 0;
  if (!cache_enabled) {
    traceLog("texture cache: disabled by %s\n", TEXTURE_CACHE_DISABLE_PATH);
    return;
  }

  for (GLuint id = 0; id < MAX_TEXTURES; id++)
    textures[id].min_filter = GL_LINEAR;

  // The store lives next to the game's own files, in the data directory the
  // user installed them into, and is kept across runs. Check there is room for
  // it before promising anything reloadable.
  uint64_t card_size = 0, card_free = 0;
  if (sceAppMgrGetDevInfo("ux0:", &card_size, &card_free) < 0) {
    traceLog("texture cache: could not read free space on ux0, running without a backing store\n");
    return;
  }
  uint64_t reserve = (uint64_t)TEXTURE_BACKUP_KEEP_FREE_MB * 1024 * 1024;
  uint64_t usable = card_free > reserve ? card_free - reserve : 0;
  if (usable > (uint64_t)TEXTURE_BACKUP_MAX_MB * 1024 * 1024)
    usable = (uint64_t)TEXTURE_BACKUP_MAX_MB * 1024 * 1024;
  if (usable < (uint64_t)TEXTURE_BACKUP_MIN_MB * 1024 * 1024) {
    traceLog("texture cache: only %d MB free on ux0, running without a backing store\n",
             (int)(card_free / (1024 * 1024)));
    return;
  }
  sceIoMkdir(DATA_PATH, 0777);
  sceIoMkdir(TEXTURE_CACHE_DIR, 0777);

  restore_scratch = malloc((size_t)TEXTURE_BACKUP_MAX_KB * 1024);
  if (!restore_scratch) {
    debugPrintf("texture cache: no scratch buffer, textures will not be reloadable\n");
    return;
  }

  backup_ready = 1;
  traceLog("texture cache: store at %s (%d MB free on ux0)\n",
           TEXTURE_CACHE_DIR, (int)(card_free / (1024 * 1024)));
}

void texture_cache_shutdown(void) {
  if (!backup_ready)
    return;
  // The store is deliberately left behind: it is keyed by texture contents, so
  // the next run finds its textures already written and starts faster. Deleting
  // ux0:data/Bully/textures is always safe.
  backup_ready = 0;
  free(restore_scratch);
  restore_scratch = NULL;
}


// A texture with no copy on the card cannot be freed, so how hard we try to get
// one written depends on how badly we need to be able to free things.

// Writes one upload's record to the cache file. Returns 0 if it could not be, in
// which case the texture simply stays resident rather than being lost.
// Called for every upload. Records what the call was, and nothing else: no
// file is created, no directory is touched, no bytes are copied. The pixels
// are still in GPU memory and stay there until the texture is actually
// evicted, which is the only moment a copy is worth making.
static int backup_stage(TextureInfo *info, GLuint id, GLint level, GLsizei width, GLsizei height,
                        uint32_t size, GLint internalformat, GLenum format, GLenum type,
                        int compressed, const void *data) {
  if (!backup_ready || !data || size == 0)
    return 0;
  if (level >= MAX_BACKUP_LEVELS)
    return 0;

  if (level == 0) {
    info->levels = 0;
    info->backup_bytes = 0;
    info->internalformat = internalformat;
    info->format = format;
    info->type = type;
    info->compressed_upload = (uint8_t)compressed;
  } else if (info->levels != (uint8_t)level) {
    // Levels have to arrive in order and exactly once: restoring walks them
    // from zero, and a gap or a repeat would rebuild a different texture.
    return 0;
  }

  info->level[level].width = (uint16_t)width;
  info->level[level].height = (uint16_t)height;
  info->level[level].size = size;
  info->levels++;
  return 1;
}

// Takes the texture's pixels out of GPU memory and writes them to the card,
// just before the memory holding them is handed back. vitaGL keeps a texture
// in its own swizzled layout, so what is written is the GPU's copy rather than
// the bytes the game originally uploaded, and restoring puts it back the same
// way round.
static int backup_capture(TextureInfo *info, GLuint id) {
  if (!backup_ready || info->levels == 0 || info->resident_size == 0)
    return 0;
  if (info->resident_size > (uint32_t)TEXTURE_BACKUP_MAX_KB * 1024)
    return 0;
  if (info->resident_size < TEXTURE_BACKUP_MIN_BYTES)
    return 0; // too small to be worth a file; it simply stays resident

  glBindTexture(GL_TEXTURE_2D, id);
  const void *pixels = vglGetTexDataPointer(GL_TEXTURE_2D);
  if (!pixels)
    return 0;

  char dir[160], path[160];
  texture_dir(dir, sizeof(dir), id);
  sceIoMkdir(dir, 0777);
  texture_path(path, sizeof(path), id, info->serial);

  BackupRecord record;
  record.magic = BACKUP_MAGIC;
  record.id = id;
  record.serial = info->serial;
  record.bytes = info->resident_size;
  record.checksum = checksum(pixels, info->resident_size);

  SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd < 0)
    return 0;
  int ok = sceIoWrite(fd, &record, sizeof(record)) == (int)sizeof(record) &&
           sceIoWrite(fd, pixels, info->resident_size) == (int)info->resident_size;
  sceIoClose(fd);
  if (!ok) {
    sceIoRemove(path); // a short write must not be mistaken for a usable copy
    return 0;
  }
  info->backup_bytes = info->resident_size;
  return 1;
}

// Puts an evicted texture back exactly as the game uploaded it, by replaying the
// original calls. Returns 0 if the copy turned out not to be usable, having left
// a valid placeholder behind.
static int restore_texture(GLuint id);

// Frees a texture's memory while keeping its name valid and the texture
// complete, so a draw call that still refers to it renders white rather than
// reading memory we have handed back. Deleting the name is not an option:
// vitaGL recycles names, so the game would end up drawing with somebody else's
// texture. The bytes live on in the cache file until the game binds it again.
static void install_placeholder(GLuint id) {
  glBindTexture(GL_TEXTURE_2D, id);
  // The placeholder has no mipmaps to sample from, so make sure the sampler does
  // not go looking for them. Restoring puts the game's own filter back.
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
}

static int restore_texture(GLuint id) {
  TextureInfo *info = &textures[id];

  char path[160];
  texture_path(path, sizeof(path), id, info->serial);
  SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0)
    return 0;
  BackupRecord record;
  int header = sceIoRead(fd, &record, sizeof(record));
  int got = 0;
  if (header == (int)sizeof(record) && record.magic == BACKUP_MAGIC && record.id == id &&
      record.serial == info->serial && record.bytes == info->backup_bytes &&
      record.bytes <= (uint32_t)TEXTURE_BACKUP_MAX_KB * 1024)
    got = sceIoRead(fd, restore_scratch, record.bytes);
  sceIoClose(fd);
  if (got != (int)record.bytes)
    return 0;
  if (checksum(restore_scratch, record.bytes) != record.checksum)
    return 0;

  // Allocate the texture again through the same call the game made, with no
  // pixels, then put the GPU's own copy back into the buffer vitaGL hands out.
  // Uploading the saved bytes as if they were the game's would send them
  // through the swizzler a second time and scramble the texture.
  // Replay every level the game uploaded, with no pixels. Allocating only the
  // base level would leave a buffer smaller than the mip chain that was
  // captured, and the copy below would run off the end of it.
  glBindTexture(GL_TEXTURE_2D, id);
  for (int i = 0; i < info->levels; i++) {
    if (info->compressed_upload)
      glCompressedTexImage2D(GL_TEXTURE_2D, i, info->internalformat, info->level[i].width,
                             info->level[i].height, 0, info->level[i].size, NULL);
    else
      glTexImage2D(GL_TEXTURE_2D, i, info->internalformat, info->level[i].width,
                   info->level[i].height, 0, info->format, info->type, NULL);
  }

  void *pixels = vglGetTexDataPointer(GL_TEXTURE_2D);
  if (!pixels) {
    install_placeholder(id);
    return 0;
  }
  memcpy(pixels, restore_scratch, record.bytes);

  // Eviction forced the sampler off mipmaps because the placeholder had none.
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, info->min_filter);

  info->size = info->resident_size;
  tracked_bytes += info->resident_size;
  info->evicted = 0;
  info->last_frame = frame_counter;
  return 1;
}

/*
 * Accounting
 */

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
  info->resident_size = info->size;
}

// vitaGL rounds texture rows up to a multiple of 8 texels. Both this and the
// bytes per pixel below only have to be close enough to drive the budget, they
// are not meant to match vitaGL's allocation to the byte.
static uint32_t bytes_per_pixel(GLint internalformat, GLenum type) {
  switch (type) {
    case GL_UNSIGNED_SHORT_5_6_5:
    case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1:
      return 2;
    default:
      break;
  }
  switch (internalformat) {
    case GL_RED:
    case GL_ALPHA:
    case GL_LUMINANCE:
      return 1;
    case GL_RG:
    case GL_LUMINANCE_ALPHA:
      return 2;
    case GL_RGB:
    case GL_BGR:
      return 3;
    default:
      return 4;
  }
}

/*
 * Eviction
 */

static void evict_texture(GLuint id) {
  evicted_count++;
  TextureInfo *info = &textures[id];

  // Take the copy first, because install_placeholder is what releases the
  // memory the pixels are sitting in. If it cannot be taken -- too small to be
  // worth a file, no room on the card, a write that failed -- the texture stays
  // exactly where it is. Dropping one we cannot put back is how the ground and
  // the buildings went black, and there is no pressure that makes it worth it.
  if (!backup_capture(info, id)) {
    info->unbacked = 1;
    evicted_count--;
    return;
  }

  install_placeholder(id);

  tracked_bytes -= info->size;
  info->size = 0;
  info->evicted = 1;
}

static int is_bound(GLuint id) {
  for (int unit = 0; unit < MAX_TEXTURE_UNITS; unit++)
    if (bound_textures[unit] == id)
      return 1;
  return 0;
}

// Evicts least recently used textures until we are under target_bytes, leaving
// anything used within the last min_idle_frames frames alone, and only ones we
// can read back again. Returns how many candidates it found, so the caller can
// tell "nothing to evict" from "hit the per-frame cap".
static int evict_textures(size_t target_bytes, uint32_t min_idle_frames) {
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
    // A texture that is still bound to a unit can be drawn without the game ever
    // binding it again, so we would have no moment at which to put it back.
    if (is_bound(id))
      continue;
    if (!is_restorable(info))
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
    return 0;

  GLuint previous = bound_textures[active_unit];

  int evicted = 0;
  for (int i = 0; i < num_candidates && tracked_bytes > target_bytes; i++) {
    evict_texture(candidates[i].id);
    evicted++;
  }

  glBindTexture(GL_TEXTURE_2D, previous);

  debugPrintf("texture cache: evicted %d textures, %d KB still in use\n",
              evicted, (int)(tracked_bytes / 1024));
  return num_candidates;
}

void texture_cache_tick(void) {
  if (!cache_enabled)
    return;

  frame_counter++;

  // A texture stays bound to its unit until something displaces it, so one the
  // game bound once and keeps drawing with is still in use even though we never
  // see another glBindTexture for it.
  for (int unit = 0; unit < MAX_TEXTURE_UNITS; unit++)
    texture_touch(bound_textures[unit]);

  const size_t budget = (size_t)TEXTURE_BUDGET_MB * 1024 * 1024;

  // How much vitaGL had to give out once it was up and the game had started
  // drawing. Sampled here rather than at init, which runs before vglInit.
  size_t free_now = vitagl_free_memory();
  if (!pool_total)
    pool_total = free_now;
  size_t headroom = pool_total / 100 * TEXTURE_FREE_HEADROOM_PERCENT;

  if (tracked_bytes <= budget && free_now >= headroom) {
    starved_frames = 0;
    return;
  }

  // Trim back, but only touch textures the game has left alone long enough that
  // they cannot be part of what it is currently drawing. Aim under both limits:
  // the byte budget for a scene that simply hoards, and the headroom for one
  // whose pressure comes from everything else sharing these pools.
  size_t target = tracked_bytes;
  if (tracked_bytes > budget)
    target = budget - budget / 8;
  if (free_now < headroom) {
    size_t needed = headroom - free_now;
    target = target > needed ? target - needed : 0;
  }
  evict_textures(target, TEXTURE_IDLE_FRAMES);

  if (tracked_bytes <= budget && vitagl_free_memory() >= headroom) {
    starved_frames = 0;
    return;
  }

  // Still over budget. Stay over it.
  //
  // There used to be a last resort here: once the lossless pass had failed for
  // long enough, evict textures with no copy on the card and accept losing
  // them. The reasoning was that bounded memory beats running out of it. On
  // hardware that is not the trade it looks like -- it took the ground and the
  // buildings out of the world and they never came back, because nothing was
  // ever going to upload them again. A texture this cache cannot restore is a
  // texture it has no business dropping, which is the whole premise of the
  // thing.
  //
  // So the only textures that ever leave are the ones that can come back. If
  // that is not enough to reach the budget, the budget is not reached.
  starved_frames++;
}

/*
 * GL entry points
 */

void glActiveTextureHook(GLenum texture) {
  int unit = texture - GL_TEXTURE0;
  if (unit >= 0 && unit < MAX_TEXTURE_UNITS)
    active_unit = unit;
  glActiveTexture(texture);
}

void glBindTextureHook(GLenum target, GLuint texture) {
  bound_textures[active_unit] = texture;

  TextureInfo *info = cache_enabled ? texture_info(texture) : NULL;
  if (info && info->evicted && target == GL_TEXTURE_2D) {
    // The game is about to draw with a texture we dropped. Put it back.
    if (!is_restorable(info) || !restore_texture(texture)) {
      info->unbacked = 1; // nothing we can do; stop pretending it is reloadable
      restore_failed_count++;
    } else {
      restored_count++;
    }
  }

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
  // The game renders into this one, so its contents are not something we could
  // ever get back off the card.
  texture_pin(texture);
  glFramebufferTexture2D(target, attachment, textarget, texture, level);
}

// Shared tail of both upload hooks: work out whether vitaGL accepted the upload
// and, if so, take a copy and account for it.
static void upload_finished(GLenum target, GLint level, GLsizei width, GLsizei height,
                            uint32_t source_bytes, uint32_t resident_bytes, GLint internalformat,
                            GLenum format, GLenum type, int compressed, const void *data) {
  if (!cache_enabled)
    return;

  // Cube map faces go through the same entry points but are six buffers behind
  // one name. Tracking one as if it were a flat texture would let us free all
  // six and "restore" a single face as a 2D texture, so leave them alone.
  if (target != GL_TEXTURE_2D)
    return;

  GLuint id = bound_textures[active_unit];
  TextureInfo *info = texture_info(id);
  if (!info)
    return;

  // vitaGL rejects uploads we cannot easily predict -- non power of two
  // compressed dimensions, anything over its maximum size, a format it does not
  // know -- by returning without allocating. Counting those would leave phantom
  // bytes in the budget and let us "evict" a texture that was never there.
  if (!texture_is_allocated()) {
    if (level == 0)
      texture_forget(id);
    return;
  }

  if (level == 0)
    info->serial = ++upload_serial;

  if (level == 0 || !info->unbacked)
    info->unbacked = !backup_stage(info, id, level, width, height, source_bytes, internalformat,
                                   format, type, compressed, data);
  if (info->unbacked)
    backup_release(info);

  texture_uploaded(id, resident_bytes, level == 0);
}

#ifdef LOADER_TRACE
extern int trace_textures;
#endif

void glTexImage2DHook(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *data) {
#ifdef LOADER_TRACE
  trace_textures++;
#endif
  // Levels above the base are dropped: the game supplies mipmaps the Vita does
  // not need and vitaGL would rebase them against whichever level arrived first.
  if (level != 0)
    return;

  glTexImage2D(target, level, internalformat, width, height, border, format, type, data);

  uint32_t bpp = bytes_per_pixel(internalformat, type);
  upload_finished(target, level, width, height,
                  (uint32_t)width * (uint32_t)height * bytes_per_pixel(format, type),
                  (uint32_t)((width + 7) & ~7) * (uint32_t)height * bpp,
                  internalformat, format, type, 0, data);
}

void glCompressedTexImage2DHook(GLenum target, GLint level, GLenum format, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data) {
#ifdef LOADER_TRACE
  trace_textures++;
#endif
  // mips for PVRTC textures break when they're under 1 block in size
  if (!(level == 0 || ((width >= 4 && height >= 4) || (format != 0x8C01 && format != 0x8C02))))
    return;

  glCompressedTexImage2D(target, level, format, width, height, border, imageSize, data);

  upload_finished(target, level, width, height, imageSize, imageSize, format, format, 0, 1, data);
}

void glTexSubImage2DHook(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels) {
  // A texture the game keeps writing into is one it is actively using, and the
  // copy we hold no longer matches what it should look like.
  texture_pin(bound_textures[active_unit]);
  glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
}

void glTexParameteriHook(GLenum target, GLenum pname, GLint param) {
  if (pname == GL_TEXTURE_MIN_FILTER) {
    TextureInfo *info = texture_info(bound_textures[active_unit]);
    if (info)
      info->min_filter = param;
  }
  glTexParameteri(target, pname, param);
}

void glTexParameterfHook(GLenum target, GLenum pname, GLfloat param) {
  if (pname == GL_TEXTURE_MIN_FILTER) {
    TextureInfo *info = texture_info(bound_textures[active_unit]);
    if (info)
      info->min_filter = (GLint)param;
  }
  glTexParameterf(target, pname, param);
}

// Reported on the trace heartbeat while this is being exercised for the first
// time on hardware: how much is resident, how much has been dropped, and how
// much of it came back.
void texture_cache_stats(int *mb, int *evicted, int *restored, int *failed) {
  *mb = (int)(tracked_bytes / (1024 * 1024));
  *evicted = (int)evicted_count;
  *restored = (int)restored_count;
  *failed = (int)restore_failed_count;
}
