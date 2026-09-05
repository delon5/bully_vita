/* streaming_patch.c -- make the game's own streamer free what it loads.
 *
 * This is the documented limitation, and this is where it lives:
 *
 *   CStreaming::IsThereEnoughFreeMemory(int size):
 *     cmp.w r0, #10485760   ; 0xa00000
 *     ite   gt
 *     movgt r0, #0
 *     movle r0, #1
 *     bx    lr
 *
 * It never looks at memory. It compares the requested size against a hardcoded
 * 10 MB and answers "yes, plenty" to anything smaller -- which is every model,
 * texture dictionary and collision mesh the game will ever stream. On a phone
 * with gigabytes to spare that is a reasonable thing to have done. Here it
 * means the streamer never frees anything, and the heap climbs until malloc
 * fails: 70 MB at startup to 194 MB of 208 over a session, ending in _malloc_r.
 *
 * The machinery to free things is all present and works. Its only caller is:
 *
 *   MakeSpaceForMemoryObject(a, b):
 *     if (IsObjectStaticallyStreamed(b)) return;
 *     while (!IsThereEnoughFreeMemoryForObject(a, b))
 *       if (!RemoveLeastUsedModel(32))
 *         return DeleteRwObjectsBehindCamera(a, b);
 *
 * so a truthful answer is all it takes to turn the game's own least-recently-
 * used eviction back on. Note that the loop terminates on its own when there is
 * nothing left to remove -- RemoveLeastUsedModel returning 0 falls out to
 * DeleteRwObjectsBehindCamera and returns -- so refusing every time cannot hang
 * it, however tight memory gets.
 *
 * Freeing through the game's own path is worth a great deal: it releases models
 * with their textures, collision and animation together, updates the streaming
 * info so they are re-requested when needed, and leaves no pointer to something
 * that no longer exists.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <malloc.h>
#include <stdint.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "streaming_patch.h"

// The original refuses anything over this regardless, and the caller copes with
// a refusal by evicting and then giving up gracefully. Keep the behaviour.
#define ORIGINAL_MAX_REQUEST (10 * 1024 * 1024)

static volatile int *ms_memory_used;
static uint32_t refusal_count;
static int installed;

// mallinfo walks the free list, and this gate is called for every object the
// streamer considers -- thousands of times through an area load. The heap does
// not move meaningfully between two of those, so sample it now and then.
static int heap_is_full(void) {
  static uint32_t calls;
  static int full;

  if ((calls++ & 31) == 0) {
    struct mallinfo info = mallinfo();
    size_t limit = (size_t)(MEMORY_NEWLIB_MB - STREAMING_HEAP_KEEP_FREE_MB) * 1024 * 1024;
    full = (size_t)info.uordblks > limit;
  }
  return full;
}

// The honest answer to the question the game is asking.
static int IsThereEnoughFreeMemory(int size) {
  if (size > ORIGINAL_MAX_REQUEST)
    return 0;

  if (heap_is_full()) {
    refusal_count++;
    return 0; // go and free something; you have the machinery for it
  }

  return 1;
}

void streaming_patch_init(void) {
  uintptr_t gate = so_symbol(&bully_mod, "_ZN10CStreaming23IsThereEnoughFreeMemoryEi");
  if (!gate) {
    // Not fatal: without it the game behaves as it always has, which is to say
    // it runs and eventually runs out of memory.
    traceLog("streaming: could not find CStreaming::IsThereEnoughFreeMemory, "
             "the game will not free streamed data\n");
    return;
  }

  ms_memory_used = (volatile int *)so_symbol(&bully_mod, "_ZN10CStreaming13ms_memoryUsedE");
  hook_addr(gate, (uintptr_t)&IsThereEnoughFreeMemory);
  installed = 1;
  traceLog("streaming: memory gate hooked, streamer capped at %d MB of heap\n",
           MEMORY_NEWLIB_MB - STREAMING_HEAP_KEEP_FREE_MB);
}

void streaming_patch_stats(StreamingStats *out) {
  out->memory_used_mb = ms_memory_used ? (int)(*ms_memory_used / (1024 * 1024)) : -1;
  out->refusals = (int)refusal_count;
  out->installed = installed;
}
