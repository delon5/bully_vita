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

#define DATA_PATH "ux0:data/Bully"
#define SO_PATH DATA_PATH "/" "libBully.so"
#define CONFIG_PATH DATA_PATH "/" "config.txt"
#define GLSL_PATH DATA_PATH "/" "glsl"
#define GXP_PATH DATA_PATH "/" "gxp"

#define SCREEN_W 960
#define SCREEN_H 544

#define TOUCH_X_MARGIN 100

#endif
