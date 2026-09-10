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
  // Also asked about a directory that would not open, to tell "not there"
  // from "there but unreadable".
  int (*stat)(const char *path);
  // Optional. Only used to report how long the listings took.
  unsigned (*now_us)(void);
  // Optional, but four threads open files and one of them relisting a slot
  // while another reads it would answer "absent" from a half-cleared table.
  // Both NULL means nothing is locked, which is only right single-threaded.
  void (*lock)(void);
  void (*unlock)(void);
} DirCacheOps;

void dir_cache_init(const DirCacheOps *ops);

// What a listing can say about the path, with no question ever put to the
// filesystem about the file itself. This is the form the open path uses,
// because there the fallback is not free: a stat that answers "not there" has
// cost as much as the open it was meant to save.
//
// ABSENT is the only answer that changes what the game sees, and it is only
// given from a complete listing that the card has already confirmed a few
// times over -- see dir_cache_observe.
#define DIR_CACHE_UNKNOWN (-1) // no listing to answer from; open the file
#define DIR_CACHE_ABSENT 0     // a complete, confirmed listing does not have it
#define DIR_CACHE_PRESENT 1
int dir_cache_lookup(const char *path);

// What the filesystem actually said, for every open that went to it. A listing
// earns the right to say "absent" by being agreed with, and loses it forever
// the first time an open succeeds where the listing said there was nothing.
// This is the guard against every way a listing can be wrong that nobody has
// thought of: the card has the last word, always.
void dir_cache_observe(const char *path, int existed);

// Whether the path exists, asking the filesystem about the file when the
// listing cannot say. The handle cache's form.
int dir_cache_exists(const char *path);

// Drop what is held about the directory this path is in. Anything that creates
// or removes a file has to call this, or a listing taken before the write goes
// on answering "not there" for a file that now is -- the one direction this
// must never be wrong in.
void dir_cache_forget(const char *path);

typedef struct {
  unsigned listed;       // directories read from the card
  unsigned missing;      // directories that are not there at all
  unsigned unreadable;   // directories there but would not open; never say no
  unsigned answered;     // questions answered from a listing
  unsigned unknown;      // questions a listing could not answer
  unsigned statted;      // questions that had to ask about one file
  unsigned entries;      // names held across every listing
  unsigned overflow;     // directories too big to hold, which never say no
  unsigned forgotten;    // listings dropped because something was written
  unsigned contradicted; // an open succeeded where a listing said absent
  unsigned stale;        // an open failed where a listing said present
} DirCacheStats;

void dir_cache_stats(DirCacheStats *out);

#endif
