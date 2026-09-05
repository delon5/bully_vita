#ifndef __CONFIG_H__
#define __CONFIG_H__

// #define DEBUG
// Writes a line to ux0:data/bully_trace.txt at each step of startup, and a
// heartbeat with the memory figures every 600 iterations of the game's main
// loop. Separate from DEBUG on purpose: DEBUG also forwards every one of the
// game's own log lines, which is thousands of file opens and slow enough to
// look like a hang. This is about 230 lines in a full session, which costs
// nothing.
//
// ON DELIBERATELY, and it stays on until the game stops crashing. A crash
// without a trace is a guess, and guessing is what cost this port a day: the
// eviction rule that never once fired was invisible until a heartbeat printed
// "cdram 0 ... ev 0". Turn it off once a session survives.
#define LOADER_TRACE
// #define HAVE_RAZOR

#define LOAD_ADDRESS 0x98000000

#define MEMORY_SCELIBC_MB 4
#ifdef HAVE_RAZOR
#define MEMORY_NEWLIB_MB 256
#else
#define MEMORY_NEWLIB_MB 160
#endif
#define MEMORY_VITAGL_THRESHOLD_MB 8

// How long a texture must go undrawn before vitaGL's own cache is allowed to
// write it out and free it, in frames. vitaGL defaults to 3600 -- two minutes
// at the frame rate this runs at -- and it only reclaims when an allocation has
// already failed. Walking into a new area asks for several thousand textures in
// a few seconds, and nothing the previous area used is anywhere near that stale,
// so the sweep finds nothing eligible, frees nothing, and the allocation fails
// anyway: on hardware the cache folder stayed empty right up to the crash.
// A few seconds is long enough that anything still on screen is safe, and short
// enough that the area you just left can be reclaimed to make room for the one
// you are walking into.
#define TEXTURE_CACHE_IDLE_FRAMES 240

// vitaGL reports "Circular pool overrun on frame 376 (Total of 6532574 bytes)"
// during the attract mode, which it warns costs performance. The default is
// 32MB and the game asks for a little over 6MB in a single frame there, so the
// overrun is transient spikes rather than a steady shortfall; 48MB absorbs them
// with room to spare and still leaves the heaps well clear of the budget the
// texture cache works to.
#define MEMORY_VITAGL_CIRCULAR_POOL_MB 48

// Create this file to turn the loader's texture cache off.
//
// vitaGL's own cache (HAVE_TEXTURE_CACHE) is enabled too, but it cannot carry
// this alone: it only reclaims when a GPU allocation fails, and vitaGL's
// allocator falls back from CDRAM to RAM to the newlib heap, so an allocation
// does not fail until everything is gone. On hardware CDRAM reached zero with
// its sweep never once having run and its folder empty. Reclaiming has to start
// from a budget, while there is still memory to reclaim into.
#define TEXTURE_CACHE_DISABLE_PATH DATA_PATH "/" "no_texcache"

// How much texture data the game is allowed to keep resident. The Android
// build never evicts anything, so this is what keeps it inside what vitaGL can
// hand out on a Vita (128MB of CDRAM plus whatever is left of main RAM once the
// game heap above has been carved out).
// Reclaim to keep this much of vitaGL's memory free, as a percentage of what
// it had when the game started drawing. A byte budget cannot work on its own:
// it only counts textures this cache tracks, while framebuffers, vertex
// buffers, shader programs, render targets and untracked textures come out of
// the same pools -- so CDRAM reached zero on hardware while the counter still
// read comfortably under budget. Free memory counts all of it.
#define TEXTURE_FREE_HEADROOM_PERCENT 25

// Still capped in bytes as well, so a scene that never pressures the pools does
// not sit on an unbounded pile of textures it stopped drawing with. This is a
// backstop, not the working limit -- the headroom rule above is what should
// normally be doing the reclaiming. Set too low it evicts constantly while
// there is memory to spare, and every eviction is work in the middle of a
// frame: at 64 MB that showed up on hardware as the framerate falling off 30
// down to nothing. Set too high it never fires at all: at 144 MB a session that
// crashed had reached 110 MB tracked having evicted nothing.
#define TEXTURE_BUDGET_MB 96
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
// Of those, how many may fall through to the memory card in one frame. A card
// write of a few hundred KB costs more than a frame is worth, so evictions that
// have to make one are rationed hard even though evictions into RAM are not.
#define TEXTURE_CARD_WRITES_PER_FRAME 1

// Evicted textures are held in the newlib heap up to this much, and only spill
// to the memory card past it. The heap is not GPU-mappable, so a texture parked
// there has genuinely left the pools vitaGL allocates from, which is the memory
// this cache exists to reclaim -- and putting it back is a memcpy rather than a
// read off the card. Kept well short of the heap so the game still has its own
// room; when it is full, or the heap will not give, eviction uses the card.
#define TEXTURE_RAM_CACHE_MB 24
// And never past this share of the heap, whatever the ceiling above allows. The
// heap is the game's before it is ours: a session that crashed had it at 153 MB
// of 160, so parking textures by our own ceiling alone would have been what
// killed it. Past this, eviction spills to the card instead.
#define TEXTURE_HEAP_PARK_PERCENT 60

// Where the source bytes of uploaded textures are kept so that an evicted one
// can be uploaded again when the game draws with it. Truncated on every boot,
// since texture names are handed out afresh each run.
// One file per texture, named by a key derived from its contents, spread over
// 256 subdirectories so no single directory grows to thousands of entries. The
// store outlives the process on purpose: the key does not depend on the texture
// name vitaGL handed out this run, so a second run finds its textures already
// there and writes nothing. Deleting the folder is always safe.
#define TEXTURE_CACHE_DIR DATA_PATH "/" "textures"
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
// Smallest texture worth a file on the card. The game uploads thousands of
// small textures for every large one, so the small ones are nearly all of the
// file operations and almost none of the memory -- and a file costs a directory
// lookup on every later run even when it is already there, which is why a
// second run was no faster than the first.
//
// Below this a texture is kept resident and never evicted. That was dangerous
// while eviction had a last resort that dropped textures with no copy: it is
// what put a black character and then black ground on screen. With that gone,
// the worst an uncopied texture can do is stay.
#define TEXTURE_BACKUP_MIN_BYTES (32 * 1024)

// Largest texture, all mipmap levels together, we are willing to copy.
#define TEXTURE_BACKUP_MAX_KB 4096
// Staging arena for copies waiting to be written. It has to be a good few times
// the largest texture, or the biggest textures -- the ones most worth evicting
// -- only ever get a copy when the queue happens to be completely empty.
#define TEXTURE_BACKUP_STAGING_KB (4 * TEXTURE_BACKUP_MAX_KB)
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
