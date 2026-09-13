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
// Accounts for every allocation the game makes and reports the largest holders
// with the trace.
//
// OFF, and not to be left on. It takes a lock on every malloc and every free,
// contended between GameMain, Sound, RenderThread and CDStreamThread, and adds
// a malloc_usable_size call and a hash probe to each -- in a game that
// allocates as hard as this one that is a serialisation point, not an
// observation. On hardware it stalls the render thread while the audio thread
// plays on, then catches up: the game freezes and resyncs, over and over.
//
// It also costs 3 MB of .bss for the table, on a port whose whole problem is
// memory. Turn it on to answer a specific question about who is holding the
// heap, read the answer, and turn it off again.
// #define LOADER_ALLOC_TRACE
// #define HAVE_RAZOR

#define LOAD_ADDRESS 0x98000000

#define MEMORY_SCELIBC_MB 4
// The game's own heap. vitaGL takes whatever is left, and the two do not trade
// off gently: at 208 MB the pools came to 133 MB against a 104 MB texture
// working set, and the texture cache spent the whole session a few megabytes
// from its limit -- escalating every sixty frames and writing textures to the
// memory card, 524 times, each one a stall. A quarter of the frame rate samples
// were under 20 fps.
//
// At 160 MB the pools are 229 MB and never fell below 101 MB free all session,
// which is the difference between a cache that reclaims occasionally and one
// that thrashes. The extra heap did buy longer sessions before the game ran out
// of memory, but it bought them with a permanent stutter, and the leak it was
// meant to address is not the renderer's to fix.
//
// 160 -> 176, on measurement rather than hope. Over a session of 8.45 million
// frames the vitaGL RAM pool never fell below 19 MB free in any of 14087
// samples, and the phycont pool -- which vitaGL falls back to when RAM runs
// short -- reported its full 26 MB free in 14086 of them. So there are 19 MB
// the renderer demonstrably never wants and 26 MB behind it that it has never
// once had to reach for.
//
// The 208 MB attempt failed for a different reason than "too much heap": it cut
// the pools to 133 MB against a 104 MB texture working set, and the cache
// thrashed against its own limit for the whole session. 176 MB leaves 213 MB of
// pools against a working set that peaked at 137 MB, which is nowhere near
// that. The session that prompted this died with the arena at 159 MB of the
// 160 available and 16 KB of contiguous space left in it.
#ifdef HAVE_RAZOR
#define MEMORY_NEWLIB_MB 256
#else
#define MEMORY_NEWLIB_MB 176
#endif
#define MEMORY_VITAGL_THRESHOLD_MB 8

// Held back at startup and handed over the first time the game asks for memory
// and cannot have it.
//
// The failure that ends a session is never a clean one. ReadBuffer::RequestData
// grows its buffer by 21/13 each time, calls memalign, and stores a refcount
// through the result without looking at it -- so an allocation that returns
// NULL is a write to address zero two instructions later. The last coredump was
// that exact instruction, wanting 295356 bytes.
//
// The heap was not out of memory when it happened: 11 MB were free. They were
// simply in pieces, the largest of them 16 KB. A block reserved at boot and
// never touched is contiguous by construction, so giving it up supplies the one
// thing the free list had run out of.
//
// Set to 0 to turn the reserve off.
#define MEMORY_RESCUE_RESERVE_MB 4

