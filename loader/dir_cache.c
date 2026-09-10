/* dir_cache.c -- answer "is this file there" from a directory listing
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * Two thirds of this game's opens -- around 12000 of 18000 a session -- are for
 * files that are not there. The handle cache has to tell those apart from an
 * open that failed because it was holding the descriptors, because getting that
 * wrong in one direction empties the cache on every probe and in the other
 * hands the game a NULL it does not check. Asking the filesystem about the file
 * is the only answer that is always right, and it cost about 43 s a session --
 * more than holding handles saved, which is why the handle cache was switched
 * off.
 *
 * Ask about the directory instead. The game probes thousands of distinct paths
 * across a handful of directories, so one listing answers for all of them.
 *
 * WHICH WAY THE ERRORS MATTER
 *
 * Saying "it is there" when it is not costs a drain that turns out to be
 * unnecessary: some lost hit rate, nothing more. Saying "it is not there" when
 * it is means the handle cache concludes the failure was not its doing, does
 * not give its descriptors back, and returns the NULL that crashed the game
 * once already. So this may only ever answer "no" from a complete listing.
 *
 * That is also why the names are kept as hashes rather than strings. A
 * collision makes a name that is absent look present -- the harmless direction
 * -- and can never make a present name look absent, because the real name's
 * hash is in the table either way. A directory whose listing does not fit is
 * marked and every question about it goes back to asking one file at a time.
 */

#include <string.h>

#include "dir_cache.h"

#define DC_DIRS 24        // directories held at once
#define DC_NAMES 4096     // names per directory; a power of two
#define DC_PATH 160
#define DC_NAME 128

typedef struct {
  unsigned hash; // of the directory path, 0 for an empty slot
  char path[DC_PATH];
  unsigned names[DC_NAMES]; // filename hashes, 0 for an empty slot
  unsigned count;
  int complete; // the whole listing fitted, so "no" can be trusted
} Dir;

static DirCacheOps io;
static Dir dirs[DC_DIRS];
static unsigned dc_next; // round robin when every slot is taken
static DirCacheStats stats;

void dir_cache_init(const DirCacheOps *ops) {
  io = *ops;
  memset(dirs, 0, sizeof(dirs));
  memset(&stats, 0, sizeof(stats));
  dc_next = 0;
}

// FNV-1a, never zero so that zero can mean an empty slot.
static unsigned hash_of(const char *s, int len) {
  unsigned h = 2166136261u;
  for (int i = 0; i < len && s[i]; i++)
    h = (h ^ (unsigned char)s[i]) * 16777619u;
  return h ? h : 1;
}

static void add_name(Dir *d, const char *name) {
  unsigned h = hash_of(name, DC_NAME);
  for (unsigned i = 0; i < DC_NAMES; i++) {
    unsigned slot = (h + i) & (DC_NAMES - 1);
    if (!d->names[slot]) {
      d->names[slot] = h;
      d->count++;
      return;
    }
    if (d->names[slot] == h)
      return; // already have it
  }
  d->complete = 0; // nowhere to put it, so this listing can no longer say "no"
}

static int has_name(const Dir *d, const char *name) {
  unsigned h = hash_of(name, DC_NAME);
  for (unsigned i = 0; i < DC_NAMES; i++) {
    unsigned slot = (h + i) & (DC_NAMES - 1);
    if (!d->names[slot])
      return 0;
    if (d->names[slot] == h)
      return 1;
  }
  return 0;
}

// Reads the whole directory into a slot. The slot is only marked complete if
// every name fitted, because a partial listing cannot be used to say no.
static Dir *list_dir(const char *path, unsigned h) {
  Dir *d = &dirs[dc_next];
  dc_next = (dc_next + 1) % DC_DIRS;
  if (d->hash)
    stats.entries -= d->count;
  memset(d, 0, sizeof(*d));

  int handle = io.open_dir(path);
  if (handle < 0)
    return NULL; // no such directory, or unreadable: fall back to stat

  d->complete = 1;
  char name[DC_NAME];
  while (io.read_dir(handle, name, sizeof(name)))
    add_name(d, name);
  io.close_dir(handle);

  d->hash = h;
  memcpy(d->path, path, strlen(path) + 1);
  stats.listed++;
  stats.entries += d->count;
  if (!d->complete)
    stats.overflow++;
  return d;
}

int dir_cache_exists(const char *path) {
  if (!path || !path[0])
    return 0;

  const char *slash = NULL;
  for (const char *c = path; *c; c++)
    if (*c == '/' || *c == ':')
      slash = c;
  if (!slash || slash - path >= DC_PATH - 1)
    return io.stat(path); // no directory part to work with

  char dir[DC_PATH];
  int len = (int)(slash - path);
  if (len == 0) // a path like "/name": the directory is the root
    len = 1;
  memcpy(dir, path, len);
  dir[len] = 0;
  const char *name = slash + 1;

  unsigned h = hash_of(dir, DC_PATH);
  Dir *d = NULL;
  for (unsigned i = 0; i < DC_DIRS; i++) {
    if (dirs[i].hash == h && strcmp(dirs[i].path, dir) == 0) {
      d = &dirs[i];
      break;
    }
  }
  if (!d)
    d = list_dir(dir, h);

  // Anything short of a complete listing, and the question goes back to the
  // filesystem one file at a time. This is the whole safety property.
  if (!d || !d->complete) {
    stats.statted++;
    return io.stat(path);
  }

  stats.answered++;
  return has_name(d, name);
}

void dir_cache_stats(DirCacheStats *out) { *out = stats; }
