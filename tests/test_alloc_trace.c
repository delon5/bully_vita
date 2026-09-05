/* test_alloc_trace.c -- the allocation accounting, on the host.
 *
 * This existed nowhere, and it shows: the tracer has shipped four separate
 * defects to hardware -- sampled blocks counted in but never out, the eboot's
 * blocks counted against the game's, a tie-break that dropped every tie, and a
 * fallback that decrements counters for blocks it never counted. Each was found
 * by a person playing the game for twenty minutes and sending back a log.
 *
 * Every one of them is a violation of an invariant that can be checked in a
 * millisecond here: allocate a mixture, free it all, and the counters must
 * return to zero without passing through a negative on the way.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_alloc_env.h"

// Return addresses to attribute allocations to. The tracer decides whose an
// allocation is by where the caller was: at or above LOAD_ADDRESS is the game,
// below it is the loader.
#define GAME_SITE(n) ((void *)(uintptr_t)(LOAD_ADDRESS + 0x1000 + (n) * 0x40))
#define EBOOT_SITE(n) ((void *)(uintptr_t)(0x81000000u + 0x2000 + (n) * 0x40))

#define SMALL 200
#define LARGE (96 * 1024)

static void *slots[40000];

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);

  // Small game blocks, freed again. Sampled or not, the counters have to come
  // back to where they started -- this is the one that reported 4072 MB.
  {
    for (int i = 0; i < 20000; i++)
      slots[i] = trace_alloc(SMALL, GAME_SITE(i % 8));
    for (int i = 0; i < 20000; i++)
      trace_free(slots[i]);
    assert(small_live_bytes == 0 && "small bytes must return to zero");
    assert(small_live_count == 0 && "small count must return to zero");
    printf("game small   : 20000 allocated and freed, counters back to zero  OK\n");
  }

  // The eboot's blocks are the game's neither in the small totals nor the
  // large: it has its own. The game's large figure used to open at 68 MB in
  // the first report of a session, before the game had allocated anything.
  {
    size_t small_before = small_live_bytes, large_before = large_live_bytes;
    for (int i = 0; i < 500; i++)
      slots[i] = trace_alloc(SMALL, EBOOT_SITE(i % 4));
    assert(small_live_bytes == small_before && "eboot blocks are not the game's small ones");
    assert(large_live_bytes == large_before && "nor its large ones");
    assert(loader_live_bytes > 0 && "the eboot has its own total");
    for (int i = 0; i < 500; i++)
      trace_free(slots[i]);
    assert(loader_live_bytes == 0 && "and it must come back to zero too");
    printf("eboot blocks : kept apart from the game's, and balanced        OK\n");
  }

  // Enough live blocks to fill the table, then free them all. A block the table
  // could not hold must not be unaccounted for on the way out: that is what
  // ran the small total below zero on hardware once the table filled.
  {
    int n = 0;
    for (; n < 30000; n++) {
      slots[n] = trace_alloc(n % 3 ? SMALL : LARGE, n % 2 ? GAME_SITE(n % 16) : EBOOT_SITE(n % 16));
      if (!slots[n])
        break;
    }
    for (int i = 0; i < n; i++)
      trace_free(slots[i]);
    printf("table full   : %d blocks through a %d slot table, %u overflowed\n", n, LIVE_SLOTS,
           table_full);
    assert(small_live_bytes == 0 && "a full table must not corrupt the small total");
    assert(large_live_bytes == 0 && "nor the large one");
    assert(loader_live_bytes == 0 && "nor the eboot's");
    printf("             : every counter back to zero regardless           OK\n");
  }

  // Churn: allocate and free repeatedly, which is what a game does. Nothing may
  // accumulate -- the sampler counted one block in sixty-four in and never out,
  // and churn alone drew a growth curve that looked exactly like the leak.
  {
    for (int round = 0; round < 40; round++) {
      for (int i = 0; i < 2000; i++)
        slots[i] = trace_alloc(SMALL, GAME_SITE(i % 8));
      for (int i = 0; i < 2000; i++)
        trace_free(slots[i]);
    }
    assert(small_live_bytes == 0 && "churn must not accumulate");
    assert(small_live_count == 0);
    printf("churn        : 80000 allocations through, nothing left behind   OK\n");
  }

  // Equal holders must all be reported. Two .obb caches of the same size, and
  // only one was ever printed, so a 68 MB total listed 36 MB of holders.
  {
    for (int i = 0; i < 4; i++)
      slots[i] = trace_alloc(LARGE, EBOOT_SITE(100 + i));
    int listed = count_reported_loader_sites();
    printf("equal sizes  : %d holders of identical size, %d listed          %s\n", 4, listed,
           listed == 4 ? "OK" : "FAIL");
    assert(listed == 4 && "equal-sized holders must not be dropped from the report");
    for (int i = 0; i < 4; i++)
      trace_free(slots[i]);
  }

  // Freeing a block the tracer never saw. OpenAL allocates through
  // aligned_alloc, which reaches the allocator without naming the memalign
  // symbol, so --wrap could not see it -- while its frees came straight
  // through. Every one subtracted bytes that had never been added, and after a
  // session of voices and buffers the small total wrapped past zero and printed
  // as 4007 MB. The suite passed anyway, because it only ever freed what it had
  // allocated.
  {
    size_t bytes_before = small_live_bytes;
    for (int i = 0; i < 5000; i++) {
      void *stranger = malloc(SMALL); // never through trace_alloc
      trace_free(stranger);
    }
    assert(small_live_bytes <= bytes_before && "an unknown free must not add bytes");
    assert(small_live_bytes != (size_t)-1 && small_live_bytes < (size_t)1 << 40 &&
           "and must never wrap past zero");
    printf("unknown free : 5000 blocks the tracer never saw, no underflow     OK\n");
  }

  printf("PASS\n");
  return 0;
}
