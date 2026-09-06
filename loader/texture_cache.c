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
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <vitaGL.h>

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <malloc.h>
#include <string.h>

#include "main.h"
#include "config.h"
#include "texture_cache.h"

// vitaGL hands out texture names as indices into a fixed slot table, so a flat
// array keyed by name is all the bookkeeping we need. Names outside the range
// are left untracked, which only costs us the ability to evict them.
#define MAX_TEXTURES 16384
#define MAX_TEXTURE_UNITS 16
// vitaGL's allocation pools: 0 CDRAM, 1 RAM, 2 phycont. 3 is the newlib entry
// and always reads 0.
#define VGL_POOLS 3

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
  uint32_t backup_bytes; // bytes of texture data saved when it was evicted
  uint8_t *ram_copy; // the saved bytes, when they fit in the heap rather than on the card
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
// The highest texture name the game has actually used. The eviction pass walks
// this array every frame it wants to reclaim, and walking all 16384 slots means
// touching two megabytes of struct to look at a few thousand live entries --
// cache pressure every frame, for nothing.
static GLuint highest_id;
static uint32_t starved_frames; // frames the lossless pass has failed to keep up
// What each pool had free once the game was drawing, sampled on the first tick.
// Kept per pool rather than as a total: on hardware CDRAM ran to zero while the
// sum still read 101 MB free, because the RAM pool had plenty -- and the cache
// sat there having evicted nothing while vitaGL spilled textures past CDRAM,
// past RAM, and into the newlib heap the game needed.
static size_t pool_start[VGL_POOLS];
// Counted for the trace: this is the first time any of this has run on hardware.
static uint32_t evicted_count, restored_count, restore_failed_count;


static uint8_t *restore_scratch;
static int backup_ready;
static int cache_enabled = 1;
static size_t ram_cache_bytes; // evicted textures currently parked in the heap
// Whether reclaiming may fall back to the memory card at all. Set only when a
// pool is genuinely at its floor, where a hitch beats running out of memory.
static int card_writes_allowed;
// ...and whether it may at all, ever. Off leaves the cache its heap tier and
// nothing else: a texture with nowhere cheap to go stays where it is, which
// defers the eviction rather than losing the texture.
static int card_tier_enabled = 1;
// Consecutive frames the cache has wanted to free memory and freed none.
static uint32_t blocked_frames;
// Counted for the trace, to tell an eviction that cost a memcpy from one that
// cost a write.
static uint32_t ram_evicted_count, card_evicted_count, ram_restored_count;
// Evictions the cache wanted to make and could not this frame -- the heap tier
// full and the frame's card write already spent. Reclaiming that is blocked
// looks identical to reclaiming that is not needed unless this is counted.
static uint32_t deferred_count;


