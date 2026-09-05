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

#include <psp2/io/stat.h>

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
static uint32_t refusal_count, backoff_count;
static int installed;

// The heap, sampled. mallinfo walks the free list and this gate is consulted
// thousands of times through an area load, so it is read now and then rather
// than per call.
static size_t heap_used(void) {
  static uint32_t calls;
  static size_t cached;

  if ((calls++ & 31) == 0) {
    struct mallinfo info = mallinfo();
    cached = (size_t)info.uordblks;
  }
  return cached;
}

// Saying "no" makes the game evict its least recently used model and ask again.
// That is only a sane thing to say while it is still getting somewhere: if the
// heap stops falling and we keep refusing, the game keeps evicting -- and what
// it evicts next is the world you are standing in. On hardware that emptied a
// school corridor to a black void with the characters still walking around in
// it, because the limit was below what the game could ever reach and so every
// answer was "no", for ever.
//
// So refuse only while refusing is working. Once a run of refusals has failed
// to bring the heap down, the streamer has nothing useful left to give up, and
// the honest answer becomes yes -- a game that is short of memory beats a game
// with no scenery.
#define REFUSALS_WITHOUT_PROGRESS 8
#define PROGRESS_BYTES (256 * 1024)
#define BACKOFF_CALLS 4096

static int should_refuse(void) {
  static size_t heap_at_last_progress;
  static uint32_t fruitless;
  static uint32_t backoff;

  size_t used = heap_used();
  size_t limit = (size_t)(MEMORY_NEWLIB_MB - STREAMING_HEAP_KEEP_FREE_MB) * 1024 * 1024;

  if (used <= limit) {
    // Under the limit: nothing to do, and the next time we are over it we start
    // judging progress afresh.
    heap_at_last_progress = 0;
    fruitless = 0;
    return 0;
  }

  // Backing off after a fruitless run, so the game can put the world back.
  if (backoff) {
    backoff--;
    return 0;
  }

  if (!heap_at_last_progress || used + PROGRESS_BYTES < heap_at_last_progress) {
    heap_at_last_progress = used; // evicting is freeing something; keep going
    fruitless = 0;
    return 1;
  }

  if (++fruitless >= REFUSALS_WITHOUT_PROGRESS) {
    // Refusing has stopped helping. Let the game load what it needs.
    fruitless = 0;
    heap_at_last_progress = 0;
    backoff = BACKOFF_CALLS;
    backoff_count++;
    return 0;
  }

  return 1;
}

// The honest answer to the question the game is asking.
static int IsThereEnoughFreeMemory(int size) {
  if (size > ORIGINAL_MAX_REQUEST)
    return 0;

  if (should_refuse()) {
    refusal_count++;
    return 0; // go and free something; you have the machinery for it
  }

  return 1;
}

void streaming_patch_init(void) {
  SceIoStat stat;
  if (sceIoGetstat(STREAMING_DISABLE_PATH, &stat) >= 0) {
    traceLog("streaming: disabled by %s\n", STREAMING_DISABLE_PATH);
    return;
  }

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
  out->memory_used_mb = (int)(heap_used() / (1024 * 1024));
  out->refusals = (int)refusal_count;
  out->backoffs = (int)backoff_count;
  out->installed = installed;
}
