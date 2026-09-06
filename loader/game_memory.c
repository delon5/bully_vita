/* game_memory.c -- the game's own memory accounting, read back out of it.
 *
 * Every session so far has ended the same way: the newlib heap fills, an
 * allocation fails, and the trace can say how much memory went but not what
 * took it. The allocation tracer answers that, but it takes a spinlock on every
 * malloc across four threads and turns area loads into a stutter, so it cannot
 * be left on while anyone is actually playing.
 *
 * It turns out not to be needed. The engine keeps the accounting itself:
 *
 *   int CMemoryHeap::GetMemoryUsed(int id)  { return allocedBytes[id]; }
 *   int CMemoryHeap::GetBlocksUsed(int id)  { return allocedBlocks[id]; }
 *
 * both reading a 64-entry table in .bss, maintained by MallocWithMemID and
 * Free, and both exported by name in the dynamic symbol table. Reading them
 * costs 128 word loads and no lock at all, and it breaks the heap down by the
 * game's own categories rather than by return address -- which is the thing
 * that was actually wanted. CStreaming::UpdateMemoryUsed sums ids 8, 9, 10, 13,
 * 15, 18, 19 and 20 into ms_memoryUsed, so those eight are the streamed data;
 * the rest are everything else the engine tags.
 *
 * There are no names for the ids in the binary -- nothing in .rodata indexes
 * them -- so they are reported by number, with the streamed ones marked.
 *
 * The other half of this file is what happens when the heap does run out. The
 * crash that closes each session is not a graceful one:
 *
 *   ReadBuffer::RequestData(unsigned int, int):
 *     ...
 *     movs r0, #8                 ; alignment
 *     blx  memalign@plt           ; capacity * 21/13 + 7
 *     ...
 *     mov  sb, r0                 ; NULL, unchecked
 *     blx  memcpy@plt             ; into NULL + 4
 *     str.w r3, [sb]              ; <- *(int *)0 = 1
 *
 * so a failed allocation is a null dereference two instructions later, with no
 * chance for anything to recover. The last coredump was exactly this: pc at
 * 0046f6aa with r9 zero, asking for 295356 bytes against a heap with 11 MB free
 * and 16 KB of it contiguous.
 *
 * The heap was not empty. It was fragmented -- arena 159 MB of a 160 MB cap,
 * live 148 MB, and no single hole big enough. So the loader keeps one block
 * back at startup and hands it over the first time the game cannot get what it
 * asked for. A reserve that has been held since boot is contiguous by
 * construction, which is the property the free list had run out of, and giving
 * it up turns the first failure into a stall instead of a crash.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alloc_trace.h"
#include "config.h"
#include "game_memory.h"
#include "main.h"
#include "so_util.h"

// GetMemoryUsed indexes the table with no bounds check, so its size is only
// visible from the symbol table: allocedBytes and allocedBlocks are 0x100 bytes
// each, which is 64 categories.
#define MEM_CATEGORIES 64

static const volatile uint32_t *alloced_bytes;
static const volatile uint32_t *alloced_blocks;
static const volatile int *streaming_used;
static const volatile int *txd_loaded;
static const volatile int *texture_heap_used;
static void (*update_memory_used)(void);

static uint32_t previous_bytes[MEM_CATEGORIES];
static int have_previous;

// The ids CStreaming::UpdateMemoryUsed adds together. Marked in the report
// because they are the ones the streaming gate is supposed to be able to move:
// growth in these is the streamer holding on, growth anywhere else is not.
static int is_streamed_category(int id) {
  switch (id) {
  case 8: case 9: case 10: case 13: case 15: case 18: case 19: case 20:
    return 1;
  default:
    return 0;
  }
}

/*
 * The rescue reserve
 */

// The genuine allocators. --wrap redirects every reference to malloc in the
// eboot to __wrap_malloc, which decides whose allocation it is from the return
// address -- and from in here the return address is the loader, not the game.
// So these call past the wrap layer and hand the game's caller to the
// accounting themselves.
void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *ptr, size_t size);
void *__real_memalign(size_t alignment, size_t size);
void __real_free(void *ptr);