// vglMemFree refuses VGL_MEM_ALL: it is the enum terminator and the wrapper
// returns 0 for anything greater than or equal to it, so asking for "all" would
// silently report no memory at all and make us think we were always in trouble.
// Ask for each pool instead. The names have changed between vitaGL versions but
// the order has not: 0 is CDRAM, 1 is RAM, 2 is phycont, and 3 is either a
// fourth pool or the newlib entry, which always reports 0.
//
// Keeping them apart rather than summing them is the point. They are not
// interchangeable, and a total hides a pool that has run dry.
static void vitagl_free_per_pool(size_t out[VGL_POOLS]) {
  for (int pool = 0; pool < VGL_POOLS; pool++)
    out[pool] = vglMemFree((vglMemType)pool);
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

// Whether the newlib heap is too far gone to be parking textures in.
//
// Sampled once a frame, in the tick, and only read here. mallinfo walks the
// whole free list and takes the malloc lock to do it, so asking per eviction
// meant taking that lock up to sixty-four times a frame -- blocking every
// thread that wanted to allocate, exactly while the game was streaming an area
// in and allocating hardest. That is a stutter, not a measurement.
static int heap_tight;

// Also published, because the streaming gate needs the same figure and taking
// it twice would mean taking the malloc lock twice a frame for one number.
static size_t heap_used_bytes;

static void sample_heap(void) {
  struct mallinfo info = mallinfo();
  size_t limit = (size_t)(MEMORY_NEWLIB_MB - TEXTURE_HEAP_KEEP_FREE_MB) * 1024 * 1024;
  heap_used_bytes = (size_t)info.uordblks;
  heap_tight = heap_used_bytes > limit;
}

size_t texture_cache_heap_used(void) {
  return heap_used_bytes;
}

static int heap_is_tight(void) {
  return heap_tight;
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
  // Any file stays. Its name carries the texture's generation, so the next
  // eviction of this name writes a different file rather than colliding. A heap
  // copy has no such protection and is the only one of the two that costs
  // memory while it is held, so it goes now.
  if (info->ram_copy) {
    ram_cache_bytes -= info->backup_bytes;
    free(info->ram_copy);
    info->ram_copy = NULL;
  }
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


// Empties the store. Files are keyed by texture name and generation, neither of
// which means anything to a different run, so nothing in here is worth keeping
// across one -- and left alone it would grow every session.
//
// Names are copied out of the listing rather than acted on during it, since
// removing entries from a directory while reading it is not something to rely
// on. They are copied as bare names, not paths: a directory name here is two
// hex digits and a file name is under thirty characters, so the buffers stay
// small enough to sit on a thread stack.
static void purge_store(void) {
  SceUID top = sceIoDopen(TEXTURE_CACHE_DIR);
  if (top < 0)
    return;

  char subdirs[256][8];
  int num_subdirs = 0;
  SceIoDirent entry;
  while (num_subdirs < 256 && sceIoDread(top, &entry) > 0) {
    if (entry.d_name[0] == '.' || strlen(entry.d_name) >= sizeof(subdirs[0]))
      continue;
    strcpy(subdirs[num_subdirs], entry.d_name);
    num_subdirs++;
  }
  sceIoDclose(top);

  for (int i = 0; i < num_subdirs; i++) {
    // Through a local of a known size, so the compiler can see that neither
    // path below can overflow rather than assuming the whole table might.
    char name[sizeof(subdirs[0])];
    memcpy(name, subdirs[i], sizeof(name)); // bounded and terminated on the way in
    char dir_path[sizeof(TEXTURE_CACHE_DIR) + sizeof(name) + 1];
    snprintf(dir_path, sizeof(dir_path), "%s/%s", TEXTURE_CACHE_DIR, name);

    // One directory can hold more entries than a batch, so keep listing it
    // until a pass comes back short.
    for (;;) {
      SceUID dir = sceIoDopen(dir_path);
      if (dir < 0)
        break;
      char victims[32][40];
      int num_victims = 0;
      while (num_victims < 32 && sceIoDread(dir, &entry) > 0) {
        if (entry.d_name[0] == '.' || strlen(entry.d_name) >= sizeof(victims[0]))
          continue;
        strcpy(victims[num_victims], entry.d_name);
        num_victims++;
      }
      sceIoDclose(dir);

      for (int j = 0; j < num_victims; j++) {
        char victim[sizeof(victims[0])];
        memcpy(victim, victims[j], sizeof(victim));
        char victim_path[sizeof(dir_path) + sizeof(victim) + 1];
        snprintf(victim_path, sizeof(victim_path), "%s/%s", dir_path, victim);
        sceIoRemove(victim_path);
      }
      if (num_victims < 32)
        break;
    }
    sceIoRmdir(dir_path);
  }
  sceIoRmdir(TEXTURE_CACHE_DIR);
}

void texture_cache_init(void) {
  SceIoStat stat;
  cache_enabled = sceIoGetstat(TEXTURE_CACHE_DISABLE_PATH, &stat) < 0;
  if (!cache_enabled) {
    traceLog("texture cache: disabled by %s\n", TEXTURE_CACHE_DISABLE_PATH);
    return;
  }
  card_tier_enabled = sceIoGetstat(TEXTURE_DISK_DISABLE_PATH, &stat) < 0;
  if (!card_tier_enabled)
    traceLog("texture cache: card tier shut by %s, heap tier only\n",
             TEXTURE_DISK_DISABLE_PATH);

  // Start from nothing. On the console this runs once and there is nothing to
  // clear, but leaving state behind would mean a heap copy from a previous run
  // of the cache counted against this one's ceiling while belonging to a
  // texture name that now means something else.
  for (GLuint id = 0; id < MAX_TEXTURES; id++) {
    free(textures[id].ram_copy);
    memset(&textures[id], 0, sizeof(TextureInfo));
    textures[id].min_filter = GL_LINEAR;
  }
  memset(bound_textures, 0, sizeof(bound_textures));
  highest_id = 0;
  active_unit = 0;
  tracked_bytes = 0;
  ram_cache_bytes = 0;
  starved_frames = 0;
  blocked_frames = 0;
  evicted_count = restored_count = restore_failed_count = 0;
  ram_evicted_count = card_evicted_count = ram_restored_count = deferred_count = 0;
  memset(pool_start, 0, sizeof(pool_start));

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
  purge_store();
  sceIoMkdir(TEXTURE_CACHE_DIR, 0777);

  // The scratch buffer is allocated when a texture is first read back from the
  // card, not here. Spilling to the card is an emergency now and a whole
  // session can pass without one, so holding four megabytes of the game's heap
  // against the possibility is four megabytes the game could have had -- and it
  // is dying of exactly that.
  backup_ready = 1;
  traceLog("texture cache: store at %s (%d MB free on ux0), card tier %s\n",
           TEXTURE_CACHE_DIR, (int)(card_free / (1024 * 1024)),
           card_tier_enabled ? "open" : "shut");
}

void texture_cache_shutdown(void) {
  if (!backup_ready)
    return;
  // Nothing here outlives the run: a file is named for a texture name and a
  // generation of it, both of which the next run hands out to different
  // textures. Clearing it now keeps the card tidy, and init clears it again in
  // case the game did not get this far.
  backup_ready = 0;
  purge_store();
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

// Takes the texture's pixels out of GPU memory and puts them somewhere the GPU
// pools are not, just before the memory holding them is handed back. vitaGL
// keeps a texture in its own swizzled layout, so what is saved is the GPU's
// copy rather than the bytes the game originally uploaded, and restoring puts
// it back the same way round.
//
// The heap comes first and the card second. Both free the same GPU memory --
// the newlib heap is not mappable by the GXM, so a texture parked there has
// genuinely left the pools -- but one costs a memcpy and the other costs a
// write of a few hundred KB in the middle of a frame. Doing every eviction
// through the card is what made the framerate collapse on hardware.
//
// Except when the heap is the thing under pressure. The game allocates its
// level data out of the same heap, and on hardware it reached 153 MB of 160
// while the cache sat on textures; parking more there would take the game down
// rather than save it.
//
// Returns 1 once the bytes are safe, 0 if this texture can never be saved, and
// -1 if it could be but not in this frame. The caller has to keep those apart:
// only the middle one is a reason to stop considering the texture.
static int backup_capture(TextureInfo *info, GLuint id) {
  if (!backup_ready || info->levels == 0 || info->resident_size == 0)
    return 0;
  if (info->resident_size > (uint32_t)TEXTURE_BACKUP_MAX_KB * 1024)
    return 0;
  if (info->resident_size < TEXTURE_BACKUP_MIN_BYTES)
    return 0; // too small to be worth saving; it simply stays resident

  // Decided before reading the texture back, so that a frame which has used up
  // its card writes does not pay for a bind and a pointer fetch to find out.
  int to_card = ram_cache_bytes + info->resident_size > (size_t)TEXTURE_RAM_CACHE_MB * 1024 * 1024 ||
                heap_is_tight();
  // The card is for emergencies only. A write of a few hundred KB costs more
  // than a frame is worth, and an area load evicts continuously: doing that
  // through the card is 1-5 frames a second on hardware. When there is nowhere
  // cheap to put a texture and memory is not actually running out, the texture
  // simply stays where it is.
  //
  // No per-frame ration once it is allowed. Rationing existed to protect the
  // frame rate, and the only thing that reaches here now is a pool at its floor
  // -- where the alternative to a stutter is running out of memory.
  if (to_card && !card_writes_allowed)
    return -1; // not now; nothing wrong with the texture

  glBindTexture(GL_TEXTURE_2D, id);
  const void *pixels = vglGetTexDataPointer(GL_TEXTURE_2D);
  if (!pixels)
    return 0;

  if (!to_card) {
    // malloc failing is the heap telling us it has no more to give, whatever
    // our own ceiling says. Fall through to the card rather than insisting.
    uint8_t *copy = malloc(info->resident_size);
    if (copy) {
      memcpy(copy, pixels, info->resident_size);
      info->ram_copy = copy;
      info->backup_bytes = info->resident_size;
      ram_cache_bytes += info->resident_size;
      ram_evicted_count++;
      return 1;
    }
    if (!card_writes_allowed)
      return -1; // the heap would not give and the card is not open
  }

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
    return -1;
  int ok = sceIoWrite(fd, &record, sizeof(record)) == (int)sizeof(record) &&
           sceIoWrite(fd, pixels, info->resident_size) == (int)info->resident_size;
  sceIoClose(fd);
  if (!ok) {
    sceIoRemove(path); // a short write must not be mistaken for a usable copy
    return -1;
  }
  info->backup_bytes = info->resident_size;
  card_evicted_count++;
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

  // Where the bytes come back from. A heap copy is already in memory we own, so
  // it is used in place; only a texture that had to spill to the card is read
  // back into the scratch buffer.
  const uint8_t *saved = info->ram_copy;
  uint32_t saved_bytes = info->backup_bytes;
  if (!saved) {
    char path[160];
    texture_path(path, sizeof(path), id, info->serial);
    if (!restore_scratch) {
      restore_scratch = malloc((size_t)TEXTURE_BACKUP_MAX_KB * 1024);
      if (!restore_scratch)
        return 0; // no room to read it back; the placeholder stays
    }

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
    saved = restore_scratch;
    saved_bytes = record.bytes;
  }

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
  memcpy(pixels, saved, saved_bytes);

  // The heap copy has done its job and is the expensive one to keep around.
  if (info->ram_copy) {
    ram_cache_bytes -= info->backup_bytes;
    free(info->ram_copy);
    info->ram_copy = NULL;
    ram_restored_count++;
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
  if (id > highest_id)
    highest_id = id;
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
  int captured = backup_capture(info, id);
  if (captured <= 0) {
    // Tell "never" from "not this frame". A texture too small to be worth
    // saving, or one vitaGL will not hand a pointer to, is unbacked for good
    // and there is no point considering it again. One that only lost this
    // frame's ration of card writes has nothing wrong with it and must stay a
    // candidate, or the first busy frame would permanently retire everything it
    // could not get to and the cache would stop reclaiming.
    if (captured == 0)
      info->unbacked = 1;
    else
      deferred_count++;
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
  for (GLuint id = 1; id <= highest_id; id++) {
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

  debugPrintf("texture cache: evicted %d textures (%u to heap, %u to card), "
              "%d KB in use, %d KB parked\n",
              evicted, ram_evicted_count, card_evicted_count,
              (int)(tracked_bytes / 1024), (int)(ram_cache_bytes / 1024));
  return num_candidates;
}

void texture_cache_tick(void) {
  if (!cache_enabled)
    return;

  frame_counter++;

  // Once a frame, for everything that asks about it below.
  sample_heap();

  // A texture stays bound to its unit until something displaces it, so one the
  // game bound once and keeps drawing with is still in use even though we never
  // see another glBindTexture for it.
  for (int unit = 0; unit < MAX_TEXTURE_UNITS; unit++)
    texture_touch(bound_textures[unit]);

  const size_t budget = (size_t)TEXTURE_BUDGET_MB * 1024 * 1024;

  size_t free_now[VGL_POOLS];
  vitagl_free_per_pool(free_now);
  if (!pool_start[1]) { // the RAM pool is never zero once vitaGL is up
    vitagl_free_per_pool(pool_start);
    // Logged once, because every later judgement is made against these and a
    // trace that does not say what the thresholds were cannot be read.
    traceLog("texture cache: pools at start cdram %d ram %d phycont %d MB, "
             "reclaiming below %d / %d / %d MB\n",
             (int)(pool_start[0] / (1024 * 1024)), (int)(pool_start[1] / (1024 * 1024)),
             (int)(pool_start[2] / (1024 * 1024)),
             (int)(pool_start[0] / 100 * TEXTURE_FREE_HEADROOM_PERCENT / (1024 * 1024)),
             (int)(pool_start[1] / 100 * TEXTURE_FREE_HEADROOM_PERCENT / (1024 * 1024)),
             (int)(pool_start[2] / 100 * TEXTURE_FREE_HEADROOM_PERCENT / (1024 * 1024)));
  }

  // How far below its share any pool has fallen, added up. A pool is judged
  // against what it started with, not against the total, because they are not
  // interchangeable: textures prefer CDRAM, and once it is gone vitaGL falls
  // back to RAM and then to the newlib heap rather than failing, so an
  // exhausted CDRAM shows up as the game running out of heap somewhere else
  // entirely.
  // Pool 0 is CDRAM and is deliberately not counted. vitaGL allocates from it
  // first and falls back to RAM, so CDRAM at zero with RAM free is the
  // allocator doing its job, not a shortage -- and reclaiming against it meant
  // evicting continuously through every area load for no gain at all.
  size_t deficit = 0;
  for (int pool = 1; pool < VGL_POOLS; pool++) {
    size_t reserve = pool_start[pool] / 100 * TEXTURE_FREE_HEADROOM_PERCENT;
    if (free_now[pool] < reserve)
      deficit += reserve - free_now[pool];
  }

  if (tracked_bytes <= budget && deficit == 0) {
    starved_frames = 0;
    blocked_frames = 0;
    return;
  }

  // Trim back, but only touch textures the game has left alone long enough that
  // they cannot be part of what it is currently drawing. Aim under both limits:
  // the byte budget for a scene that simply hoards, and the headroom for one
  // whose pressure comes from everything else sharing these pools.
  size_t target = tracked_bytes;
  if (tracked_bytes > budget)
    target = budget - budget / 8;
  if (deficit)
    target = target > deficit ? target - deficit : 0;

  // Only a pool actually close to empty justifies touching the card. Being over
  // the byte budget does not: that is a backstop against hoarding, not a
  // shortage, and paying a card write per frame for it is what made area loads
  // unplayable.
  //
  // But a fixed threshold is not enough on its own. On hardware the RAM pool
  // settled at 13% -- above the 12% that opens the card, below the 25% the
  // cache is trying to reach -- and there it stayed, wanting to evict and being
  // refused, 3.6 million times, while the pools ran dry underneath it. So the
  // card also opens when reclaiming has simply not been working: if the cache
  // has wanted to free memory for this many consecutive frames and freed
  // nothing, cheap has been tried and cheap has failed.
  size_t emergency = pool_start[1] / 100 * TEXTURE_POOL_EMERGENCY_PERCENT;
  card_writes_allowed =
      card_tier_enabled && (free_now[1] < emergency || blocked_frames >= TEXTURE_BLOCKED_FRAMES);

  // Stop rescanning once it is established that there is nowhere to put
  // anything.
  //
  // The scan walks every texture the game has ever named -- thirty thousand of
  // them, two megabytes of struct -- to build a candidate list, and then places
  // none of them because the heap tier is full and the card is shut. A session
  // with the card tier shut did that for 3.4 million consecutive frames and
  // counted 11736069 deferrals doing it: the cache burned a full scan a frame,
  // for an hour, to reach the same answer every time.
  //
  // Being blocked normally opens the card, which resolves it. This only bites
  // when the card cannot open at all, so throttle rather than stop: one scan in
  // thirty still notices the moment conditions change.
  static uint32_t idle_scan;
  if (blocked_frames >= TEXTURE_BLOCKED_FRAMES && !card_writes_allowed &&
      ++idle_scan % TEXTURE_BLOCKED_RESCAN_FRAMES)
    return;

  size_t before = tracked_bytes;
  evict_textures(target, TEXTURE_IDLE_FRAMES);
  // Wanted to reclaim and got nothing: the heap tier is full or the game has
  // the heap, and the card is shut. Counted so that it cannot go on forever.
  if (tracked_bytes < before)
    blocked_frames = 0;
  else if (blocked_frames < TEXTURE_BLOCKED_FRAMES)
    blocked_frames++;

  if (tracked_bytes <= budget) {
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
    // Walking back into an area restores hundreds of textures, and all of it
    // can happen between two ticks -- so waiting for the next tick to make room
    // is waiting too long, and the restore that finds no memory is the crash
    // this cache exists to stop. If the pool a restore allocates from is at its
    // floor, take some back first. Urgent idle window, because anything drawn
    // recently is part of the area being walked into.
    if (pool_start[1] && vglMemFree((vglMemType)1) <
                             pool_start[1] / 100 * TEXTURE_POOL_EMERGENCY_PERCENT) {
      // Genuinely out of memory: a hitch beats a crash. Still nothing doing if
      // the tier is shut -- the eviction is deferred instead, and the pools
      // carry what the card would have taken.
      card_writes_allowed = card_tier_enabled;
      size_t target = tracked_bytes > info->resident_size ? tracked_bytes - info->resident_size : 0;
      evict_textures(target, TEXTURE_IDLE_FRAMES_URGENT);
    }

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
void texture_cache_stats(TextureCacheStats *out) {
  out->tracked_mb = (int)(tracked_bytes / (1024 * 1024));
  out->parked_mb = (int)(ram_cache_bytes / (1024 * 1024));
  out->evicted = (int)evicted_count;
  out->restored = (int)restored_count;
  out->failed = (int)restore_failed_count;
  out->spilled = (int)card_evicted_count;
  out->starved = (int)starved_frames;
  out->deferred = (int)deferred_count;
  out->blocked = (int)blocked_frames;
}
