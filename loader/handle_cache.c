/* handle_cache.c -- keep the game's files open instead of reopening them
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * Measured, not guessed. The game opens 17667 files in a session and 12379 of
 * those are a path it has already opened; opening costs 54.5 s against 76.5 s
 * of reading, and inside an area-load freeze the opens cost more than the
 * reads do. Splitting the timing settled the question the read cache got
 * wrong:
 *
 *              first open   repeat open
 *   frozen       3.60 ms      3.54 ms
 *   boot         3.33 ms      3.03 ms
 *   playing      3.60 ms      2.61 ms
 *
 * A repeat costs what a first costs. Nothing under fopen is keeping the path
 * warm, so the work is there to be removed -- unlike the reads, where the
 * calls a cache could take away were already free and taking them away bought
 * nothing at all.
 *
 * So don't close. When the game closes a file, park the handle under its path;
 * when it opens that path again, hand the handle back rewound. The saving is
 * the whole cost of a repeat open, about 36 s a session, a quarter of an
 * area-load freeze and a fifth of the wait before the first frame.
 *
 * What makes this safe to do at all is that every one of those 17667 opens is
 * read only -- the trace counts zero for writing -- and the game imports no
 * remove, unlink or rename, so nothing it can do makes a held handle stale.
 * The cache still refuses any mode that could write, so that stays true if the
 * game ever changes.
 */

#include <stdlib.h>
#include <string.h>

#include "handle_cache.h"

// Long enough for the paths this game builds, and short enough that the whole
// table stays a few tens of KB. A path that does not fit is not cached at all
// rather than truncated, because two paths sharing a truncation would serve
// one file's bytes for the other's.
#define HC_PATH 192
#define HC_SLOTS 64
// Handles the game has open right now. fclose is given a FILE * and nothing
// else, so parking one means remembering the path it was opened under.
#define HC_LIVE 256
// Never hold fewer than this once the cache has been made to back off, or it
// stops being a cache at all.
#define HC_MIN_SLOTS 4

typedef struct {
  FILE *file; // NULL for an empty slot
  unsigned used;
  char path[HC_PATH];
} Parked;

typedef struct {
  FILE *file; // NULL for an empty slot
  char path[HC_PATH];
} Live;

static HandleCacheOps io;
static Parked parked[HC_SLOTS];
static Live live[HC_LIVE];
static unsigned hc_slots = HC_SLOTS;
static unsigned hc_clock;
static HandleCacheStats stats;

// The same spinlock the allocation tracer uses. Four threads open and close
// files here and the critical sections are a handful of string compares, so a
// lock is both necessary and cheap. Nothing that can block -- no fopen, fclose
// or fseek -- is ever called while it is held.
static volatile int table_lock;

static void lock(void) {
  while (__atomic_test_and_set(&table_lock, __ATOMIC_ACQUIRE))
    ;
}

static void unlock(void) { __atomic_clear(&table_lock, __ATOMIC_RELEASE); }

void handle_cache_init(const HandleCacheOps *ops, unsigned slots) {
  io = *ops;
  hc_slots = slots > HC_SLOTS ? HC_SLOTS : (slots ? slots : 1);
  stats.slots = hc_slots;
  memset(parked, 0, sizeof(parked));
  memset(live, 0, sizeof(live));
  hc_clock = 0;
  memset(&stats, 0, sizeof(stats));
}

// Anything that is not purely reading. "r" and "rb" are kept; everything else
// goes straight through, so a writer can never be handed a handle positioned
// and buffered by an earlier reader.
static int read_only(const char *mode) {
  if (!mode || mode[0] != 'r')
    return 0;
  for (const char *m = mode; *m; m++)
    if (*m == '+' || *m == 'w' || *m == 'a')
      return 0;
  return 1;
}

static int keyable(const char *path) {
  return path && path[0] && strlen(path) < HC_PATH;
}

// All four of these want the lock held.

static void remember_live(FILE *file, const char *path) {
  for (unsigned i = 0; i < HC_LIVE; i++) {
    if (!live[i].file) {
      live[i].file = file;
      memcpy(live[i].path, path, strlen(path) + 1);
      return;
    }
  }
  // No room to remember it, so its close will be a real one. Not an error:
  // the cache does less, and nothing it does is wrong.
}

// Copies the path this handle was opened under into out and forgets it.
// Returns 0 if the cache never saw it opened.
static int forget_live(FILE *file, char *out) {
  for (unsigned i = 0; i < HC_LIVE; i++) {
    if (live[i].file == file) {
      if (out)
        memcpy(out, live[i].path, strlen(live[i].path) + 1);
      live[i].file = NULL;
      return 1;
    }
  }
  return 0;
}