// How much of the newlib heap to keep clear of the game's streamer.
//
// The game's CStreaming::IsThereEnoughFreeMemory never looked at memory at all
// -- it answered "yes" to any request under 10 MB -- so nothing streamed was
// ever freed. The loader answers it honestly instead, and this is the figure it
// answers against: once the heap is fuller than MEMORY_NEWLIB_MB minus this,
// the streamer is told to make room and evicts its least recently used models
// through its own code.
//
// Set too small and the game evicts constantly and re-streams what it just
// dropped; too large and it runs out before it is ever asked to free anything.
// A traced session reached 194 MB of a 208 MB heap before dying, so there is
// real room to reclaim here.
// The streamer's budget is not a fixed number: it is whatever the game has
// settled at once it is properly in play, plus this much room to grow before it
// is asked to let something go. Measured rather than chosen, because a figure
// picked in advance was wrong in the dangerous direction -- below what the game
// needs, so every answer was "no" and it evicted the world.
// How far the game may grow past what it settled at before the streamer is
// told to let something go. The traces put the growth at 86 MB over a session
// with nothing ever freed, so this is the size of the leak we are willing to
// carry rather than a guess at what the game wants.
#define STREAMING_BUDGET_MARGIN_MB 24
// The budget is never set above this much of the heap, whatever the game
// settled at, so the gate cannot end up asking for room that will not exist.
// 24 -> 18. The gate's ceiling was 152 MB and the game's working set in play is
// 151, so the ceiling was sitting exactly where the game wanted to be and the
// gate ground against it for a whole session. Measured against the arena rather
// than guessed: a session holding 153 MB live ran an arena of 164, so eleven
// megabytes above live covers the fragmentation, and a 158 MB ceiling leaves
// the arena around 169 of 176 with the rescue reserve behind it.
#define STREAMING_HEAP_KEEP_FREE_MB 18

// How much room to give the game above a figure it has proved it needs, when
// the budget follows it up. Smaller than the margin taken at calibration time:
// that one is a guess about a game that has barely started, this one is added
// to a reading of the game in play.
#define STREAMING_BUDGET_RAISE_MB 6
// Frames to let pass before taking that measurement, so it is a settled figure
// and not the middle of the first area load.
#define STREAMING_CALIBRATE_FRAMES 1800
// An empty file here turns the streaming fix off, the way no_texcache does for
// the texture cache.
#define STREAMING_DISABLE_PATH DATA_PATH "/" "no_streamfix"

// How long a vertex buffer must go unlocked before the loader takes back the
// CPU-side copy of its data. The game keeps that copy for the life of the
// buffer so it can be locked again without reading back from the GPU, which is
// a fair trade on a phone and is the largest single leak here.
//
// Long enough that anything the game touches regularly is never swept -- those
// are the buffers where handing back fresh memory could matter, since a buffer
// filled completely before it is unlocked does not care.
// Which cores the streaming thread may run on.
//
// The loader hard-pins one thread per core, which is the homebrew convention,
// and it puts two of them on core 2: the process's own main thread -- the one
// that runs ProcessEvents, and so the texture and vertex ticks -- and
// CDStreamThread. Measured on hardware they come to 54% and 45%, which is 99%
// of that core, while core 3 sits 61% idle with only Sound and the OpenAL mixer
// on it.
//
// The main thread is priority 127 and CDStreamThread is 65. Lower is higher
// here, so every time they want the core at once the streamer wins and the tick
// waits. That is the wrong way round: one of them is a background reader and
// the other is the frame.
//
// A live reading of all four cores while the game was running:
//
//   c0 85%   GameMain, alone
//   c1 28%   RenderThread, vitaGL garbage collector
//   c2 79%   this process's main thread, CDStreamThread
//   c3 34%   Sound, OpenAL mixer
//
// Two cores near saturation and two half idle, which is what pinning one
// thread per core and never revisiting it produces. Core 0 is not fixable from
// here -- that is one thread wanting 85% of a core, and no affinity mask splits
// a thread. Core 2 is, because two threads are on it and the slack is next
// door.
//
// So the streamer gets cores 1, 2 and 3 and the scheduler places it. Not a move
// to one chosen core: which core has room changes with the scene, and picking
// from a single sample is how you end up hand-pinning the next collision. Not
// core 0 either, since that is the busiest and the streamer would only wait
// there anyway.
//
// This cannot push anything important aside. CDStreamThread is priority 65;
// RenderThread, GameMain and the mixer are all 64, and lower wins here, so it
// can never preempt them. What it can displace is the main thread and the
// garbage collector, both 127 -- and on core 1 that is the collector, which is
// exactly the trade wanted.
//
// It is also what the retail titles do: a coreprobe capture of two of them
// shows every game thread carrying all of its cores in the mask, with the
// scheduler migrating them tens of thousands of times a session.
//
// Only when CapUnlocker is present. Without it core 3 belongs to the system and
// the loader stays off it entirely.
#define THREAD_CDSTREAM_AFFINITY (0x20000 | 0x40000 | 0x80000)

