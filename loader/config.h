#ifndef __CONFIG_H__
#define __CONFIG_H__

// #define DEBUG
// #define HAVE_RAZOR

#define LOAD_ADDRESS 0x98000000

#define MEMORY_SCELIBC_MB 4
#ifdef HAVE_RAZOR
#define MEMORY_NEWLIB_MB 256
#else
#define MEMORY_NEWLIB_MB 160
#endif
#define MEMORY_VITAGL_THRESHOLD_MB 8

// How much texture data the game is allowed to keep resident. The Android
// build never evicts anything, so this is what keeps it inside what vitaGL can
// hand out on a Vita (128MB of CDRAM plus whatever is left of main RAM once the
// game heap above has been carved out).
#define TEXTURE_BUDGET_MB 128
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
// Ceiling on the cache file. Past this we stop taking copies; the textures that
// miss out simply stay resident instead of being evicted.
#define TEXTURE_BACKUP_MAX_MB 768
// Largest texture, all mipmap levels together, we are willing to copy. This is
// also the size of the buffer we read one back through.
#define TEXTURE_BACKUP_MAX_KB 4096
// How much waiting-to-be-written texture data may sit in memory. If a streaming
// burst outruns the card we skip copies rather than stall the render thread.
#define TEXTURE_BACKUP_STAGING_KB 4096
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
