/* test_dir_cache.c -- it may be wrong about a file being there, never about a
 * file not being there.
 *
 * This answers the question the handle cache uses to decide whether a failed
 * open was its own fault. Wrong towards "present" costs a drain nobody needed.
 * Wrong towards "absent" means the handle cache keeps its descriptors, the open
 * stays failed, and the game reads through a NULL -- which it has already done
 * once. So the tests are asymmetric on purpose: every case that could produce a
 * false "absent" is checked, and the cheap direction is only checked for cost.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../loader/dir_cache.c"

#define DIRA "out/dc_a"
#define DIRB "out/dc_b"

static int stat_calls, open_dir_calls;
static int refuse_open_dir;

// A handle table, because the interface hands back an int -- sceIoDopen returns
// an SceUID -- and a host DIR * does not fit in one. Truncating the pointer
// gave a negative handle, which the cache correctly read as "could not open"
// and quietly answered every question with a stat instead.
#define HANDLES 8
static DIR *open_dirs[HANDLES];

static int host_open_dir(const char *path) {
  open_dir_calls++;
  if (refuse_open_dir)
    return -1;
  DIR *d = opendir(path);
  if (!d)
    return -1;
  for (int i = 0; i < HANDLES; i++) {
    if (!open_dirs[i]) {
      open_dirs[i] = d;
      return i;
    }
  }
  closedir(d);
  return -1;
}

static int host_read_dir(int handle, char *name, int size) {
  struct dirent *e;
  DIR *d = open_dirs[handle];
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.')
      continue; // the host has . and .. and the Vita does not
    snprintf(name, size, "%s", e->d_name);
    return 1;
  }
  return 0;
}

static void host_close_dir(int handle) {
  closedir(open_dirs[handle]);
  open_dirs[handle] = NULL;
}

static int host_stat(const char *path) {
  stat_calls++;
  return access(path, F_OK) == 0;
}

static const DirCacheOps HOST = { host_open_dir, host_read_dir, host_close_dir,
                                  host_stat };

static void make(const char *dir, int n, const char *prefix) {
  mkdir(dir, 0755);
  for (int i = 0; i < n; i++) {
    char p[512];
    snprintf(p, sizeof(p), "%s/%s%04d.bin", dir, prefix, i);
    FILE *f = fopen(p, "wb");
    assert(f);
    fputc('x', f);
    fclose(f);
  }
}

static void wipe(const char *dir) {
  DIR *d = opendir(dir);
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.')
      continue;
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
    unlink(p);
  }
  closedir(d);
  rmdir(dir);
}

// Every file that is really there must be found, or the handle cache stops
// draining and the game gets a NULL.
static void test_present_files_are_found(void) {
  dir_cache_init(&HOST);
  char p[512];
  for (int i = 0; i < 300; i++) {
    snprintf(p, sizeof(p), DIRA "/a%04d.bin", i);
    assert(dir_cache_exists(p) && "a file that is there must be found");
  }
  DirCacheStats s;
  dir_cache_stats(&s);
  assert(s.listed == 1);
  printf("present      : 300 files found from %u listing, %u entries held  OK\n",
         s.listed, s.entries);
}

static void test_absent_files_are_absent(void) {
  dir_cache_init(&HOST);
  char p[512];
  int asked = stat_calls;
  for (int i = 0; i < 500; i++) {
    snprintf(p, sizeof(p), DIRA "/nothing%04d.bin", i);
    assert(!dir_cache_exists(p));
  }
  DirCacheStats s;
  dir_cache_stats(&s);
  assert(s.answered == 500);
  assert(stat_calls == asked && "not one file was asked about individually");
  printf("absent       : 500 misses answered from 1 listing, 0 stats  OK\n");
}

// The point of the whole thing: one question to the card per directory, not
// one per file.
static void test_one_listing_serves_everything(void) {
  dir_cache_init(&HOST);
  int dirs_before = open_dir_calls, stats_before = stat_calls;
  char p[512];
  for (int round = 0; round < 20; round++) {
    for (int i = 0; i < 300; i++) {
      snprintf(p, sizeof(p), DIRA "/a%04d.bin", i);
      assert(dir_cache_exists(p));
      snprintf(p, sizeof(p), DIRA "/gone%04d.bin", i);
      assert(!dir_cache_exists(p));
    }
  }
  assert(open_dir_calls - dirs_before == 1);
  assert(stat_calls - stats_before == 0);
  printf("one listing  : 12000 questions, %d directory read, %d files asked "
         "about  OK\n", open_dir_calls - dirs_before, stat_calls - stats_before);
}

// A directory it cannot read must not be turned into "nothing in here exists".
static void test_an_unreadable_directory_falls_back(void) {
  dir_cache_init(&HOST);
  refuse_open_dir = 1;
  int before = stat_calls;
  char p[512];
  snprintf(p, sizeof(p), DIRA "/a0000.bin");
  assert(dir_cache_exists(p) && "a real file must still be found");
  snprintf(p, sizeof(p), DIRA "/nothere.bin");
  assert(!dir_cache_exists(p));
  refuse_open_dir = 0;
  assert(stat_calls == before + 2 && "both went to the filesystem");
  printf("unreadable   : falls back to asking about each file  OK\n");
}

// More names than a slot can hold. The listing is incomplete, so it may not be
// used to say no -- every question about that directory must go to the
// filesystem instead.
static void test_an_overfull_directory_never_says_no(void) {
  dir_cache_init(&HOST);
  Dir *d = &dirs[0];
  memset(d, 0, sizeof(*d));
  d->hash = hash_of(DIRB, DC_PATH);
  memcpy(d->path, DIRB, strlen(DIRB) + 1);
  d->complete = 0; // as list_dir marks one that did not fit

  int before = stat_calls;
  char p[512];
  snprintf(p, sizeof(p), DIRB "/b0000.bin");
  assert(dir_cache_exists(p));
  snprintf(p, sizeof(p), DIRB "/absent.bin");
  assert(!dir_cache_exists(p));
  assert(stat_calls == before + 2 && "an incomplete listing answers nothing");
  printf("overfull     : an incomplete listing is never used to say no  OK\n");
}

// More directories than slots. Whichever is evicted must still give right
// answers, by listing again rather than by guessing.
static void test_more_directories_than_slots(void) {
  dir_cache_init(&HOST);
  char dir[512], p[512];
  for (int i = 0; i < DC_DIRS + 6; i++) {
    snprintf(dir, sizeof(dir), "out/dc_m%02d", i);
    make(dir, 3, "m");
  }
  for (int round = 0; round < 3; round++) {
    for (int i = 0; i < DC_DIRS + 6; i++) {
      snprintf(p, sizeof(p), "out/dc_m%02d/m0001.bin", i);
      assert(dir_cache_exists(p) && "still there after the slot was reused");
      snprintf(p, sizeof(p), "out/dc_m%02d/m9999.bin", i);
      assert(!dir_cache_exists(p));
    }
  }
  for (int i = 0; i < DC_DIRS + 6; i++) {
    snprintf(dir, sizeof(dir), "out/dc_m%02d", i);
    wipe(dir);
  }
  printf("many dirs    : %d directories through %d slots, every answer right  OK\n",
         DC_DIRS + 6, DC_DIRS);
}

// A directory with more names than the table holds. Making four thousand files
// to prove it would be slow, so drive the placement directly: once a name
// cannot be stored, the listing has to stop being trusted, or that one name
// becomes a file that exists and reads as absent.
static void test_a_name_that_will_not_fit_spoils_the_listing(void) {
  dir_cache_init(&HOST);
  Dir *d = &dirs[0];
  memset(d, 0, sizeof(*d));
  d->complete = 1;

  char name[64];
  for (unsigned i = 0; i < DC_NAMES + 16; i++) {
    snprintf(name, sizeof(name), "f%08u.bin", i);
    add_name(d, name);
  }
  assert(!d->complete && "a listing that lost a name may not be used to say no");
  printf("did not fit  : a name with nowhere to go marks the listing unusable, "
         "%u of %u held  OK\n", d->count, (unsigned)DC_NAMES);
}

// A path with no directory in it at all must not be mishandled.
static void test_a_bare_name(void) {
  dir_cache_init(&HOST);
  int before = stat_calls;
  assert(!dir_cache_exists("nosuchfile"));
  assert(stat_calls == before + 1);
  assert(!dir_cache_exists(""));
  printf("bare name    : a path with no directory goes to the filesystem  OK\n");
}

int main(void) {
  make(DIRA, 300, "a");
  make(DIRB, 4, "b");

  test_present_files_are_found();
  test_absent_files_are_absent();
  test_one_listing_serves_everything();
  test_an_unreadable_directory_falls_back();
  test_an_overfull_directory_never_says_no();
  test_more_directories_than_slots();
  test_a_name_that_will_not_fit_spoils_the_listing();
  test_a_bare_name();

  wipe(DIRA);
  wipe(DIRB);
  printf("PASS\n");
  return 0;
}