// Put it back on core 2 alone, to measure against.
#define THREAD_SPREAD_DISABLE_PATH DATA_PATH "/" "no_corespread"

#define VERTEX_CACHE_IDLE_FRAMES 600
// An empty file here turns it off.
#define VERTEX_CACHE_DISABLE_PATH DATA_PATH "/" "no_vertexfix"

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

// There was a MEMORY_VITAGL_CIRCULAR_POOL_MB here, set to 48, with a comment
// saying 48 MB absorbed the overruns "with room to spare". Nothing ever read
// it: vglSetCircularPoolSize is not called anywhere in the loader, so vitaGL
// has been running its 32 MB default the whole time and the comment described
// a build that never existed.
//
// Reading it properly, the pool is not spare memory to reclaim -- it is
// already short. It is split across gxm_display_buffer_count buffers, which is
// three, so each frame has 10.6 MB of it; and vitaGL only logs the overrun,
// which reports the overshoot rather than the demand. An overshoot of 6535454
// bytes against a 10.6 MB allowance means that frame wanted about 17 MB. It
// comes out of the same RAM the textures allocate from, so raising it costs the
// texture pool directly, and it has overrun exactly once per session, during
// the attract movie, never in play. Left at vitaGL's default deliberately.

// Create this file to turn the loader's texture cache off.
//
// vitaGL's own cache (HAVE_TEXTURE_CACHE) is enabled too, but it cannot carry
// this alone: it only reclaims when a GPU allocation fails, and vitaGL's
// allocator falls back from CDRAM to RAM to the newlib heap, so an allocation
// does not fail until everything is gone. On hardware CDRAM reached zero with
// its sweep never once having run and its folder empty. Reclaiming has to start
// from a budget, while there is still memory to reclaim into.
#define TEXTURE_CACHE_DISABLE_PATH DATA_PATH "/" "no_texcache"

// Create this file to keep the cache but shut the memory card tier, leaving it
// the heap tier and nothing else.
//
// Worth being able to answer without a rebuild, because the writes have never
// paid for themselves. Across four sessions the cache spilled 500, 415, 681 and
// 1100 textures to the card and restored 84, 21, 122 and 327 -- and those
// restore figures include the ones that came back from the heap, so the share
// of writes ever read is lower still. What kept the tier was that it is the
// only thing standing between a texture the cache had to drop and a texture the
// game cannot get back, and a white placeholder is not an acceptable answer.
//
// With this set, a texture with nowhere cheap to go simply stays where it is.
// That is safe -- nothing is lost, the eviction is deferred -- and the question
// it settles is whether the pools can carry the working set without the card at
// all. On the last session they never came close to empty: the RAM pool never
// fell below 19 MB free and phycont was never touched.
#define TEXTURE_DISK_DISABLE_PATH DATA_PATH "/" "no_texdisk"

// Tested, and the answer is no: the pools cannot carry the working set on their
// own. With the card tier shut the cache evicted 337 textures in the first
// 540000 frames, filled its 13 MB of heap tier, and then had nowhere to put
// anything for the remaining 3.4 million -- the RAM pool bled 11 MB -> 0, took
// phycont's 26 MB down with it, and vitaGL was failing 32 KB allocations by the
// end. The newlib heap was never the problem in that session: 142 MB live of a
// 176 MB arena, and not one refused allocation.
//
// So the switch stays for diagnosis and the tier stays on by default.

// How often to rescan when the cache is blocked and the card cannot open.
// One frame in this many; see the comment at the use.
#define TEXTURE_BLOCKED_RESCAN_FRAMES 30

// Create this file to have the texture store emptied at startup.
//
// The store is kept across runs, so there has to be a way to throw it away
// without deleting a directory by hand -- after a change to how records are
// written, or if the card was pulled mid-write often enough to be worth not
// wondering about. It is not self-clearing: while the file is there, every
// launch starts from nothing.
#define TEXTURE_STORE_WIPE_PATH DATA_PATH "/" "wipe_texcache"

