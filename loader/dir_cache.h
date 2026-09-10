/* dir_cache.h -- answer "is this file there" from a directory listing
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __DIR_CACHE_H__
#define __DIR_CACHE_H__

// The directory calls this sits on, injected so the host tests drive the same
// code against real directories.
typedef struct {
  // Opens a directory for listing. Any negative value means it could not be
  // opened, and the cache falls back to asking about files one at a time.
  int (*open_dir)(const char *path);
  // Writes the next name into name and returns 1, or returns 0 at the end.
  int (*read_dir)(int handle, char *name, int size);
  void (*close_dir)(int handle);
  // The fallback, and the thing this exists to avoid: one question per file.
  int (*stat)(const char *path);
} DirCacheOps;

void dir_cache_init(const DirCacheOps *ops);

// Whether the path exists. Never answers "no" from anything less than a
// complete listing of the directory it is in -- see the note in the .c on why
// that direction is the one that matters.
int dir_cache_exists(const char *path);

typedef struct {
  unsigned listed;    // directories read from the card
  unsigned answered;  // questions answered from a listing
  unsigned statted;   // questions that had to ask about one file
  unsigned entries;   // names held across every listing
  unsigned overflow;  // directories too big to hold, which always stat
} DirCacheStats;

void dir_cache_stats(DirCacheStats *out);

#endif