static void *reserve;
static uint32_t failures;      // allocations the heap could not satisfy
static uint32_t rescued;       // ...of which the reserve saved
static size_t largest_failure; // the biggest one asked for

static void reserve_take(void) {
  if (MEMORY_RESCUE_RESERVE_MB <= 0)
    return;
  reserve = __real_memalign(16, (size_t)MEMORY_RESCUE_RESERVE_MB * 1024 * 1024);
  if (!reserve)
    traceLog("gamemem: could not set aside a %d MB rescue reserve\n",
             MEMORY_RESCUE_RESERVE_MB);
}

// Called from every allocator the game uses, on the way out of a failure.
//
// Reports first, because a line naming the size and the caller is worth more
// than the rescue: it turns the next one of these from a coredump to read into
// a fact already written down. The caller is the return address, given as an
// offset into libBully.so so tools/addr2sym.py can name it.
static void report_allocation_failure(size_t size, size_t alignment, void *caller) {
  failures++;

  // The report below writes to the card, and writing to the card can itself
  // want memory. One failure reporting another would recurse until the stack
  // went, which is a worse ending than the one being reported.
  static volatile int reporting;
  if (reporting)
    return;
  reporting = 1;
  if (size > largest_failure)
    largest_failure = size;

  struct mallinfo heap = mallinfo();
  uintptr_t ra = (uintptr_t)caller;
  if (ra >= LOAD_ADDRESS)
    traceLog("gamemem: OUT OF MEMORY -- %u bytes aligned %u for libBully.so+0x%x; "
             "arena %d MB, live %d MB, free-listed %d MB, largest hole %d KB\n",
             (unsigned)size, (unsigned)alignment, (unsigned)(ra - LOAD_ADDRESS),
             (int)(heap.arena / (1024 * 1024)), (int)(heap.uordblks / (1024 * 1024)),
             (int)(heap.fordblks / (1024 * 1024)), (int)(heap.keepcost / 1024));
  else
    traceLog("gamemem: OUT OF MEMORY -- %u bytes aligned %u for eboot 0x%08x; "
             "arena %d MB, live %d MB, free-listed %d MB, largest hole %d KB\n",
             (unsigned)size, (unsigned)alignment, (unsigned)ra,
             (int)(heap.arena / (1024 * 1024)), (int)(heap.uordblks / (1024 * 1024)),
             (int)(heap.fordblks / (1024 * 1024)), (int)(heap.keepcost / 1024));

  // Whatever the game was holding at the moment it ran out, by category. This
  // is the reading that matters most and the one that has never been taken.
  game_memory_report();
  reporting = 0;
}

// Gives the reserve up, once. There is nothing else safe to free from here:
// the texture cache's parked copies and the vertex cache's staging buffers both
// belong to the render thread, and this runs on whichever thread happened to
// ask for memory. The reserve belongs to nobody.
static int release_reserve(void) {
  if (!reserve)
    return 0;
  void *block = reserve;
  reserve = NULL;
  __real_free(block);
  rescued++;
  traceLog("gamemem: handed over the %d MB rescue reserve; the heap is now on its own\n",
           MEMORY_RESCUE_RESERVE_MB);
  return 1;
}

void *game_malloc(size_t size) {
  void *caller = __builtin_return_address(0);
  void *p = __real_malloc(size);
  if (!p) {
    report_allocation_failure(size, 0, caller);
    if (release_reserve())
      p = __real_malloc(size);
  }
  alloc_trace_alloc(p, size, caller);
  return p;
}

void *game_calloc(size_t count, size_t size) {
  void *caller = __builtin_return_address(0);
  void *p = __real_calloc(count, size);
  if (!p) {
    report_allocation_failure(count * size, 0, caller);
    if (release_reserve())
      p = __real_calloc(count, size);
  }
  alloc_trace_alloc(p, count * size, caller);
  return p;
}

void *game_realloc(void *ptr, size_t size) {
  void *caller = __builtin_return_address(0);
  if (ptr)
    alloc_trace_free(ptr);
  void *p = __real_realloc(ptr, size);
  if (!p && size) {
    report_allocation_failure(size, 0, caller);
    // realloc leaves the original block alone when it fails, so retrying is
    // safe and the game's pointer is still good either way.
    if (release_reserve())
      p = __real_realloc(ptr, size);
  }
  alloc_trace_alloc(p ? p : ptr, size, caller);
  return p;
}