// Takes a handle for this path out of the table, or NULL if none is held.
static FILE *unpark(const char *path) {
  for (unsigned i = 0; i < hc_slots; i++) {
    if (parked[i].file && strcmp(parked[i].path, path) == 0) {
      FILE *f = parked[i].file;
      parked[i].file = NULL;
      stats.held--;
      return f;
    }
  }
  return NULL;
}

// Puts a handle in, and hands back whichever one it displaced. There is always
// a slot, because the least recently used one is always available.
static FILE *park(FILE *file, const char *path) {
  unsigned slot = 0, oldest = 0;
  FILE *evicted = NULL;

  for (unsigned i = 0; i < hc_slots; i++) {
    if (!parked[i].file) {
      slot = i;
      oldest = 0;
      break;
    }
    if (i == 0 || parked[i].used < oldest) {
      oldest = parked[i].used;
      slot = i;
    }
  }
  if (parked[slot].file) {
    evicted = parked[slot].file;
    stats.evicted++;
    stats.held--;
  }
  parked[slot].file = file;
  parked[slot].used = ++hc_clock;
  memcpy(parked[slot].path, path, strlen(path) + 1);
  stats.parked++;
  stats.held++;
  return evicted;
}

FILE *handle_cache_fopen(const char *path, const char *mode) {
  if (!read_only(mode) || !keyable(path))
    return io.fopen(path, mode);

  lock();
  FILE *held = unpark(path);
  unlock();

  if (held) {
    // Hand it back the way a fresh open would: at the start. fseek also clears
    // the end-of-file indicator, which is the only other state a previous
    // reader could have left behind that matters here.
    if (io.fseek(held, 0, SEEK_SET) == 0) {
      lock();
      stats.hits++;
      remember_live(held, path);
      unlock();
      return held;
    }
    // It will not even seek. Drop it and open properly rather than hand back
    // something broken.
    io.fclose(held);
  }

  FILE *f = io.fopen(path, mode);
  if (!f) {
    // The safety net for running out of descriptors: give every held handle
    // back and try again, so running out degrades into being slow rather than
    // into failing to load.
    //
    // Whether to fire it is not a judgement call, and the two attempts to make
    // it one both did damage. Draining on every NULL meant draining on every
    // probe for a file that is not there, 5492 times in a session. Capping
    // those drains to learn from meant that once the cap was spent the net
    // stopped catching anything: the cache held 28 descriptors, the game
    // opened OBJECTS/IDE.DIR, got NULL, and passed it to OS_FileSetPosition,
    // which read through it. The game does not check.
    //
    // So ask instead of guessing. A file that is not there is not this cache's
    // doing and never needs a drain; a file that is there and will not open is
    // descriptors, and this cache is holding some of them. That check costs one
    // stat, only ever on the failure path, and it makes the guarantee absolute:
    // the cache is never the reason an existing file fails to open.
    if (io.exists && !io.exists(path)) {
      lock();
      stats.absent++;
      unlock();
    } else {
      lock();
      unsigned holding = stats.held;
      if (holding)
        stats.drains++;
      unlock();
      if (holding) {
        handle_cache_drain();
        f = io.fopen(path, mode);
        if (f) {
          // It really was us. That many was too many, so hold fewer -- the
          // descriptor budget belongs to the game and the only way to learn it
          // is to be told.
          lock();
          stats.rescued++;
          unsigned fewer = holding - holding / 4;
          hc_slots = fewer < HC_MIN_SLOTS ? HC_MIN_SLOTS : fewer;
          stats.slots = hc_slots;
          unlock();
        }
      }
    }
  }

  lock();
  stats.misses++;
  if (f)
    remember_live(f, path);
  unlock();
  return f;
}

int handle_cache_fclose(FILE *stream) {
  char path[HC_PATH];

  lock();
  int tracked = forget_live(stream, path);
  unlock();

  if (!tracked)
    return io.fclose(stream);

  // A handle that has already failed carries its error flag to whoever picks
  // it up next, and there is no clearerr to reach through the bridge.
  if (io.errored && io.errored(stream)) {
    lock();
    stats.dropped++;
    unlock();
    return io.fclose(stream);
  }

  lock();
  FILE *evicted = park(stream, path);
  unlock();

  if (evicted)
    io.fclose(evicted);
  // The game asked for it closed, and as far as it can tell it is.
  return 0;
}

void handle_cache_drain(void) {
  FILE *closing[HC_SLOTS];
  unsigned n = 0;

  lock();
  for (unsigned i = 0; i < hc_slots; i++) {
    if (parked[i].file) {
      closing[n++] = parked[i].file;
      parked[i].file = NULL;
    }
  }
  stats.held = 0;
  unlock();

  for (unsigned i = 0; i < n; i++)
    io.fclose(closing[i]);
}

void handle_cache_stats(HandleCacheStats *out) {
  lock();
  *out = stats;
  unlock();
}
