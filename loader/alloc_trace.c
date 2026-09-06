/* alloc_trace.c -- who is holding the heap.
 *
 * The port's remaining crash is the game running out of newlib heap after a
 * long session: a coredump lands in _malloc_r with the heap at 194 MB of 208,
 * and the trace shows it climbing steadily the whole way. Textures are not it
 * -- those live in vitaGL's pools and the texture cache holds them flat -- so
 * something else is being loaded and never let go, which is the Android
 * assumption this port exists to work around.
 *
 * Every allocation the game makes comes through the loader, because the loader
 * is what resolves malloc for the .so. So rather than guess at what is growing,
 * account for it: keep what is live, keyed by the address that asked for it,
 * and report the largest holders. The caller is reported as an offset into
 * libBully.so, which is what a disassembler needs to name the function.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dialog.h"
#include "main.h"
#include "config.h"
#include "alloc_trace.h"
#include "so_util.h"

#ifdef LOADER_ALLOC_TRACE

// The genuine allocators, behind the --wrap layer installed at the bottom of
// this file.
void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *ptr, size_t size);
void *__real_memalign(size_t alignment, size_t size);
void *__real_aligned_alloc(size_t alignment, size_t size);
void __real_free(void *ptr);

// Live allocations of SMALL_BYTES or more, open addressed by pointer. Only
// large blocks reach it, so it can be a fraction of the size the first version
// needed -- 384 KB rather than three megabytes, which matters on a port that is
// short of memory. A full table would silently stop accounting, so overflow is
// counted and reported rather than ignored.
#define LIVE_SLOTS 32768
#define LIVE_MASK (LIVE_SLOTS - 1)

// Allocations smaller than this are counted in bulk, without the table and
// without the lock. The first version of this drew the line at 1 KB, which put
// a contended lock in front of nearly every allocation the game made and turned
// the game into a slideshow. At 64 KB the table sees a few thousand blocks
// rather than hundreds of thousands, so the lock is taken rarely enough to
// disappear -- and a leak measured in megabytes is made of large blocks or of
// so many small ones that the bulk counter shows it anyway.
#define SMALL_BYTES (64 * 1024)

// One small allocation in this many is credited to its call site, so the bulk
// of the game's memory can be named without the table's lock in front of every
// allocation it makes.
#define SMALL_SAMPLE 64
// The eboot's text starts here or above; the game's .so is mapped much higher,
// at LOAD_ADDRESS, which is how a call site tells you which of the two asked.
#define EBOOT_TEXT_BASE 0x81000000u
// Marks a sampled block, so the report can scale it back up and so its bytes
// are not counted twice against the large total.
#define SAMPLED_FLAG 0x40000000u

// Distinct call sites worth remembering. Raised well past the eboot's, because
// a site that cannot claim a slot goes uncredited and its memory then appears
// only in the total -- which is exactly the kind of gap this exists to close.
#define CALLERS 2048

// A slot that has been freed, as distinct from one that was never used. Open
// addressing puts a block wherever it lands after its hash, so a lookup has to
// keep probing past collisions; clearing a slot to empty on deletion cuts that
// chain, and every block that landed after the deleted one becomes invisible.
// Its bytes are then never taken off the counters, and its slot is never
// reclaimed -- which is the whole of the accounting corruption: totals that
// only rise, an underflow when a fallback fires for a block that was still
// tabled, and a table that fills with entries nothing can find.
#define TOMBSTONE ((void *)1)

typedef struct {
  void *ptr;
  uint32_t size;
  uint32_t caller; // offset into the .so, or 0 for a slot that is free
} LiveBlock;

typedef struct {
  uint32_t caller;
  uint32_t live_bytes;
  uint32_t live_count;
  uint32_t total_count;
} CallerTotals;

// GameMain, Sound, RenderThread and CDStreamThread all allocate, so the table
// needs a lock. newlib's own malloc takes one anyway -- the coredump that
// started this landed in it -- so this adds a spin, not a new contention point.
static volatile int table_lock;

static inline void lock_table(void) {
  while (__atomic_test_and_set(&table_lock, __ATOMIC_ACQUIRE))
    ;
}

static inline void unlock_table(void) {
  __atomic_clear(&table_lock, __ATOMIC_RELEASE);
}

static LiveBlock live[LIVE_SLOTS];
static CallerTotals callers[CALLERS];
static size_t large_live_bytes, small_live_bytes;
// Frees of blocks that were never counted. OpenAL allocates through
// aligned_alloc, which reaches _memalign_r without ever naming the memalign
// symbol, so --wrap cannot see it -- while al_free goes straight through
// __wrap_free. Every one of those frees was subtracting bytes that had never
// been added, and after a session of voices and buffers the small total wrapped
// past zero and printed as 4007 MB.
static uint32_t unknown_frees;
static uint32_t small_live_count, untracked_frees, table_full;

// Pointers from a single heap are 8 byte aligned at worst, so the low bits
// carry no information. Spread the useful ones over the table.
static inline uint32_t hash_ptr(const void *ptr) {
  uint32_t x = (uint32_t)(uintptr_t)ptr >> 3;
  x *= 0x9e3779b1u;
  return x ^ (x >> 15);
}

// Where in libBully.so the caller is, which is the only form of the address
// that means anything once the module has been relocated.
static uint32_t so_offset(void *return_address) {
  uintptr_t addr = (uintptr_t)return_address;
  if (addr < bully_mod.text_base || addr >= bully_mod.text_base + bully_mod.text_size)
    return 0; // not the game: the loader's own allocations are not the question
  return (uint32_t)(addr - bully_mod.text_base);
}

// The totals for one call site, claiming a free slot the first time it is seen.
// A slot is free while it has never been used; slots are never given back, so a
// site that allocated and freed everything still shows how much it ever did.
static CallerTotals *caller_slot(uint32_t caller) {
  uint32_t i = (caller * 0x9e3779b1u) >> 21;
  for (uint32_t probe = 0; probe < CALLERS; probe++) {
    CallerTotals *c = &callers[(i + probe) & (CALLERS - 1)];
    if (c->caller == caller)
      return c;
    if (c->total_count == 0) {
      c->caller = caller;
      return c;
    }
  }
  return NULL; // more distinct call sites than slots; the rest go uncredited
}

// Put one block in the table, credited to a call site. The caller value is
// whatever identifies the code that asked: an offset into the .so for the
// game, an eboot address for the loader.
// Whether a small block is one of the sampled ones. Derived from the pointer
// rather than from a counter, so that the free path reaches the same answer as
// the allocation path without having to look anything up.
static inline int sampled(const void *ptr) {
  return (hash_ptr(ptr) & (SMALL_SAMPLE - 1)) == 0;
}

// Returns whether the block got a slot. The eboot's totals are kept in step
// with that answer rather than bumped alongside it: they used to be added on
// the way in and only subtracted if the block was still findable on the way
// out, so a block lost to a full table was counted in and never out, and the
// loader's figure grew by exactly the size of every overflow, for ever.
static int remember_block(void *ptr, size_t size, uint32_t caller) {
  uint32_t i = hash_ptr(ptr) & LIVE_MASK;
  lock_table();
  for (uint32_t probe = 0; probe < 64; probe++) {
    LiveBlock *b = &live[(i + probe) & LIVE_MASK];
    if (!b->ptr || b->ptr == TOMBSTONE) {
      b->ptr = ptr;
      b->size = (uint32_t)size;
      b->caller = caller;
      // The game's large blocks only. Sampled blocks belong to the small
      // totals, and the eboot has its own, so counting either here made the
      // game's figure open at 68 MB before it had allocated anything.
      if (!(caller & SAMPLED_FLAG) && caller < EBOOT_TEXT_BASE)
        large_live_bytes += size;
      CallerTotals *c = caller_slot(caller);
      if (c) {
        c->live_bytes += (uint32_t)size;
        c->live_count++;
        c->total_count++;
      }
      unlock_table();
      return 1;
    }
  }
  table_full++;
  unlock_table();
  return 0;
}

// Take a block back out, returning how big it was, or 0 if it was never in
// there -- which is how the caller tells a tabled block from a bulk-counted one.
static size_t forget_block(void *ptr, uint32_t *caller_out) {
  uint32_t i = hash_ptr(ptr) & LIVE_MASK;
  lock_table();
  for (uint32_t probe = 0; probe < 64; probe++) {
    LiveBlock *b = &live[(i + probe) & LIVE_MASK];
    if (b->ptr == ptr) {
      size_t size = b->size;
      if (caller_out)
        *caller_out = b->caller;
      if (!(b->caller & SAMPLED_FLAG) && b->caller < EBOOT_TEXT_BASE)
        large_live_bytes -= size;
      CallerTotals *c = caller_slot(b->caller);
      if (c && c->caller == b->caller) {
        c->live_bytes -= b->size;
        c->live_count--;
      }
      b->ptr = TOMBSTONE; // not NULL: that would cut the chain behind it
      b->size = 0;
      b->caller = 0;
      unlock_table();
      return size ? size : 1;
    }
    if (!b->ptr)
      break; // a slot never used ends the chain; a tombstone does not
  }
  unlock_table();
  return 0;
}

static void remember(void *ptr, size_t size, void *return_address) {
  if (!ptr)
    return;
  // What the heap actually gave up, not what was asked for, so that these
  // figures can be compared with mallinfo and so that a block is classified the
  // same way when it is freed as when it was taken.
  (void)size;
  size = malloc_usable_size(ptr);

  // Small blocks never touch the table or the lock: atomics on two counters,
  // which is the difference between an observation and a stall.
  if (size < SMALL_BYTES) {
    __atomic_add_fetch(&small_live_bytes, size, __ATOMIC_RELAXED);
    __atomic_add_fetch(&small_live_count, 1, __ATOMIC_RELAXED);
    // One in SMALL_SAMPLE of them is credited to its call site anyway. 44 MB
    // across 175000 small allocations is the largest thing the game holds and
    // the only one still anonymous, and tabling them all would put the lock
    // back in front of every allocation -- which is what made the game a
    // slideshow before. Sampling costs a counter and names the holder just as
    // well: a site responsible for a tenth of them is a tenth of the sample.
    if (sampled(ptr))
      remember_block(ptr, size, so_offset(return_address) | SAMPLED_FLAG);
    return;
  }

  remember_block(ptr, size, so_offset(return_address));
}

static void forget(void *ptr) {
  if (!ptr)
    return;

  size_t size = malloc_usable_size(ptr);
  if (size < SMALL_BYTES) {
    // Never below zero. A total that wraps is worse than one that is short: it
    // reads as gigabytes and buries the answer, where a floor plus a count of
    // what could not be explained says plainly how much to trust the figure.
    if (__atomic_load_n(&small_live_count, __ATOMIC_RELAXED) == 0 ||
        __atomic_load_n(&small_live_bytes, __ATOMIC_RELAXED) < size) {
      __atomic_add_fetch(&unknown_frees, 1, __ATOMIC_RELAXED);
      return;
    }
    __atomic_sub_fetch(&small_live_bytes, size, __ATOMIC_RELAXED);
    __atomic_sub_fetch(&small_live_count, 1, __ATOMIC_RELAXED);
    // A sampled block has to leave the table too. Counting one in every
    // sixty-four in but never out would have made every high-churn call site --
    // the per-frame temporaries -- look like the biggest leak in the game,
    // while a real one stayed buried. It would also have filled the table with
    // dead entries and corrupted the large-block totals along with it.
    if (sampled(ptr))
      forget_block(ptr, NULL);
    return;
  }

  if (!forget_block(ptr, NULL))
    untracked_frees++; // allocated before tracking began, or lost to a full table
}

// operator new and operator new[], replaced rather than merely observed.
//
// The game defines its own -- the standard libstdc++ shape: round zero up to
// one, malloc, and on failure run the new_handler or throw. They call malloc,
// so without this every C++ allocation in the game would be credited to
// operator new itself and the report would say nothing at all. Hooked, the
// return address is the code that actually wrote "new".
//
// operator delete needs no such treatment: it is a plain branch to free, which
// the loader already resolves to bully_free.
//
// The one behavioural difference is the failure path. The original walks the
// new_handler chain and throws std::bad_alloc; reproducing that from out here
// means reaching into the game's exception machinery. This is a diagnostic
// build of a port whose whole problem is running out of memory, so a failed
// allocation says so and stops, which is more use than an obscure crash a few
// frames later.
static void *op_new(size_t size, void *return_address) {
  if (size == 0)
    size = 1;
  // __real_malloc, not malloc: going through the wrap layer would record this
  // frame as the caller, and every C++ allocation in the game would be credited
  // to the loader -- which is the exact figure this is trying to measure.
  void *ptr = __real_malloc(size);
  if (!ptr)
    fatal_error("out of memory: the game asked for %d bytes and the heap had none left.\n"
                "This is the leak, caught at the moment it ran out.",
                (int)size);
  remember(ptr, size, return_address);
  return ptr;
}

void *bully_operator_new(size_t size) {
  return op_new(size, __builtin_return_address(0));
}

void *bully_operator_new_array(size_t size) {
  return op_new(size, __builtin_return_address(0));
}

// What a call site really holds. A sampled site has one block in every
// sixty-four in the table, so its recorded bytes are a sixty-fourth of the
// truth -- and ranking on that raw figure put a site holding 19 MB below one
// holding 1 MB. The two biggest sampled sites were the only ones that ever
// surfaced; everything growing sat under a crowd of sites holding nothing.
static size_t weigh(const CallerTotals *c) {
  return (c->caller & SAMPLED_FLAG) ? (size_t)c->live_bytes * SMALL_SAMPLE : c->live_bytes;
}

void alloc_trace_report(void) {
  // The biggest holders, found by a few passes rather than by sorting the
  // table, since this runs inside the game's loop.
  traceLog("heap: %d MB in large blocks, %d MB in small (%u of them), %u untracked frees%s\n",
           (int)(large_live_bytes / (1024 * 1024)), (int)(small_live_bytes / (1024 * 1024)),
           small_live_count, untracked_frees + unknown_frees,
           table_full ? " [TABLE FULL]" : "");

  size_t ceiling_bytes = (size_t)-1;
  uint32_t ceiling_caller = 0;
  for (int rank = 0; rank < 16; rank++) {
    CallerTotals *best = NULL;
    for (int i = 0; i < CALLERS; i++) {
      CallerTotals *c = &callers[i];
      // Eboot sites have their own report; this one is the game's.
      // Ranked on size and then call site, because comparing size alone drops
      // exact ties -- which is why a report listing 36 MB of holders could
      // announce a 68 MB total: the two .obb caches are the same size and the
      // second was never printed.
      if (c->total_count == 0 || c->caller >= EBOOT_TEXT_BASE)
        continue;
      // Skip what has already been printed: anything bigger, or the same size
      // with a call site at or before the last one shown. Comparing the other
      // way round skipped every tie rather than stepping through them, which is
      // how a 68 MB total came to list 36 MB of holders -- the two .obb caches
      // are exactly the same size.
      if (weigh(c) > ceiling_bytes ||
          (weigh(c) == ceiling_bytes && c->caller <= ceiling_caller))
        continue;
      if (!best || weigh(c) > weigh(best) ||
          (weigh(c) == weigh(best) && c->caller < best->caller))
        best = c;
    }
    if (!best || weigh(best) < 512 * 1024)
      break;
    if (best->caller & SAMPLED_FLAG)
      traceLog("heap:   libBully.so+0x%x holds about %d MB in small blocks (1 in %d sampled)\n",
               best->caller & ~SAMPLED_FLAG,
               (int)(best->live_bytes * SMALL_SAMPLE / (1024 * 1024)), SMALL_SAMPLE);
    else
      traceLog("heap:   libBully.so+0x%x holds %d MB in %u blocks (%u ever)\n", best->caller,
               (int)(best->live_bytes / (1024 * 1024)), best->live_count, best->total_count);
    ceiling_bytes = weigh(best);
    ceiling_caller = best->caller;
  }
}

#endif

/*
 * Linker-level accounting
 *
 * Everything above only sees what the game asks for, because those are the
 * allocators the loader resolves for the .so. That left 79 MB of live heap
 * unaccounted for: vitaGL, OpenAL, the movie player and the loader's own code
 * are linked into the eboot and call malloc directly, so none of it came
 * through those wrappers -- and it is the larger share of the leak.
 *
 * --wrap catches all of it. Every reference to malloc anywhere in the eboot --
 * the game's included, since its calls arrive through the loader's exported
 * symbol -- becomes a call to __wrap_malloc, with __real_malloc the genuine
 * article. The build already used --wrap for memcpy, so the mechanism is known
 * to work here.
 *
 * Who asked is decided by the return address: the .so is mapped at
 * LOAD_ADDRESS, so anything at or above that is the game. The game's
 * allocations are counted in bulk -- there are 177000 of them and they are
 * already understood -- while the eboot's are tabled individually whatever
 * their size, because they are far rarer and they are the ones we cannot
 * currently name.
 */

