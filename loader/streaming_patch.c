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
#include "texture_cache.h"

// The original refuses anything over this regardless, and the caller copes with
// a refusal by evicting and then giving up gracefully. Keep the behaviour.
#define ORIGINAL_MAX_REQUEST (10 * 1024 * 1024)

static volatile int *ms_memory_used;
static void (*update_memory_used)(void);
static uint32_t call_count, refusal_count, backoff_count;
// Exposed so the trace can tell "the gate stopped refusing" from "the game
// stopped asking". A whole session read 160 refusals from start to finish and
// there was no way to tell which of the two had happened. It was the second.
static uint32_t backoff_until_frame;
static size_t budget;
static int installed;

// How much the game is holding.
//
// A correction, because the reasoning that used to be written here was wrong
// and it closed off the most promising route in the port.
//
// It said the game sub-allocates from an arena, so that freeing a model returns
// its memory to a free list inside the game and never reaches newlib. That is
// false for this binary. MemoryMgrMalloc branches straight to memalign@plt and
// MemoryMgrFree branches straight to free@plt; CMemoryHeap::Init has no callers
// anywhere in the .text. There is no arena, and making the engine free a model
// does return that memory to the heap.
//
// What actually went wrong on hardware -- 24389 refusals with the heap at a
// flat 179 MB -- was the policy, not the gauge: an absolute threshold below the
// figure the game runs at, so the answer was "no" for ever and it evicted the
// world trying to satisfy it.
//
// CStreaming::UpdateMemoryUsed sums CMemoryHeap::GetMemoryUsed over the
// streaming memory IDs, which is exactly the figure that moves when a model is
// freed. Nothing calls it -- the Android build stopped maintaining the
// accounting when it stubbed the memory check -- but it is still there and
// still correct, so the loader calls it.
static size_t streamer_used(void) {
  // The newlib heap, which is the thing that actually runs out, sampled once a
  // frame by the texture cache and read here for free.
  //
  // CStreaming::ms_memoryUsed was the obvious choice and it is useless: nothing
  // in the binary calls UpdateMemoryUsed, so the accounting behind it stopped
  // being maintained when the Android build stubbed the memory check, and
  // driving it by hand reported 1 MB of a 156 MB heap. The gate never fired
  // once in a whole session.
  //
  // The heap is the right measure here for a reason that was, until today,
  // written down backwards: MemoryMgrMalloc branches to memalign@plt and
  // MemoryMgrFree to free@plt, with CMemoryHeap::Init called from nowhere at
  // all. The engine has no arena. When a model is freed, the memory goes back
  // to newlib, so the heap figure moves and refusing has somewhere to go.
  return texture_cache_heap_used();
}

// The budget, taken from what the game turns out to need rather than from a
// number picked in advance. One area's worth is whatever it is holding once it
// has settled into play; the margin is what it may grow to beyond that before
// being asked to let go of something.
static void calibrate(size_t used) {
  if (budget || frames_swapped < STREAMING_CALIBRATE_FRAMES || used == 0)
    return;

  // Measured, not chosen. Twice now a figure picked in advance has landed below
  // what the game needs to run, and the game answered by evicting until the
  // level itself was gone. Starting from what it is actually holding once it is
  // in play cannot do that: the budget is above the floor by construction.
  budget = used + (size_t)STREAMING_BUDGET_MARGIN_MB * 1024 * 1024;

  // And never past what the heap can give, or the gate would be asking for
  // room that does not exist by the time it matters.
  size_t ceiling = (size_t)(MEMORY_NEWLIB_MB - STREAMING_HEAP_KEEP_FREE_MB) * 1024 * 1024;
  if (budget > ceiling)
    budget = ceiling;

  traceLog("streaming: settled at %d MB of heap, budget %d MB\n",
           (int)(used / (1024 * 1024)), (int)(budget / (1024 * 1024)));
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
// Frames, now, rather than calls -- so this is how long the gate insists before
// accepting that the game has nothing more it can usefully give up.
#define REFUSALS_WITHOUT_PROGRESS 8
#define PROGRESS_BYTES (256 * 1024)
// Frames, and this is the correction. It used to be 4096 calls, "deliberately:
// long enough to load an area" -- and the new call counter shows what that
// actually bought. Over a session of 3.9 million frames the game asked this
// question 2084 times in total, and the backoff still had 2950 calls left to
// run when the session ended. One backoff turned the gate off for longer than a
// whole session, which is exactly what the previous trace was showing when it
// read a flat 160 refusals for 7.8 million frames and there was no way to tell
// whether the gate had stopped refusing or the game had stopped asking.
//
// Frames cannot do that: the game's request rate has no bearing on how long the
// gate stays out of the way, and this is thirty seconds at thirty frames a
// second -- long enough to load an area, short enough to be temporary.
#define BACKOFF_FRAMES 900

static int should_refuse(void) {
  static size_t used_at_last_progress;
  static uint32_t fruitless;
  static int judged_on_frame;

  size_t used = streamer_used();
  calibrate(used);
  if (!budget)
    return 0; // not calibrated yet: behave as the game always has

  size_t limit = budget;

  if (used <= limit) {
    // Under the limit: nothing to do, and the next time we are over it we start
    // judging progress afresh.
    used_at_last_progress = 0;
    fruitless = 0;
    return 0;
  }

  // Backing off after a fruitless run, so the game can put the world back.
  if (backoff_until_frame) {
    if ((uint32_t)frames_swapped < backoff_until_frame)
      return 0;
    backoff_until_frame = 0;
  }

  if (!used_at_last_progress || used + PROGRESS_BYTES < used_at_last_progress) {
    used_at_last_progress = used; // evicting is freeing something; keep going
    fruitless = 0;
    return 1;
  }

  // Progress is judged once a frame, not once a call. The heap figure is
  // sampled in the tick, so every call within a frame reads the same number:
  // counting each of them as a failure would exhaust the allowance inside a
  // single frame, on evidence that had no chance to change, and the gate would
  // spend its life backed off and do nothing at all.
  if (judged_on_frame == frames_swapped)
    return 1;
  judged_on_frame = frames_swapped;

  if (++fruitless >= REFUSALS_WITHOUT_PROGRESS) {
    // Refusing has stopped helping. Let the game load what it needs.
    fruitless = 0;
    used_at_last_progress = 0;
    backoff_until_frame = (uint32_t)frames_swapped + BACKOFF_FRAMES;
    backoff_count++;
    return 0;
  }

  return 1;
}

// The honest answer to the question the game is asking.
static int IsThereEnoughFreeMemory(int size) {
  call_count++;
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
  update_memory_used =
      (void (*)(void))so_symbol(&bully_mod, "_ZN10CStreaming16UpdateMemoryUsedEv");
  if (!ms_memory_used || !update_memory_used) {
    traceLog("streaming: no memory accounting to read, leaving the gate alone\n");
    return;
  }

  hook_addr(gate, (uintptr_t)&IsThereEnoughFreeMemory);
  installed = 1;
  traceLog("streaming: memory gate hooked, budget set after %d frames\n",
           STREAMING_CALIBRATE_FRAMES);
}

void streaming_patch_stats(StreamingStats *out) {
  out->memory_used_mb = (int)(streamer_used() / (1024 * 1024));
  out->budget_mb = (int)(budget / (1024 * 1024));
  out->calls = (int)call_count;
  out->refusals = (int)refusal_count;
  out->backoffs = (int)backoff_count;
  out->backoff_left = backoff_until_frame > (uint32_t)frames_swapped
                          ? (int)(backoff_until_frame - (uint32_t)frames_swapped)
                          : 0;
  out->installed = installed;
}