void *game_memalign(size_t alignment, size_t size) {
  void *caller = __builtin_return_address(0);
  void *p = __real_memalign(alignment, size);
  if (!p) {
    report_allocation_failure(size, alignment, caller);
    if (release_reserve())
      p = __real_memalign(alignment, size);
  }
  alloc_trace_alloc(p, size, caller);
  return p;
}

/*
 * Reporting
 */

void game_memory_init(void) {
  alloced_bytes = (const volatile uint32_t *)so_symbol(&bully_mod, "allocedBytes");
  alloced_blocks = (const volatile uint32_t *)so_symbol(&bully_mod, "allocedBlocks");
  streaming_used = (const volatile int *)so_symbol(&bully_mod, "_ZN10CStreaming13ms_memoryUsedE");
  txd_loaded = (const volatile int *)so_symbol(
      &bully_mod, "_ZN9CTxdStore32ms_totalTXDMemoryCurrentlyLoadedE");
  texture_heap_used = (const volatile int *)so_symbol(
      &bully_mod, "_ZN17TextureHeapHelper27cachedUsedTextureMemorySizeE");
  update_memory_used =
      (void (*)(void))so_symbol(&bully_mod, "_ZN10CStreaming16UpdateMemoryUsedEv");

  reserve_take();

  if (!alloced_bytes || !alloced_blocks) {
    traceLog("gamemem: the engine's accounting tables are not exported, "
             "reporting the heap only\n");
    return;
  }
  traceLog("gamemem: reading the engine's own accounting, %d categories, "
           "%d MB held back for a failed allocation\n",
           MEM_CATEGORIES, MEMORY_RESCUE_RESERVE_MB);
}

void game_memory_report(void) {
  if (!alloced_bytes || !alloced_blocks)
    return;

  // Nothing maintains ms_memoryUsed in this build -- the Android port stopped
  // calling UpdateMemoryUsed when it stubbed the memory check -- so drive it
  // here. It is a read of eight table entries and one store.
  if (update_memory_used)
    update_memory_used();

  char used[512];
  char moved[512];
  int u = 0, m = 0;
  uint32_t total = 0, blocks = 0;

  for (int id = 0; id < MEM_CATEGORIES; id++) {
    uint32_t bytes = alloced_bytes[id];
    uint32_t count = alloced_blocks[id];
    total += bytes;
    blocks += count;

    if (bytes && u < (int)sizeof(used) - 32)
      u += snprintf(used + u, sizeof(used) - u, " %d%s=%uK/%u", id,
                    is_streamed_category(id) ? "*" : "", (unsigned)(bytes / 1024),
                    (unsigned)count);

    if (have_previous && m < (int)sizeof(moved) - 32) {
      int32_t delta = (int32_t)bytes - (int32_t)previous_bytes[id];
      if (delta >= GAME_MEMORY_DELTA_KB * 1024 || delta <= -GAME_MEMORY_DELTA_KB * 1024)
        m += snprintf(moved + m, sizeof(moved) - m, " %d%s%+dK", id,
                      is_streamed_category(id) ? "*" : "", (int)(delta / 1024));
    }
    previous_bytes[id] = bytes;
  }
  have_previous = 1;

  traceLog("gamemem: %d MB in %u blocks, streamed %d MB, txd %d MB, texheap %d MB |%s\n",
           (int)(total / (1024 * 1024)), (unsigned)blocks,
           streaming_used ? (int)(*streaming_used / (1024 * 1024)) : -1,
           txd_loaded ? (int)(*txd_loaded / (1024 * 1024)) : -1,
           texture_heap_used ? (int)(*texture_heap_used / (1024 * 1024)) : -1,
           u ? used : " nothing tagged");
  if (m)
    traceLog("gamemem: moved |%s\n", moved);
  if (failures)
    traceLog("gamemem: %u allocations refused, largest %u bytes, reserve %s\n",
             (unsigned)failures, (unsigned)largest_failure,
             reserve ? "still held" : (rescued ? "spent" : "never taken"));
}