#ifdef LOADER_ALLOC_TRACE

static size_t loader_live_bytes;
static uint32_t loader_live_count;

static int from_the_game(void *return_address) {
  return (uintptr_t)return_address >= LOAD_ADDRESS;
}

// The moment an allocation fails is the moment the game dies, and until now it
// passed in silence: only operator new was watched, and a plain malloc that
// returns NULL just gets used and faults somewhere unrelated a few frames
// later. Every crash today was found by inference from a coredump because of
// this. Report the first handful, with the size and who asked.
static void report_failure(size_t size, void *return_address) {
  static uint32_t reported;
  if (reported >= 8)
    return;
  reported++;
  traceLog("OUT OF MEMORY: %s asked for %d bytes and got nothing (caller %s0x%x)\n",
           from_the_game(return_address) ? "the game" : "the loader", (int)size,
           from_the_game(return_address) ? "libBully.so+" : "eboot ",
           from_the_game(return_address) ? (unsigned)((uintptr_t)return_address - LOAD_ADDRESS)
                                         : (unsigned)(uintptr_t)return_address);
}

static void account_alloc(void *ptr, void *return_address) {
  if (!ptr)
    return;

  if (from_the_game(return_address)) {
    remember(ptr, 0, return_address);
    return;
  }

  size_t size = malloc_usable_size(ptr);
  // Tabled at any size: the eboot allocates orders of magnitude less often than
  // the game, so the lock is uncontended, and a leak made of small blocks needs
  // naming just as much as one made of large ones. Only what reached the table
  // is counted, because only what reached the table can be found again to take
  // back off.
  if (remember_block(ptr, size, (uint32_t)(uintptr_t)return_address)) {
    __atomic_add_fetch(&loader_live_bytes, size, __ATOMIC_RELAXED);
    __atomic_add_fetch(&loader_live_count, 1, __ATOMIC_RELAXED);
  }
}

