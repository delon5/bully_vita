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

#include "main.h"
#include "config.h"
#include "alloc_trace.h"
#include "so_util.h"

#ifdef LOADER_ALLOC_TRACE

// Live allocations, open addressed by pointer. Sized well past what the game
// keeps live so lookups stay short; a full table would silently stop
// accounting, so overflow is counted and reported rather than ignored.
#define LIVE_SLOTS 262144
#define LIVE_MASK (LIVE_SLOTS - 1)

// Allocations smaller than this are counted in bulk rather than individually.
// A hundred thousand small allocations are a fact of life in a game this size,
// and the question here is where megabytes go.
#define SMALL_BYTES 1024

// Distinct call sites worth remembering. The game has far fewer places that
// allocate large blocks than it has allocations.
#define CALLERS 512

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
  uint32_t i = (caller * 0x9e3779b1u) >> 23;
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

static void remember(void *ptr, size_t size, void *return_address) {
  if (!ptr)
    return;
  // What the heap actually gave up, not what was asked for, so that these
  // figures can be compared with mallinfo and so that a block is classified the
  // same way when it is freed as when it was taken.
  (void)size;
  size = malloc_usable_size(ptr);

  lock_table();
  if (size < SMALL_BYTES) {
    small_live_bytes += size;
    small_live_count++;
    unlock_table();
    return;
  }

  uint32_t caller = so_offset(return_address);
  uint32_t i = hash_ptr(ptr) & LIVE_MASK;
  for (uint32_t probe = 0; probe < 64; probe++) {
    LiveBlock *b = &live[(i + probe) & LIVE_MASK];
    if (!b->ptr) {
      b->ptr = ptr;
      b->size = (uint32_t)size;
      b->caller = caller;
      large_live_bytes += size;
      CallerTotals *c = caller_slot(caller);
      if (c) {
        c->live_bytes += (uint32_t)size;
        c->live_count++;
        c->total_count++;
      }
      unlock_table();
      return;
    }
  }
  table_full++;
  unlock_table();
}

static void forget(void *ptr) {
  if (!ptr)
    return;

  // Small blocks are most of the traffic and are not in the table, so settle
  // that first rather than probing for something that was never put there.
  size_t size = malloc_usable_size(ptr);
  lock_table();
  if (size < SMALL_BYTES) {
    if (small_live_count) {
      small_live_bytes -= small_live_bytes < size ? small_live_bytes : size;
      small_live_count--;
    } else {
      untracked_frees++;
    }
    unlock_table();
    return;
  }

  uint32_t i = hash_ptr(ptr) & LIVE_MASK;
  for (uint32_t probe = 0; probe < 64; probe++) {
    LiveBlock *b = &live[(i + probe) & LIVE_MASK];
    if (b->ptr == ptr) {
      large_live_bytes -= b->size;
      CallerTotals *c = caller_slot(b->caller);
      if (c && c->caller == b->caller) {
        c->live_bytes -= b->size;
        c->live_count--;
      }
      b->ptr = NULL;
      b->size = 0;
      b->caller = 0;
      unlock_table();
      return;
    }
    if (!b->ptr)
      break; // the run ended, so it was never a tracked block
  }
  untracked_frees++; // allocated before tracking began, or lost to a full table
  unlock_table();
}

void *bully_malloc(size_t size) {
  void *ptr = malloc(size);
  remember(ptr, size, __builtin_return_address(0));
  return ptr;
}

void *bully_calloc(size_t count, size_t size) {
  void *ptr = calloc(count, size);
  remember(ptr, count * size, __builtin_return_address(0));
  return ptr;
}

void *bully_realloc(void *ptr, size_t size) {
  if (ptr)
    forget(ptr);
  void *out = realloc(ptr, size);
  remember(out, size, __builtin_return_address(0));
  return out;
}

void *bully_memalign(size_t alignment, size_t size) {
  void *ptr = memalign(alignment, size);
  remember(ptr, size, __builtin_return_address(0));
  return ptr;
}

void bully_free(void *ptr) {
  forget(ptr);
  free(ptr);
}

void alloc_trace_report(void) {
  // The biggest holders, found by a few passes rather than by sorting the
  // table, since this runs inside the game's loop.
  traceLog("heap: %d MB in large blocks, %d MB in small (%u of them), %u untracked frees%s\n",
           (int)(large_live_bytes / (1024 * 1024)), (int)(small_live_bytes / (1024 * 1024)),
           small_live_count, untracked_frees, table_full ? " [TABLE FULL]" : "");

  uint32_t ceiling = 0xffffffffu;
  for (int rank = 0; rank < 8; rank++) {
    CallerTotals *best = NULL;
    for (int i = 0; i < CALLERS; i++) {
      CallerTotals *c = &callers[i];
      if (c->total_count == 0 || c->live_bytes >= ceiling)
        continue;
      if (!best || c->live_bytes > best->live_bytes)
        best = c;
    }
    if (!best || best->live_bytes < 512 * 1024)
      break;
    traceLog("heap:   libBully.so+0x%x holds %d MB in %u blocks (%u ever)\n", best->caller,
             (int)(best->live_bytes / (1024 * 1024)), best->live_count, best->total_count);
    ceiling = best->live_bytes;
  }
}

#endif
