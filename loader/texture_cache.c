/* texture_cache.c -- reloadable LRU texture cache
 *
 * The game was written for phones, where there is enough RAM to never let go of
 * a texture once it has been uploaded. On the Vita that exhausts vitaGL's memory
 * pools and kills the game after a long session.
 *
 * So we drop the textures the game has stopped drawing with. The game has no
 * idea we did and will never upload them again, which means dropping one has to
 * be reversible: every upload is copied to a cache file on ux0 by a background
 * writer thread, evicting frees the GPU allocation and remembers where the
 * source bytes went, and the next time the game binds the texture we read them
 * back and replay the original upload. Walking out of an area and back into it
 * costs a disk read rather than a wall of white.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <vitaGL.h>

#include <stdint.h>
#include <stdlib.h>
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
#define BACKUP_MAGIC 0x30435442

typedef struct {
  uint32_t magic;
  uint32_t id;
  uint32_t serial; // which generation of this texture name the record belongs to
  uint32_t level;
  uint32_t width;
  uint32_t height;
  uint32_t size; // payload bytes following this header
  uint32_t internalformat;
  uint32_t format;
  uint32_t type;
  uint32_t compressed;
  uint32_t checksum; // over the payload, so a short write cannot go unnoticed
} BackupRecord;

typedef struct {
  uint32_t size; // bytes we believe vitaGL has allocated for this texture
  uint32_t resident_size; // what size will be again once it is restored
  uint32_t last_frame; // frame counter at the time of the last bind

  // Everything needed to upload this texture again from scratch.
  uint32_t serial;
  uint32_t backup_offset; // where its records start in the cache file
  uint32_t backup_bytes; // how many bytes of records it has
  int32_t min_filter; // the game's last glTexParameteri, clobbered while evicted
  uint8_t levels;

  uint8_t tracked;
  uint8_t pinned; // render target or streamed-into texture, never evict
  uint8_t evicted;
  uint8_t unbacked; // no usable copy on disk, so evicting it would lose it
} TextureInfo;

typedef struct {
  GLuint id;
  uint32_t last_frame;
} EvictionCandidate;

// One queued write. The payload is a private copy, so the game is free to reuse
// or free its own buffer the moment the upload call returns.
typedef struct {
  BackupRecord record;
  uint32_t offset; // where in the cache file this record belongs
  void *payload;
} PendingWrite;

#define QUEUE_SLOTS 256 // must be a power of two
#define QUEUE_MASK (QUEUE_SLOTS - 1)

static TextureInfo textures[MAX_TEXTURES];
static GLuint bound_textures[MAX_TEXTURE_UNITS];
static int active_unit = 0;
static uint32_t frame_counter = 1;
static size_t tracked_bytes = 0;
static uint32_t upload_serial = 1;

static PendingWrite queue[QUEUE_SLOTS];
static volatile uint32_t queue_head; // producer: the render thread
static volatile uint32_t queue_tail; // consumer: the writer thread
static volatile uint32_t queued_bytes; // producer only, wraps harmlessly
static volatile uint32_t written_bytes; // consumer only, wraps harmlessly
static volatile uint32_t backup_flushed; // file offset the writer has committed

static SceUID backup_write_fd = -1;
static SceUID backup_read_fd = -1;
static SceUID backup_sema = -1;
static uint32_t backup_cursor; // next free offset in the cache file
static uint8_t *restore_scratch;
static int backup_ready;

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
  return backup_ready && !info->unbacked && info->levels > 0 &&
         info->backup_offset + info->backup_bytes <= __atomic_load_n(&backup_flushed, __ATOMIC_ACQUIRE);
}

static void texture_forget(GLuint id) {
  TextureInfo *info = texture_info(id);
  if (!info)
    return;
  if (info->tracked)
    tracked_bytes -= info->size;
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

static int backup_thread(SceSize args, void *argp) {
  for (;;) {
    if (sceKernelWaitSema(backup_sema, 1, NULL) < 0)
      return sceKernelExitDeleteThread(0);

    uint32_t tail = queue_tail;
    PendingWrite *write = &queue[tail & QUEUE_MASK];

    // Seek explicitly rather than trusting the file position: a short write must
    // not shift every record that follows it.
    sceIoLseek(backup_write_fd, write->offset, SCE_SEEK_SET);
    // The record header and its payload go out as one contiguous run, so the
    // reader can pull a whole texture back with a single read.
    sceIoWrite(backup_write_fd, &write->record, sizeof(BackupRecord));
    sceIoWrite(backup_write_fd, write->payload, write->record.size);

    // Advance past this record either way. If the write failed, the region will
    // not pass validation on restore and that texture falls back to a
    // placeholder, which is better than holding up every texture behind it.
    __atomic_store_n(&backup_flushed, write->offset + (uint32_t)sizeof(BackupRecord) + write->record.size,
                     __ATOMIC_RELEASE);

    free(write->payload);
    write->payload = NULL;
    __atomic_store_n(&written_bytes, written_bytes + write->record.size, __ATOMIC_RELAXED);
    __atomic_store_n(&queue_tail, tail + 1, __ATOMIC_RELEASE);
  }
}

void texture_cache_init(void) {
  for (GLuint id = 0; id < MAX_TEXTURES; id++)
    textures[id].min_filter = GL_LINEAR;

  // The cache lives next to the game's own files, in the data directory the user
  // installed them into. It is only meaningful for the run that wrote it --
  // texture names are handed out afresh every time -- so it starts empty, and
  // truncating is also what stops it growing across sessions.
  sceIoMkdir(DATA_PATH, 0777);
  backup_write_fd = sceIoOpen(TEXTURE_BACKUP_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (backup_write_fd < 0) {
    debugPrintf("texture cache: could not open %s, textures will not be reloadable\n", TEXTURE_BACKUP_PATH);
    return;
  }

  backup_read_fd = sceIoOpen(TEXTURE_BACKUP_PATH, SCE_O_RDONLY, 0777);
  restore_scratch = malloc(TEXTURE_BACKUP_MAX_KB * 1024);
  backup_sema = sceKernelCreateSema("bully_texcache", 0, 0, QUEUE_SLOTS, NULL);

  if (backup_read_fd < 0 || !restore_scratch || backup_sema < 0) {
    debugPrintf("texture cache: backing store unavailable, textures will not be reloadable\n");
    return;
  }

  SceUID thid = sceKernelCreateThread("bully_texcache", backup_thread, TEXTURE_BACKUP_THREAD_PRIORITY,
                                      64 * 1024, 0, TEXTURE_BACKUP_THREAD_AFFINITY, NULL);
  if (thid < 0 || sceKernelStartThread(thid, 0, NULL) < 0) {
    debugPrintf("texture cache: could not start writer thread\n");
    return;
  }

  backup_ready = 1;
  debugPrintf("texture cache: backing store at %s\n", TEXTURE_BACKUP_PATH);
}

void texture_cache_shutdown(void) {
  // Best effort: leaving a multi-hundred-megabyte file on the user's card after
  // they quit would be rude. If the removal does not take, the next boot
  // truncates it anyway.
  if (backup_ready)
    sceIoRemove(TEXTURE_BACKUP_PATH);
}

// Is there room in the queue, and in the memory budget we allow queued copies?
static int backup_has_room(uint32_t size) {
  uint32_t in_flight = queued_bytes - __atomic_load_n(&written_bytes, __ATOMIC_RELAXED);
  if (in_flight + size > (uint32_t)TEXTURE_BACKUP_STAGING_KB * 1024)
    return 0;
  if (queue_head - __atomic_load_n(&queue_tail, __ATOMIC_ACQUIRE) >= QUEUE_SLOTS)
    return 0;
  return 1;
}

// A texture with no copy on the card cannot be freed, so how hard we try to get
// one written depends on how badly we need to be able to free things.
static int backup_make_room(uint32_t size) {
  if (backup_has_room(size))
    return 1;

  const size_t budget = (size_t)TEXTURE_BUDGET_MB * 1024 * 1024;

  // Plenty of headroom: nothing is going to be evicted any time soon, so a
  // missed copy costs us nothing and is not worth stalling the render thread.
  if (tracked_bytes < budget - budget / 4)
    return 0;

  // Near or over the budget, waiting for the card is cheaper than ending up
  // with memory we are not allowed to reclaim.
  int wait_ms = tracked_bytes > budget ? TEXTURE_BACKUP_WAIT_MS_MAX : TEXTURE_BACKUP_WAIT_MS;
  for (int i = 0; i < wait_ms; i++) {
    sceKernelDelayThread(1000);
    if (backup_has_room(size))
      return 1;
  }
  return 0;
}

// Hands one upload to the writer thread. Returns 0 if it could not be queued,
// in which case the texture simply stays resident rather than being lost.
static int backup_stage(TextureInfo *info, GLuint id, GLint level, GLsizei width, GLsizei height,
                        uint32_t size, GLint internalformat, GLenum format, GLenum type,
                        int compressed, const void *data) {
  if (!backup_ready || !data || size == 0)
    return 0;
  // The whole texture, every level of it, has to fit the buffer we read it back
  // through -- checking only this level would overflow that buffer on a mipmap.
  uint32_t already = (level == 0) ? 0 : info->backup_bytes;
  if (already + sizeof(BackupRecord) + size > (uint32_t)TEXTURE_BACKUP_MAX_KB * 1024)
    return 0;
  if ((uint64_t)backup_cursor + sizeof(BackupRecord) + size > (uint64_t)TEXTURE_BACKUP_MAX_MB * 1024 * 1024)
    return 0; // the cache file has grown as far as we are willing to let it

  if (!backup_make_room(size))
    return 0;

  uint32_t head = queue_head;
  PendingWrite *write = &queue[head & QUEUE_MASK];
  write->payload = malloc(size);
  if (!write->payload)
    return 0;
  memcpy(write->payload, data, size);

  write->record.magic = BACKUP_MAGIC;
  write->record.id = id;
  write->record.serial = info->serial;
  write->record.level = level;
  write->record.width = width;
  write->record.height = height;
  write->record.size = size;
  write->record.internalformat = internalformat;
  write->record.format = format;
  write->record.type = type;
  write->record.compressed = compressed;
  write->record.checksum = checksum(write->payload, size);
  write->offset = backup_cursor;

  if (level == 0) {
    info->backup_offset = backup_cursor;
    info->backup_bytes = 0;
    info->levels = 0;
  }
  info->backup_bytes += sizeof(BackupRecord) + size;
  info->levels++;
  backup_cursor += sizeof(BackupRecord) + size;
  queued_bytes += size;

  __atomic_store_n(&queue_head, head + 1, __ATOMIC_RELEASE);
  sceKernelSignalSema(backup_sema, 1);
  return 1;
}

// Puts an evicted texture back exactly as the game uploaded it, by replaying the
// original calls. Returns 0 if the copy on disk turned out not to be usable, in
// which case the texture keeps its placeholder.
static int restore_texture(GLuint id) {
  TextureInfo *info = &textures[id];

  if (sceIoLseek(backup_read_fd, info->backup_offset, SCE_SEEK_SET) < 0)
    return 0;
  if (sceIoRead(backup_read_fd, restore_scratch, info->backup_bytes) != (int)info->backup_bytes)
    return 0;

  // Validate every record before touching GL: a half-written or superseded one
  // must leave the placeholder alone rather than upload garbage.
  uint32_t offset = 0;
  for (int i = 0; i < info->levels; i++) {
    if (offset + sizeof(BackupRecord) > info->backup_bytes)
      return 0;
    BackupRecord *record = (BackupRecord *)(restore_scratch + offset);
    if (record->magic != BACKUP_MAGIC || record->id != id || record->serial != info->serial ||
        record->level != (uint32_t)i)
      return 0;
    if (offset + sizeof(BackupRecord) + record->size > info->backup_bytes)
      return 0;
    if (checksum(restore_scratch + offset + sizeof(BackupRecord), record->size) != record->checksum)
      return 0;
    offset += sizeof(BackupRecord) + record->size;
  }

  glBindTexture(GL_TEXTURE_2D, id);

  offset = 0;
  for (int i = 0; i < info->levels; i++) {
    BackupRecord *record = (BackupRecord *)(restore_scratch + offset);
    const void *payload = restore_scratch + offset + sizeof(BackupRecord);
    if (record->compressed)
      glCompressedTexImage2D(GL_TEXTURE_2D, record->level, record->internalformat, record->width,
                             record->height, 0, record->size, payload);
    else
      glTexImage2D(GL_TEXTURE_2D, record->level, record->internalformat, record->width, record->height,
                   0, record->format, record->type, payload);
    offset += sizeof(BackupRecord) + record->size;
  }

  info->size = info->resident_size;
  tracked_bytes += info->resident_size;

  // Eviction forced the sampler off mipmaps because the placeholder had none.
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, info->min_filter);

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

// The number of bytes the game handed us for one uncompressed upload, which is
// what we have to keep a copy of (vitaGL's own allocation may differ).
static uint32_t source_size(GLsizei width, GLsizei height, GLenum format, GLenum type) {
  uint32_t bpp;

  switch (type) {
    case GL_UNSIGNED_SHORT_5_6_5:
    case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1:
      bpp = 2;
      break;
    default:
      switch (format) {
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

  return (uint32_t)width * (uint32_t)height * bpp;
}

/*
 * Eviction
 */