static void account_free(void *ptr) {
  // The table holds the eboot's blocks and the game's large ones together, so
  // the caller decides whose counters to adjust. Decrementing the eboot's for
  // every tabled block -- which is what this did at first -- ran the count below
  // zero and reported 3725 MB in 4294967204 blocks.
  uint32_t caller = 0;
  size_t reclaimed = forget_block(ptr, &caller);
  if (reclaimed) {
    if (caller & SAMPLED_FLAG) {
      // A sampled block is a small block that also happens to be in the table.
      // Its bytes are counted in the small totals, and this is the only path
      // that frees it -- forget() is never reached from here, which is where
      // the previous attempt at this fix was put. Left as it was, one small
      // block in every sixty-four was counted in and never out, and the small
      // total grew forever: a fabricated leak in the exact figure being
      // investigated.
      __atomic_sub_fetch(&small_live_bytes, reclaimed, __ATOMIC_RELAXED);
      __atomic_sub_fetch(&small_live_count, 1, __ATOMIC_RELAXED);
    } else if (caller >= EBOOT_TEXT_BASE) {
      __atomic_sub_fetch(&loader_live_bytes, reclaimed, __ATOMIC_RELAXED);
      __atomic_sub_fetch(&loader_live_count, 1, __ATOMIC_RELAXED);
    }
    return;
  }
  forget(ptr);
}

