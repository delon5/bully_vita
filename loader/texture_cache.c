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

#define QUEUE_SLOTS 64 // must be a power of two
#define QUEUE_MASK (QUEUE_SLOTS - 1)
// Staged payload buffers up to this size are kept and reused by their queue
// slot. Anything larger is returned to the heap rather than held indefinitely.
#define PAYLOAD_KEEP_BYTES (64 * 1024)

// The cache file is carved into extents whose sizes are these buckets, so that a
// texture the game uploads again can go back into the space its previous
// generation occupied instead of appending for ever.
#define NUM_BUCKETS 7
#define SMALLEST_BUCKET (64 * 1024)
#define FREE_LIST_CAP 512

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

  uint32_t serial;
  uint32_t backup_offset; // where its records live in the cache file
  uint32_t backup_bytes; // how many bytes of records it has
  uint32_t backup_seq; // queue position its last record was staged at
  int32_t min_filter; // the game's last glTexParameteri, clobbered while evicted
  uint8_t backup_bucket;
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
  uint32_t capacity; // how big payload actually is, so slots can reuse it
} PendingWrite;

static TextureInfo textures[MAX_TEXTURES];
static GLuint bound_textures[MAX_TEXTURE_UNITS];
static int active_unit = 0;
static uint32_t frame_counter = 1;
static size_t tracked_bytes = 0;
static uint32_t upload_serial = 1;
static uint32_t starved_frames; // frames the lossless pass has failed to keep up
// Counted for the trace: this is the first time any of this has run on hardware.
static uint32_t evicted_count, restored_count, restore_failed_count;

static PendingWrite queue[QUEUE_SLOTS];
static volatile uint32_t queue_head; // producer: the render thread
static volatile uint32_t queue_tail; // consumer: the writer thread
static volatile uint32_t queued_bytes; // producer only, wraps harmlessly
static volatile uint32_t written_bytes; // consumer only, wraps harmlessly

static SceUID backup_fd = -1;
static SceUID backup_sema = -1;
static uint8_t *restore_scratch;
static int backup_ready;
static int cache_enabled = 1;

// Cache file allocator.
static uint32_t file_cursor;
static uint32_t file_limit; // how far the cache file may grow, set from free space
static uint32_t free_list[NUM_BUCKETS][FREE_LIST_CAP];
static uint16_t free_count[NUM_BUCKETS];
// Every record a texture can own must be readable back in one go, so a texture's
// extent is capped by the buffer we read it through.
static uint32_t bucket_bytes(int bucket) {
  return (uint32_t)SMALLEST_BUCKET << bucket;
}

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
  if (!backup_ready || info->unbacked || info->levels == 0)
    return 0;
  // Anything still sitting in the queue has not reached the card yet, and
  // dropping a texture whose copy is not written is how you lose it. Records go
  // out in the order they were staged, so a texture is safe once the writer has
  // consumed past the position its last one took.
  return (int32_t)(__atomic_load_n(&queue_tail, __ATOMIC_ACQUIRE) - info->backup_seq) >= 0;
}

/*
 * Cache file allocator
 */

static int bucket_for(uint32_t size) {
  for (int bucket = 0; bucket < NUM_BUCKETS; bucket++)
    if (size <= bucket_bytes(bucket))
      return bucket;
  return -1;
}

static int extent_alloc(uint32_t size, uint32_t *offset) {
  int bucket = bucket_for(size);
  if (bucket < 0)
    return -1;

  if (free_count[bucket] > 0) {
    *offset = free_list[bucket][--free_count[bucket]];
    return bucket;
  }
  if ((uint64_t)file_cursor + bucket_bytes(bucket) > (uint64_t)file_limit)
    return -1;
  *offset = file_cursor;
  file_cursor += bucket_bytes(bucket);
  return bucket;
}

static void extent_free(uint32_t offset, int bucket) {
  if (free_count[bucket] < FREE_LIST_CAP)
    free_list[bucket][free_count[bucket]++] = offset;
}

// Gives a texture an extent big enough for the mipmap levels that will follow
// the base one. A compressed mip chain is at most four thirds of its base level,
// so we ask for half again and never have to move the extent mid-upload.
static int extent_claim(TextureInfo *info, uint32_t base_bytes) {
  uint32_t wanted = base_bytes + base_bytes / 2;
  int bucket = bucket_for(wanted);
  if (bucket < 0)
    return 0;

  if (info->levels > 0 && info->backup_bucket == bucket)
    return 1; // its previous generation's extent is the right size, reuse it

  if (info->levels > 0)
    extent_free(info->backup_offset, info->backup_bucket);

  uint32_t offset;
  int got = extent_alloc(wanted, &offset);
  if (got < 0) {
    info->levels = 0;
    return 0;
  }
  info->backup_offset = offset;
  info->backup_bucket = got;
  return 1;
}