// The longest the cache will go without asking vitaGL how much of each pool is
// free, and how fast it assumes a pool can drain while it is not asking.
//
// vglMemFree walks vitaGL's own free lists: measured at 8062 us for the five
// pools, per call. Once a frame made it 8 ms of every 33 ms frame -- a quarter
// of the game's frame time, and by far the largest thing this loader cost.
//
// It cannot simply be sampled less often, though: a pool that runs dry is the
// crash this cache exists to prevent, and a test that drops a pool below the
// mark and expects it reclaimed within four frames failed when this was a flat
// fifteen. So the interval is the distance to the mark divided by the megabytes
// a frame can plausibly take, which is full rate near the line and a fifteenth
// of it when a pool is twenty megabytes clear -- where it normally sits.
#define TEXTURE_POOL_SAMPLE_FRAMES 15
#define TEXTURE_POOL_MB_PER_FRAME 2

#define TEXTURE_FREE_HEADROOM_PERCENT 25

// ...and the mark reclaiming starts at. Reaching for a quarter free only once a
// pool has actually dropped to this, rather than the moment it slips below the
// quarter, is what stops the cache trickling evictions forever against a pool
// that is sitting a megabyte under its ideal. Above the emergency threshold, so
// the card still opens before this ever becomes a shortage.
#define TEXTURE_FREE_HEADROOM_LOW_PERCENT 15
// The point at which a pool counts as actually running out, rather than merely
// below its target. Only here may reclaiming fall back to the memory card, so
// this is the floor the cache really defends: above it a texture with nowhere
// cheap to go simply stays resident, because a write inside a frame costs more
// than the memory is worth. Under sustained pressure free memory settles here
// rather than at the target above, which is the intended hysteresis.
#define TEXTURE_POOL_EMERGENCY_PERCENT 12
// How long the cache may want to free memory and free none before it stops
// waiting for a cheap way to do it and uses the card. Two seconds or so: long
// enough that a passing spike is not paid for with a stutter, short enough that
// it cannot sit blocked while the pools drain, which on hardware it did for
// three and a half million frames.
#define TEXTURE_BLOCKED_FRAMES 60

// Still capped in bytes as well, so a scene that never pressures the pools does
// not sit on an unbounded pile of textures it stopped drawing with. This is a
// backstop, not the working limit -- the headroom rule above is what should
// normally be doing the reclaiming. Set too low it evicts constantly while
// there is memory to spare, and every eviction is work in the middle of a
// frame: at 64 MB that showed up on hardware as the framerate falling off 30
// down to nothing. It is a backstop against a scene that hoards without ever
// pressuring the pools, not the working limit -- so it belongs above the
// working set, not through it. A session traced on hardware peaked at 110 MB of
// textures with 101 MB still free across the pools: nothing needed evicting,
// and a 96 MB budget had the cache fighting a working set that fitted.
#define TEXTURE_BUDGET_MB 160
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

// Evicted textures are held in the newlib heap up to this much, and only spill
// to the memory card past it. The heap is not GPU-mappable, so a texture parked
// there has genuinely left the pools vitaGL allocates from, which is the memory
// this cache exists to reclaim -- and putting it back is a memcpy rather than a
// read off the card. Kept well short of the heap so the game still has its own
// room; when it is full, or the heap will not give, eviction uses the card.
#define TEXTURE_RAM_CACHE_MB 40
// And never within this much of the end of the heap, whatever the ceiling above
// allows. The heap is the game's before it is ours: a session that crashed had
// it at 153 MB of 160, so parking textures by our own ceiling alone would have
// been what killed it. Measured against what is actually free rather than as a
// share of the total, because the game's own usage is most of it and a
// percentage of the total says nothing about what is left.
#define TEXTURE_HEAP_KEEP_FREE_MB 40

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

#define SCREEN_W 960
#define SCREEN_H 544

#define TOUCH_X_MARGIN 100

#endif