void alloc_trace_loader_report(void) {
  traceLog("loader heap: %d MB in %u blocks not allocated by the game\n",
           (int)(loader_live_bytes / (1024 * 1024)), loader_live_count);

  size_t ceiling_bytes = (size_t)-1;
  uint32_t ceiling_caller = 0;
  for (int rank = 0; rank < 16; rank++) {
    CallerTotals *best = NULL;
    for (int i = 0; i < CALLERS; i++) {
      CallerTotals *c = &callers[i];
      if (c->total_count == 0 || c->caller < EBOOT_TEXT_BASE)
        continue;
      // Skip what has already been printed: anything bigger, or the same size
      // with a call site at or before the last one shown. Comparing the other
      // way round skipped every tie rather than stepping through them, which is
      // how a 68 MB total came to list 36 MB of holders -- the two .obb caches
      // are exactly the same size.
      if (weigh(c) > ceiling_bytes ||
          (weigh(c) == ceiling_bytes && c->caller <= ceiling_caller))
        continue;
      if (!best || weigh(c) > weigh(best) ||
          (weigh(c) == weigh(best) && c->caller < best->caller))
        best = c;
    }
    // Down to 64 KB: the three sites over a quarter of a megabyte came to 37 MB
    // of the 155 MB live, so whatever holds the rest is spread thinner than the
    // old threshold could see.
    if (!best || best->live_bytes < 64 * 1024)
      break;
    traceLog("loader heap:   eboot 0x%x holds %d KB in %u blocks (%u ever)\n", best->caller,
             (int)(best->live_bytes / 1024), best->live_count, best->total_count);
    ceiling_bytes = weigh(best);
    ceiling_caller = best->caller;
  }
}