// Frees a texture's memory while keeping its name valid and the texture
// complete, so a draw call that still refers to it renders white rather than
// reading memory we have handed back. Deleting the name is not an option:
// vitaGL recycles names, so the game would end up drawing with somebody else's
// texture. The bytes live on in the cache file until the game binds it again.
static void evict_texture(GLuint id) {
  TextureInfo *info = &textures[id];

  glBindTexture(GL_TEXTURE_2D, id);
  // The placeholder has no mipmaps to sample from, so make sure the sampler does
  // not go looking for them. Restoring puts the game's own filter back.
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

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
// anything bound within the last min_idle_frames frames alone. When lossy is
// false only textures we can read back again are touched.
static void evict_textures(size_t target_bytes, uint32_t min_idle_frames, int lossy) {
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
    // A texture that is still bound to a unit can be drawn without the game
    // ever binding it again, so we would have no moment at which to put it back.
    if (is_bound(id))
      continue;
    if (!lossy && !is_restorable(info))
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

  debugPrintf("texture cache: evicted %d textures%s, %d KB still in use\n",
              evicted, lossy ? " (no copy on disk)" : "", (int)(tracked_bytes / 1024));
}

void texture_cache_tick(void) {
  frame_counter++;

  // A texture stays bound to its unit until something displaces it, so one the
  // game bound once and keeps drawing with is still in use even though we never
  // see another glBindTexture for it.
  for (int unit = 0; unit < MAX_TEXTURE_UNITS; unit++)
    texture_touch(bound_textures[unit]);

  const size_t budget = (size_t)TEXTURE_BUDGET_MB * 1024 * 1024;
  const size_t reserve = (size_t)TEXTURE_RESERVE_MB * 1024 * 1024;

  if (vglMemFree(VGL_MEM_ALL) < reserve) {
    // vitaGL is nearly out of memory, which is where the crash used to come
    // from. Free as much as we safely can and take textures that only just fell
    // out of use along with the rest.
    evict_textures(budget / 2, TEXTURE_IDLE_FRAMES_URGENT, 0);
    if (vglMemFree(VGL_MEM_ALL) < reserve)
      evict_textures(budget / 2, TEXTURE_IDLE_FRAMES_URGENT, 1);
  } else if (tracked_bytes > budget + budget / 2) {
    // We are a long way over budget, which means the card could not keep up and
    // we are holding textures we have no copy of. Memory has to be bounded
    // whatever the card is doing, so these go even though they will come back
    // white. Better a blemish than running out of memory.
    evict_textures(budget, TEXTURE_IDLE_FRAMES, 0);
    if (tracked_bytes > budget + budget / 2)
      evict_textures(budget, TEXTURE_IDLE_FRAMES, 1);
  } else if (tracked_bytes > budget) {
    // Steady state: trim back below the budget, but only touch textures the game
    // has left alone long enough that they cannot be part of what it is
    // currently drawing. If the working set genuinely does not fit we stay over
    // budget rather than evict textures we would immediately need again.
    evict_textures(budget - budget / 8, TEXTURE_IDLE_FRAMES, 0);
  }
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

  TextureInfo *info = texture_info(texture);
  if (info && info->evicted) {
    // The game is about to draw with a texture we dropped. Put it back.
    if (!is_restorable(info) || !restore_texture(texture))
      info->unbacked = 1; // nothing we can do; stop pretending it is reloadable
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

void glTexImage2DHook(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *data) {
  if (level != 0)
    return;

  glTexImage2D(target, level, internalformat, width, height, border, format, type, data);

  GLuint id = bound_textures[active_unit];
  TextureInfo *info = texture_info(id);
  if (!info)
    return;

  info->serial = ++upload_serial;
  info->unbacked = !backup_stage(info, id, level, width, height,
                                 source_size(width, height, format, type),
                                 internalformat, format, type, 0, data);
  texture_uploaded(id, uncompressed_size(width, height, internalformat, type), 1);
}

void glCompressedTexImage2DHook(GLenum target, GLint level, GLenum format, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data) {
  // mips for PVRTC textures break when they're under 1 block in size
  if (!(level == 0 || ((width >= 4 && height >= 4) || (format != 0x8C01 && format != 0x8C02))))
    return;

  glCompressedTexImage2D(target, level, format, width, height, border, imageSize, data);

  GLuint id = bound_textures[active_unit];
  TextureInfo *info = texture_info(id);
  if (!info)
    return;

  // A texture is only reloadable if every one of its levels made it to disk, so
  // one skipped level writes the whole thing off.
  if (level == 0) {
    info->serial = ++upload_serial;
    info->unbacked = !backup_stage(info, id, level, width, height, imageSize, format, format, 0, 1, data);
  } else if (!info->unbacked) {
    info->unbacked = !backup_stage(info, id, level, width, height, imageSize, format, format, 0, 1, data);
  }
  texture_uploaded(id, imageSize, level == 0);
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
