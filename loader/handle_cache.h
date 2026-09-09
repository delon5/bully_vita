/* handle_cache.h -- keep the game's files open instead of reopening them
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __HANDLE_CACHE_H__
#define __HANDLE_CACHE_H__

#include <stdio.h>

// The stdio calls the cache sits on. Set at init so the host tests can drive
// the same code against real files.
typedef struct {
  FILE *(*fopen)(const char *path, const char *mode);
  int (*fclose)(FILE *stream);
  int (*fseek)(FILE *stream, long offset, int origin);
  // A handle that has already failed is not worth keeping: there is no
  // clearerr on the bridge, so a set error flag would follow the handle to
  // whoever picked it up next. Not called ferror, which newlib defines as a
  // macro -- the member expands and the file stops compiling for the device
  // while still building fine on a host.
  int (*errored)(FILE *stream);
  // Whether the path is there at all. An open that fails on a file that does
  // not exist is nothing to do with this cache; an open that fails on a file
  // that does exist is descriptors, and this cache is holding some. Without
  // this the two are indistinguishable, and guessing between them put a NULL
  // handle into a game that does not check for one.
  int (*exists)(const char *path);
} HandleCacheOps;

// slots is how many handles may be held open at once. Clamped to what the
// cache was built for.
void handle_cache_init(const HandleCacheOps *ops, unsigned slots);

// Drop-in for fopen and fclose. Both fall back to the real call for anything
// the cache will not hold: a mode that can write, a path too long to key on,
// or a handle it never saw opened.
FILE *handle_cache_fopen(const char *path, const char *mode);
int handle_cache_fclose(FILE *stream);

// Closes everything parked. For shutdown, and for the retry after an fopen
// fails, where the handles being held are the likeliest reason.
void handle_cache_drain(void);

typedef struct {
  unsigned hits;      // an open served by a handle already held
  unsigned misses;    // an open that had to go to the card
  unsigned parked;    // closes turned into a park
  unsigned evicted;   // parked handles closed to make room
  unsigned dropped;   // closes that could not be parked at all
  unsigned drains;    // fopen failed and the cache gave its handles back
  unsigned rescued;   // ...and the open then succeeded, so it really was us
  unsigned absent;    // an open that failed on a file that is not there
  unsigned slots;     // the cap, which comes down when a rescue says it must
  unsigned held;      // how many are parked right now
} HandleCacheStats;

void handle_cache_stats(HandleCacheStats *out);

#endif
