/* dir_cache.c -- answer "is this file there" from a directory listing
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * Two thirds of this game's opens -- 11633 of 17478 a session -- are for files
 * that are not there, and they cost 28 s between them. The game asks for a
 * loose copy of an asset in a couple of directories before reading it out of
 * the archive, and the loose copy is almost never there. Every one of those
 * questions goes to the card.
 *
 * Ask about the directory instead. The probes fall in 32 directories, 91% of
 * them in two, so one listing answers for thousands of them.
 *
 * WHICH WAY THE ERRORS MATTER
 *
 * Saying "it is there" when it is not costs an open that fails, which is what
 * would have happened anyway. Saying "it is not there" when it is hands the
 * game a NULL it does not check, and that is the crash this branch already
 * had once. So every part of this is built so that the second mistake is the
 * hard one to make:
 *
 *  - "No" only ever comes from a complete listing. A directory that would not
 *    open, or did not fit, answers "unknown" and the file is opened as before.
 *  - Names are kept as hashes, folded to one case, because the card does not
 *    care about case and the game asks both ways. A collision makes an absent
 *    name look present -- the harmless direction -- and never the reverse,
 *    since the real name's hash is in the table either way.
 *  - A listing does not get to say "no" until the card has agreed with it a
 *    few times: the first opens into each directory go through, and their
 *    result is checked against what the listing would have said. The first
 *    time an open succeeds where the listing said absent, that directory is
 *    marked and never says "no" again. That covers every way a listing can be
 *    wrong that nobody has thought of. The card has the last word.
 *  - Anything that writes drops the listing first.
 *  - One lock around the table, because a slot being relisted by one thread
 *    while another reads it is a half-cleared table answering "absent".
 */

#include <string.h>

#include "dir_cache.h"

#define DC_DIRS 48        // directories held at once
#define DC_NAMES 4096     // names per directory; a power of two
#define DC_PATH 160
#define DC_NAME 128
#define DC_VERIFY 4       // opens the card must agree with before "no" is trusted

typedef struct {
  unsigned hash; // of the directory path, 0 for an empty slot
  char path[DC_PATH];
  unsigned names[DC_NAMES]; // filename hashes, 0 for an empty slot
  unsigned count;
  int complete;      // the whole listing fitted, so "no" can be trusted
  int missing;       // the directory itself is not there, so nothing in it is
  int retired;       // caught saying absent about a file that opened: never again
  unsigned verified; // opens the card has agreed with the listing about
} Dir;

// How long the listings take, and what they found, for the trace.
unsigned dir_list_us;
char dir_last[4][DC_PATH];
unsigned dir_last_count[4];
static unsigned dir_last_next;

static DirCacheOps io;
static Dir dirs[DC_DIRS];
static unsigned dc_next; // round robin when every slot is taken
static DirCacheStats stats;

static void lock(void) {
  if (io.lock)
    io.lock();
}

static void unlock(void) {
  if (io.unlock)
    io.unlock();
}

void dir_cache_init(const DirCacheOps *ops) {
  io = *ops;
  memset(dirs, 0, sizeof(dirs));
  memset(&stats, 0, sizeof(stats));
  dc_next = 0;
}

// FNV-1a, never zero so that zero can mean an empty slot. Case is folded
// because the card's filesystem does not distinguish it and the game asks for
// "Config" and "config" in the same session: folding can only merge names, so
// it can only ever make an absent name look present.
static unsigned hash_of(const char *s, int len) {
  unsigned h = 2166136261u;
  for (int i = 0; i < len && s[i]; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
    h = (h ^ c) * 16777619u;
  }
  return h ? h : 1;
}

// Both sides of a name lookup hash the same number of characters as read_dir
// can hand back, or a name longer than that never matches itself.
static unsigned name_hash(const char *name) { return hash_of(name, DC_NAME - 1); }

static void add_name(Dir *d, const char *name) {
  unsigned h = name_hash(name);
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
  unsigned h = name_hash(name);
  for (unsigned i = 0; i < DC_NAMES; i++) {
    unsigned slot = (h + i) & (DC_NAMES - 1);
    if (!d->names[slot])
      return 0;
    if (d->names[slot] == h)
      return 1;
  }
  return 0;
}

// Reads the whole directory into a slot. Called with the lock held.
static Dir *list_dir(const char *path, unsigned h) {
  Dir *d = &dirs[dc_next];
  dc_next = (dc_next + 1) % DC_DIRS;
  if (d->hash)
    stats.entries -= d->count;
  memset(d, 0, sizeof(*d));

  unsigned t0 = io.now_us ? io.now_us() : 0;
  int handle = io.open_dir(path);
  if (handle < 0) {
    // Two different failures wearing one return value, and they call for
    // opposite answers. If the directory is not there, then nothing in it is,
    // and that is a complete answer -- the strongest one there is, since no
    // listing can ever contradict it. On hardware one such directory absorbs
    // 5791 probes a session on its own.
    //
    // If the directory is there but would not open, nothing is known, and the
    // slot is kept anyway so the failed open is not repeated thousands of
    // times -- kept incomplete, so it can never be used to say no.
    if (!io.stat(path)) {
      d->missing = 1;
      d->complete = 1;
      stats.missing++;
    } else {
      stats.unreadable++;
    }
  } else {
    d->complete = 1;
    char name[DC_NAME];
    while (io.read_dir(handle, name, sizeof(name)))
      add_name(d, name);
    io.close_dir(handle);
    if (!d->complete)
      stats.overflow++;
    stats.listed++;
  }
  if (io.now_us)
    dir_list_us += io.now_us() - t0;

  d->hash = h;
  memcpy(d->path, path, strlen(path) + 1);
  memcpy(dir_last[dir_last_next], path, strlen(path) + 1);
  dir_last_count[dir_last_next] = d->count;
  dir_last_next = (dir_last_next + 1) % 4;
  stats.entries += d->count;
  return d;
}

