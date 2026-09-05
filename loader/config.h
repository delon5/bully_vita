#ifndef __CONFIG_H__
#define __CONFIG_H__

// #define DEBUG
// Writes a line to ux0:data/bully_trace.txt at each step of startup. Separate
// from DEBUG on purpose: DEBUG also forwards every one of the game's own log
// lines, which is thousands of file opens and slow enough to look like a hang.
// #define LOADER_TRACE
// #define HAVE_RAZOR

#define LOAD_ADDRESS 0x98000000

#define MEMORY_SCELIBC_MB 4
#ifdef HAVE_RAZOR
#define MEMORY_NEWLIB_MB 256
#else
#define MEMORY_NEWLIB_MB 160
#endif
#define MEMORY_VITAGL_THRESHOLD_MB 8

// vitaGL reports "Circular pool overrun on frame 376 (Total of 6532574 bytes)"
// during the attract mode, which it warns costs performance. The default is
// 32MB and the game asks for a little over 6MB in a single frame there, so the
// overrun is transient spikes rather than a steady shortfall; 48MB absorbs them
// with room to spare and still leaves the heaps well clear of the budget the
// texture cache works to.
#define MEMORY_VITAGL_CIRCULAR_POOL_MB 48

// Create this file to turn the texture cache off: nothing is tracked, nothing
// is evicted and no backing store is opened, leaving the game's texture
// handling exactly as it was. It is a runtime switch rather than a build option
// so the binary is identical either way, which matters both for telling a
// problem in here apart from a problem elsewhere and because changing the size
// of the loader can upset how vita-elf-create lays out its segments.
#define TEXTURE_CACHE_DISABLE_PATH DATA_PATH "/" "no_texcache"

// How much texture data the game is allowed to keep resident. The Android
// build never evicts anything, so this is what keeps it inside what vitaGL can
// hand out on a Vita (128MB of CDRAM plus whatever is left of main RAM once the
// game heap above has been carved out).
#define TEXTURE_BUDGET_MB 128
// Nothing is evicted while we are comfortably under budget, so a copy taken
// then is written to the card for no reason -- and taking one for every
// texture the game loads means writing a couple of hundred megabytes to slow
// storage during startup, competing with the game's own asset reads. Start
// copying once resident textures pass this mark. What was loaded before it is
// the core set the game keeps hot anyway, and the growth that used to run the
// console out of memory is all above the line and backed as normal.
#define TEXTURE_BACKUP_START_MB 64
// Evict regardless of the budget once vitaGL has less than this much free, so
// that memory pressure coming from anywhere else does not kill us either.
#define TEXTURE_RESERVE_MB 32
// How long a texture has to go unused before we are willing to drop it, in
// frames. The urgent value applies when we are about to run out of memory.
#define TEXTURE_IDLE_FRAMES 150
#define TEXTURE_IDLE_FRAMES_URGENT 30
// Upper bound on how many textures a single frame may evict, so that reclaiming
// memory does not turn into a visible hitch.
#define TEXTURE_EVICTIONS_PER_FRAME 64

// Where the source bytes of uploaded textures are kept so that an evicted one
// can be uploaded again when the game draws with it. Truncated on every boot,
// since texture names are handed out afresh each run.
#define TEXTURE_BACKUP_PATH DATA_PATH "/" "texcache.bin"
// Ceiling on the cache file, and how much of the card to leave alone. The
// actual limit is whichever is smaller: this, or the free space minus the
// reserve. Filling a memory card is not a fair thing to do to somebody -- the
// game writes its saves and its .obb indexes to the same card, and those do not
// survive being truncated by a full disk.
#define TEXTURE_BACKUP_MAX_MB 512
#define TEXTURE_BACKUP_KEEP_FREE_MB 512
// Below this there is no point having a backing store at all; the cache runs
// without one and falls back to evicting textures it cannot reload, which
// still keeps memory bounded.
#define TEXTURE_BACKUP_MIN_MB 64
// Largest texture, all mipmap levels together, we are willing to copy.
#define TEXTURE_BACKUP_MAX_KB 4096
// Staging arena for copies waiting to be written. It has to be a good few times
// the largest texture, or the biggest textures -- the ones most worth evicting
// -- only ever get a copy when the queue happens to be completely empty.
#define TEXTURE_BACKUP_STAGING_KB (4 * TEXTURE_BACKUP_MAX_KB)
// Frames the lossless pass may fail to reach the budget before we accept losing
// textures that have no copy. vitaGL frees texture memory on its garbage
// collector several frames after we drop a texture, so a shorter window would
// escalate against memory that is already on its way back.
#define TEXTURE_LOSSY_AFTER_FRAMES 60
// The writer thread only ever touches the memory card, so it runs below every
// thread the game creates (which sit at 64-65) and shares the streaming core.
// How long an upload may wait for the writer to make room. While we are far
// from the budget a missed copy costs nothing, so we do not wait at all; the
// longer bound only applies once we are over budget, where stalling briefly
// beats losing the ability to free the texture at all.
#define TEXTURE_BACKUP_WAIT_MS 8
#define TEXTURE_BACKUP_WAIT_MS_MAX 120
#define TEXTURE_BACKUP_THREAD_PRIORITY 0x7F
#define TEXTURE_BACKUP_THREAD_AFFINITY 0x40000

#define DATA_PATH "ux0:data/Bully"
#define SO_PATH DATA_PATH "/" "libBully.so"
#define CONFIG_PATH DATA_PATH "/" "config.txt"
#define GLSL_PATH DATA_PATH "/" "glsl"
#define GXP_PATH DATA_PATH "/" "gxp"

#define SCREEN_W 960
#define SCREEN_H 544

#define TOUCH_X_MARGIN 100

#endif