static void backup_release(TextureInfo *info) {
  if (info->levels > 0)
    extent_free(info->backup_offset, info->backup_bucket);
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

static int backup_thread(SceSize args, void *argp) {
  for (;;) {
    if (sceKernelWaitSema(backup_sema, 1, NULL) < 0)
      return sceKernelExitDeleteThread(0);

    uint32_t tail = queue_tail;
    PendingWrite *write = &queue[tail & QUEUE_MASK];

    // Seek explicitly rather than trusting the file position: extents are
    // reused out of order, and a short write must not shift what follows it.
    sceIoLseek(backup_fd, write->offset, SCE_SEEK_SET);
    sceIoWrite(backup_fd, &write->record, sizeof(BackupRecord));
    sceIoWrite(backup_fd, write->payload, write->record.size);

    // A failed or partial write leaves a region that will not pass validation on
    // restore, and that texture falls back to a placeholder. That is better than
    // holding up every texture queued behind it.
    if (write->capacity > PAYLOAD_KEEP_BYTES) {
      free(write->payload);
      write->payload = NULL;
      write->capacity = 0;
    }
    __atomic_store_n(&written_bytes, written_bytes + write->record.size, __ATOMIC_RELAXED);
    __atomic_store_n(&queue_tail, tail + 1, __ATOMIC_RELEASE);
  }
}

void texture_cache_init(void) {
  SceIoStat stat;
  cache_enabled = sceIoGetstat(TEXTURE_CACHE_DISABLE_PATH, &stat) < 0;
  if (!cache_enabled) {
    traceLog("texture cache: disabled by %s\n", TEXTURE_CACHE_DISABLE_PATH);
    return;
  }

  for (GLuint id = 0; id < MAX_TEXTURES; id++)
    textures[id].min_filter = GL_LINEAR;

  // The cache lives next to the game's own files, in the data directory the user
  // installed them into. It is only meaningful for the run that wrote it --
  // texture names are handed out afresh every time -- so it starts empty, and
  // truncating is also what stops it growing across sessions.
  // Work out how much of the card we may actually use. The cap is a ceiling,
  // not an entitlement: whatever is free minus a reserve wins if it is smaller.
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
  file_limit = (uint32_t)usable;

  sceIoMkdir(DATA_PATH, 0777);
  // One descriptor, read/write: the writer seeks and writes, the render thread
  // seeks and reads, and both are on the render thread's side of a handshake
  // that never lets them touch the same record at the same time.
  backup_fd = sceIoOpen(TEXTURE_BACKUP_PATH, SCE_O_RDWR | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (backup_fd < 0) {
    debugPrintf("texture cache: could not open %s, textures will not be reloadable\n", TEXTURE_BACKUP_PATH);
    return;
  }

  restore_scratch = malloc(bucket_bytes(NUM_BUCKETS - 1));
  backup_sema = sceKernelCreateSema("bully_texcache", 0, 0, QUEUE_SLOTS, NULL);
  SceUID thid = -1;
  if (restore_scratch && backup_sema >= 0)
    thid = sceKernelCreateThread("bully_texcache", backup_thread, TEXTURE_BACKUP_THREAD_PRIORITY,
                                 64 * 1024, 0, TEXTURE_BACKUP_THREAD_AFFINITY, NULL);

  if (thid < 0 || sceKernelStartThread(thid, 0, NULL) < 0) {
    debugPrintf("texture cache: backing store unavailable, textures will not be reloadable\n");
    free(restore_scratch);
    restore_scratch = NULL;
    if (backup_sema >= 0)
      sceKernelDeleteSema(backup_sema);
    sceIoClose(backup_fd);
    backup_fd = -1;
    return;
  }

  backup_ready = 1;
  traceLog("texture cache: backing store at %s, limit %d MB (%d MB free on ux0)\n",
           TEXTURE_BACKUP_PATH, (int)(file_limit / (1024 * 1024)), (int)(card_free / (1024 * 1024)));
}

void texture_cache_shutdown(void) {
  if (!backup_ready)
    return;

  // Stop the writer before touching the file: removing a path with a write in
  // flight is not something we should ask the filesystem to make sense of.
  backup_ready = 0;
  sceKernelDeleteSema(backup_sema);
  backup_sema = -1;
  sceIoClose(backup_fd);
  backup_fd = -1;
  // Leaving a multi-hundred-megabyte file on the user's card after they quit
  // would be rude. If it does not take, the next boot truncates it anyway.
  sceIoRemove(TEXTURE_BACKUP_PATH);
}

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

  // Near or over the budget, waiting for the card is cheaper than ending up with
  // memory we are not allowed to reclaim.
  int wait_ms = tracked_bytes > budget ? TEXTURE_BACKUP_WAIT_MS_MAX : TEXTURE_BACKUP_WAIT_MS;
  for (int i = 0; i < wait_ms; i++) {
    sceKernelDelayThread(1000);
    if (backup_has_room(size))
      return 1;
  }
  return 0;
}

// Hands one upload to the writer thread. Returns 0 if it could not be queued, in
// which case the texture simply stays resident rather than being lost.
static int backup_stage(TextureInfo *info, GLuint id, GLint level, GLsizei width, GLsizei height,
                        uint32_t size, GLint internalformat, GLenum format, GLenum type,
                        int compressed, const void *data) {
  if (!backup_ready || !data || size == 0)
    return 0;

  if (level == 0) {
    if (!extent_claim(info, sizeof(BackupRecord) + size))
      return 0;
    info->backup_bytes = 0;
    info->levels = 0;
  } else if (info->levels != (uint8_t)level) {
    // Levels have to arrive in order and exactly once: replay walks them from
    // zero, and a gap or a repeat would rebuild a different texture.
    return 0;
  }

  if (info->backup_bytes + sizeof(BackupRecord) + size > bucket_bytes(info->backup_bucket))
    return 0; // more mipmap levels than the extent was sized for

  if (!backup_make_room(size))
    return 0;

  uint32_t head = queue_head;
  PendingWrite *write = &queue[head & QUEUE_MASK];
  if (write->capacity < size) {
    free(write->payload);
    write->payload = malloc(size);
    write->capacity = write->payload ? size : 0;
  }
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
  write->offset = info->backup_offset + info->backup_bytes;

  info->backup_bytes += sizeof(BackupRecord) + size;
  info->levels++;
  info->backup_seq = head + 1;
  queued_bytes += size;

  __atomic_store_n(&queue_head, head + 1, __ATOMIC_RELEASE);
  sceKernelSignalSema(backup_sema, 1);
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

  if (sceIoLseek(backup_fd, info->backup_offset, SCE_SEEK_SET) < 0 ||
      sceIoRead(backup_fd, restore_scratch, info->backup_bytes) != (int)info->backup_bytes)
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
      glTexImage2D(GL_TEXTURE_2D, record->level, record->internalformat, record->width,
                   record->height, 0, record->format, record->type, payload);

    // vitaGL frees the old data before it allocates, and reports failure by
    // quietly not allocating. Left alone that is a texture whose descriptor
    // points at memory the garbage collector is about to hand out, so put a real
    // placeholder back rather than returning with a dangling one.
    if (!texture_is_allocated()) {
      install_placeholder(id);
      return 0;
    }
    offset += sizeof(BackupRecord) + record->size;
  }

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
// anything used within the last min_idle_frames frames alone. When lossy is
// false only textures we can read back again are touched. Returns how many
// candidates it found, so the caller can tell "nothing to evict" from "hit the
// per-frame cap".
static int evict_textures(size_t target_bytes, uint32_t min_idle_frames, int lossy) {
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
    return 0;

  GLuint previous = bound_textures[active_unit];

  int evicted = 0;
  for (int i = 0; i < num_candidates && tracked_bytes > target_bytes; i++) {
    evict_texture(candidates[i].id);
    evicted++;
  }

  glBindTexture(GL_TEXTURE_2D, previous);

  debugPrintf("texture cache: evicted %d textures%s, %d KB still in use\n",
              evicted, lossy ? " (no copy on disk)" : "", (int)(tracked_bytes / 1024));
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

  if (tracked_bytes <= budget) {
    starved_frames = 0;
    return;
  }

  // Trim back below the budget, but only touch textures the game has left alone
  // long enough that they cannot be part of what it is currently drawing.
  evict_textures(budget - budget / 8, TEXTURE_IDLE_FRAMES, 0);

  if (tracked_bytes <= budget) {
    starved_frames = 0;
    return;
  }

  // Still over. That is fine for a while: vitaGL only really frees a dropped
  // texture on its garbage collector a few frames later, and the working set may
  // genuinely be a little larger than the budget. Staying over budget is better
  // than evicting textures we are about to need again.
  starved_frames++;

  // But if we have not been able to keep up for a long time, we are holding
  // textures we have no copy of and memory has to be bounded whatever the card
  // is doing. These come back white, which beats running out of memory.
  if (starved_frames > TEXTURE_LOSSY_AFTER_FRAMES &&
      (tracked_bytes > budget + budget / 2 ||
       vitagl_free_memory() < (size_t)TEXTURE_RESERVE_MB * 1024 * 1024)) {
    evict_textures(budget, TEXTURE_IDLE_FRAMES_URGENT, 1);
    starved_frames = 0;
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

  // Below the start mark a copy would never be used: eviction does not run
  // until we are at the budget, and reaching it only requires the textures
  // above the line to be reloadable.
  int worth_backing = tracked_bytes >= (size_t)TEXTURE_BACKUP_START_MB * 1024 * 1024;
  if (!worth_backing)
    info->unbacked = 1;
  else if (level == 0 || !info->unbacked)
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