// Splits path into the directory it is in and the name within it. Returns
// the directory's length, or 0 if there is no directory part to work with.
// A device prefix keeps its colon -- "ux0:" is a directory, "ux0" is not.
static int split(const char *path, char dir[DC_PATH], const char **name) {
  const char *sep = NULL;
  for (const char *c = path; *c; c++)
    if (*c == '/' || *c == ':')
      sep = c;
  if (!sep)
    return 0;
  int len = (int)(sep - path);
  if (*sep == ':' || len == 0) // "ux0:name", or "/name" whose directory is "/"
    len++;
  if (len >= DC_PATH - 1)
    return 0;
  memcpy(dir, path, len);
  dir[len] = 0;
  *name = sep + 1;
  return len;
}

// The slot holding this directory, or NULL. Called with the lock held.
static Dir *find_dir(const char *dir, unsigned h) {
  for (unsigned i = 0; i < DC_DIRS; i++)
    if (dirs[i].hash == h && strcmp(dirs[i].path, dir) == 0)
      return &dirs[i];
  return NULL;
}

// The slot for the directory path is in, listing it first if need be. Called
// with the lock held. NULL means there is nothing to answer from.
static Dir *dir_of(const char *path, const char **name) {
  char dir[DC_PATH];
  if (!split(path, dir, name))
    return NULL;
  unsigned h = hash_of(dir, DC_PATH);
  Dir *d = find_dir(dir, h);
  return d ? d : list_dir(dir, h);
}

// Whether a listing may be used to say "no". Called with the lock held. One
// that is not complete never may; one that has been caught out never may
// again; a complete one may once the card has agreed with it DC_VERIFY times.
static int may_say_no(const Dir *d) {
  return d->complete && !d->retired && d->verified >= DC_VERIFY;
}

int dir_cache_lookup(const char *path) {
  // Before init there is nothing to answer from and nothing to call. Saying so
  // is always safe; the caller opens the file as it always did.
  if (!io.open_dir || !path || !path[0])
    return DIR_CACHE_UNKNOWN;

  lock();
  const char *name = NULL;
  Dir *d = dir_of(path, &name);
  int answer;
  if (!d || !d->complete) {
    stats.unknown++;
    answer = DIR_CACHE_UNKNOWN;
  } else if (has_name(d, name)) {
    stats.answered++;
    answer = DIR_CACHE_PRESENT;
  } else if (may_say_no(d)) {
    stats.answered++;
    answer = DIR_CACHE_ABSENT;
  } else {
    // The listing says no, but it has not earned the right to yet. Open the
    // file; observe() will hear how that went.
    stats.unknown++;
    answer = DIR_CACHE_UNKNOWN;
  }
  unlock();
  return answer;
}

void dir_cache_observe(const char *path, int existed) {
  if (!io.open_dir || !path || !path[0])
    return;
  lock();
  char dir[DC_PATH];
  const char *name = NULL;
  Dir *d = NULL;
  if (split(path, dir, &name))
    d = find_dir(dir, hash_of(dir, DC_PATH)); // never lists: observe only
  if (d && d->complete) {
    int listed = has_name(d, name);
    if (existed && !listed) {
      // The one thing that must never happen happened, and the card caught it
      // before the game did. This listing never says no again -- it can still
      // say yes, which is the harmless direction and still saves nothing but
      // costs nothing either.
      if (!d->retired)
        stats.contradicted++;
      d->retired = 1;
    } else if (!existed && listed) {
      stats.stale++; // harmless direction: the open failed as it would have
    } else if (!existed && !listed) {
      d->verified++; // the listing and the card agree
    }
  }
  unlock();
}

int dir_cache_exists(const char *path) {
  if (!path || !path[0])
    return 0;

  lock();
  const char *name = NULL;
  Dir *d = dir_of(path, &name);
  int answer = -1;
  if (d && d->complete) {
    if (has_name(d, name))
      answer = 1;
    else if (may_say_no(d))
      answer = 0;
  }
  if (answer >= 0)
    stats.answered++;
  else
    stats.statted++;
  unlock();

  // Anything the listing cannot yet say goes to the filesystem one file at a
  // time, and what it says is fed back so the listing can earn its trust.
  if (answer < 0) {
    answer = io.stat(path);
    dir_cache_observe(path, answer);
  }
  return answer;
}

void dir_cache_forget(const char *path) {
  if (!io.open_dir || !path || !path[0])
    return;
  lock();
  // Find the slot without listing anything: a directory nobody has listed has
  // nothing to forget, and listing it here would be a card read caused by a
  // write, in the middle of the write.
  char dir[DC_PATH];
  const char *name = NULL;
  if (split(path, dir, &name)) {
    Dir *d = find_dir(dir, hash_of(dir, DC_PATH));
    if (d) {
      stats.entries -= d->count;
      stats.forgotten++;
      memset(d, 0, sizeof(*d));
    }
  }
  unlock();
}

void dir_cache_stats(DirCacheStats *out) {
  lock();
  *out = stats;
  unlock();
}
