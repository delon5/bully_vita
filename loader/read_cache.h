/* read_cache.h -- coalesce the game's small file reads
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __READ_CACHE_H__
#define __READ_CACHE_H__

#include <stdio.h>
#include <stddef.h>

// The three stdio calls the cache sits on. Set at init so the host tests can
// drive the same code against real files.
typedef struct {
  size_t (*fread)(void *ptr, size_t size, size_t count, FILE *stream);
  int (*fseek)(FILE *stream, long offset, int origin);
  long (*ftell)(FILE *stream);
  // Who is calling. Four threads read concurrently in this game and each gets
  // its own buffers; see the comment on ownership in read_cache.c.
  unsigned (*thread_id)(void);
} ReadCacheOps;

void read_cache_init(const ReadCacheOps *ops);

// Drop-in for fread. Returns items read, exactly as fread does.
size_t read_cache_fread(void *ptr, size_t size, size_t count, FILE *stream);

// A file is closing, so nothing may keep a buffer keyed to it -- the next
// fopen can hand back the same FILE * for a different file.
void read_cache_forget(FILE *stream);

// Hands a thread's buffers and tracking entries back to the pool.
void read_cache_release(unsigned thread);

typedef struct {
  unsigned hits, misses, refills, bytes_served_kb, unowned;
} ReadCacheStats;

void read_cache_stats(ReadCacheStats *out);

#endif