#endif

/*
 * The wrappers themselves are defined whatever the trace setting, because the
 * link always asks for them: --wrap turns every reference to malloc in the
 * eboot into a reference to __wrap_malloc, so a build with the accounting
 * switched off would otherwise fail to link. With it off they are a straight
 * forward to the real allocator.
 */

#ifdef LOADER_ALLOC_TRACE
#define note_failure(size, ra) report_failure(size, ra)
#else
#define note_failure(size, ra) ((void)0)
void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *ptr, size_t size);
void *__real_memalign(size_t alignment, size_t size);
void *__real_aligned_alloc(size_t alignment, size_t size);
void __real_free(void *ptr);
#define account_alloc(ptr, ra) ((void)0)
#define account_free(ptr) ((void)0)
#endif

#ifdef LOADER_ALLOC_TRACE
void alloc_trace_alloc(void *ptr, size_t size, void *caller) {
  (void)size; // the heap's own answer is used, so that in and out agree
  account_alloc(ptr, caller);
}

void alloc_trace_free(void *ptr) {
  account_free(ptr);
}
#endif

void *__wrap_malloc(size_t size) {
  void *ptr = __real_malloc(size);
  if (!ptr)
    note_failure(size, __builtin_return_address(0));
  account_alloc(ptr, __builtin_return_address(0));
  return ptr;
}

void *__wrap_calloc(size_t count, size_t size) {
  void *ptr = __real_calloc(count, size);
  if (!ptr)
    note_failure(count * size, __builtin_return_address(0));
  account_alloc(ptr, __builtin_return_address(0));
  return ptr;
}

void *__wrap_memalign(size_t alignment, size_t size) {
  void *ptr = __real_memalign(alignment, size);
  if (!ptr)
    note_failure(size, __builtin_return_address(0));
  account_alloc(ptr, __builtin_return_address(0));
  return ptr;
}

// OpenAL's al_malloc goes through aligned_alloc, which reaches _memalign_r
// without ever referencing the memalign symbol -- so wrapping memalign alone
// left its allocations invisible while its frees were counted, which is what
// drove the small total below zero.
void *__wrap_aligned_alloc(size_t alignment, size_t size) {
  void *ptr = __real_aligned_alloc(alignment, size);
  if (!ptr)
    note_failure(size, __builtin_return_address(0));
  account_alloc(ptr, __builtin_return_address(0));
  return ptr;
}

void *__wrap_realloc(void *ptr, size_t size) {
  if (ptr)
    account_free(ptr);
  void *out = __real_realloc(ptr, size);
  if (!out && size)
    note_failure(size, __builtin_return_address(0));
  account_alloc(out, __builtin_return_address(0));
  return out;
}

void __wrap_free(void *ptr) {
  if (ptr)
    account_free(ptr);
  __real_free(ptr);
}
