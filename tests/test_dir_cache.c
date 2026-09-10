/* test_dir_cache.c -- it may be wrong about a file being there, never about a
 * file not being there.
 *
 * This answers the question the open path uses to skip an open altogether.
 * Wrong towards "present" costs an open that fails, as it would have anyway.
 * Wrong towards "absent" means the game gets a NULL from fopen for a file
 * that is there, and reads through it -- which it has already done once. So
 * the tests are asymmetric on purpose: every case that could produce a false
 * "absent" is checked, and the cheap direction is only checked for cost.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <assert.h>
#include <dirent.h>
#include <pthread.h>
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
static pthread_mutex_t host_mutex = PTHREAD_MUTEX_INITIALIZER;

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

static void host_lock(void) { pthread_mutex_lock(&host_mutex); }
static void host_unlock(void) { pthread_mutex_unlock(&host_mutex); }

static const DirCacheOps HOST = { host_open_dir, host_read_dir, host_close_dir,
                                  host_stat, NULL, host_lock, host_unlock };

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

static void touch(const char *path) {
  FILE *f = fopen(path, "wb");
  assert(f);
  fclose(f);
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

// Lets a directory's listing say "no": the card agrees with it DC_VERIFY times
// over, as the first opens into a directory do on hardware.
static void earn_trust(const char *dir) {
  char p[512];
  for (int i = 0; i < DC_VERIFY; i++) {
    snprintf(p, sizeof(p), "%s/never%d.bin", dir, i);
    assert(dir_cache_lookup(p) == DIR_CACHE_UNKNOWN &&
           "not trusted to say no until the card has agreed");
    dir_cache_observe(p, 0);
  }
}

// Every file that is really there must be found, or the game gets a NULL.
static void test_present_files_are_found(void) {
  dir_cache_init(&HOST);
  char p[512];
  for (int i = 0; i < 300; i++) {
    snprintf(p, sizeof(p), DIRA "/a%04d.bin", i);
    assert(dir_cache_lookup(p) == DIR_CACHE_PRESENT && "a file that is there");
  }
  DirCacheStats s;
  dir_cache_stats(&s);
  assert(s.listed == 1);
  printf("present      : 300 files found from %u listing, %u entries held  OK\n",
         s.listed, s.entries);
}

static void test_absent_files_are_absent(void) {
  dir_cache_init(&HOST);
  earn_trust(DIRA);
  char p[512];
  int asked = stat_calls;
  for (int i = 0; i < 500; i++) {
    snprintf(p, sizeof(p), DIRA "/nothing%04d.bin", i);
    assert(dir_cache_lookup(p) == DIR_CACHE_ABSENT);
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
  earn_trust(DIRA);
  int dirs_before = open_dir_calls, stats_before = stat_calls;
  char p[512];
  for (int round = 0; round < 20; round++) {
    for (int i = 0; i < 300; i++) {
      snprintf(p, sizeof(p), DIRA "/a%04d.bin", i);
      assert(dir_cache_lookup(p) == DIR_CACHE_PRESENT);
      snprintf(p, sizeof(p), DIRA "/gone%04d.bin", i);
      assert(dir_cache_lookup(p) == DIR_CACHE_ABSENT);
    }
  }
  assert(open_dir_calls - dirs_before == 0 && "listed once, before the loop");
  assert(stat_calls - stats_before == 0);
  printf("one listing  : 12000 questions, 0 directory reads, 0 files asked "
         "about  OK\n");
}

// A listing does not get to say "no" until the card has agreed with it, and
// the first time the card disagrees -- an open succeeds where the listing said
// absent -- that directory never says "no" again. This is the guard for every
// way a listing can be wrong that has not been thought of.
static void test_a_listing_must_earn_the_right_to_say_no(void) {
  dir_cache_init(&HOST);
  char p[512];
  snprintf(p, sizeof(p), DIRA "/absent.bin");
  for (int i = 0; i < DC_VERIFY; i++) {
    assert(dir_cache_lookup(p) == DIR_CACHE_UNKNOWN && "open it and see");
    dir_cache_observe(p, 0);
  }
  assert(dir_cache_lookup(p) == DIR_CACHE_ABSENT && "trusted now");

  // The card says a file the listing does not have is there. Whatever the
  // reason -- case, a path the bridge resolves differently, anything -- the
  // listing is wrong in the one way that matters, and is retired.
  snprintf(p, sizeof(p), DIRA "/the-listing-missed-this.bin");
  dir_cache_observe(p, 1);
  DirCacheStats s;
  dir_cache_stats(&s);
  assert(s.contradicted == 1);
  snprintf(p, sizeof(p), DIRA "/absent.bin");
  for (int i = 0; i < 100; i++) {
    assert(dir_cache_lookup(p) == DIR_CACHE_UNKNOWN && "never says no again");
    dir_cache_observe(p, 0); // agreeing again does not restore trust
  }
  snprintf(p, sizeof(p), DIRA "/a0001.bin");
  assert(dir_cache_lookup(p) == DIR_CACHE_PRESENT && "still says yes");
  printf("earned trust : no \"absent\" before %d agreements, none after one "
         "contradiction  OK\n", DC_VERIFY);
}

// The card's filesystem does not care about case and the game asks both ways.
// A file that is there under one case must never be "absent" under another.
static void test_case_does_not_make_a_file_absent(void) {
  dir_cache_init(&HOST);
  touch(DIRA "/Mixed.BIN");
  earn_trust(DIRA);
  assert(dir_cache_lookup(DIRA "/mixed.bin") != DIR_CACHE_ABSENT);
  assert(dir_cache_lookup(DIRA "/MIXED.bin") != DIR_CACHE_ABSENT);
  assert(dir_cache_lookup(DIRA "/Mixed.BIN") == DIR_CACHE_PRESENT);
  unlink(DIRA "/Mixed.BIN");
  printf("case         : Mixed.BIN is never absent as mixed.bin  OK\n");
}

// A name longer than read_dir can hand back must still match itself.
static void test_a_long_name_matches_itself(void) {
  dir_cache_init(&HOST);
  char name[200], p[512];
  memset(name, 'n', sizeof(name) - 1);
  name[sizeof(name) - 1] = 0;
  snprintf(p, sizeof(p), DIRA "/%s", name);
  touch(p);
  earn_trust(DIRA);
  assert(dir_cache_lookup(p) == DIR_CACHE_PRESENT &&
         "a 199-character name is found");
  unlink(p);
  printf("long name    : a name longer than the buffer still matches  OK\n");
}

// A directory that is not there at all is a complete answer -- nothing in it
// exists -- but still only after the card has agreed, since "not there" came
// from stat and the open may resolve the path some other way.
static void test_a_missing_directory_answers_for_everything_in_it(void) {
  dir_cache_init(&HOST);
  int listings = open_dir_calls, asked = stat_calls;
  earn_trust("out/dc_never_made");
  assert(open_dir_calls == listings + 1 && "tried to list it once");
  assert(stat_calls == asked + 1 && "asked once whether the directory exists");
  char p[512];
  for (int i = 0; i < 1000; i++) {
    snprintf(p, sizeof(p), "out/dc_never_made/f%04d.bin", i);
    assert(dir_cache_lookup(p) == DIR_CACHE_ABSENT);
  }
  assert(open_dir_calls == listings + 1 && "never tried again");
  assert(stat_calls == asked + 1);
  DirCacheStats s;
  dir_cache_stats(&s);
  assert(s.missing == 1);
  printf("missing dir  : 1000 probes into a directory that is not there, 1 "
         "question to the card  OK\n");
}

// "ux0:name" lives in "ux0:", not "ux0". The colon is kept, or a directory
// that exists is asked about under a name that does not.
static void test_a_device_root_keeps_its_colon(void) {
  dir_cache_init(&HOST);
  char dir[DC_PATH];
  const char *name = NULL;
  assert(split("ux0:name.txt", dir, &name) == 4);
  assert(strcmp(dir, "ux0:") == 0 && strcmp(name, "name.txt") == 0);
  assert(split("ux0:data/Bully/x.tex", dir, &name) > 0);
  assert(strcmp(dir, "ux0:data/Bully") == 0 && strcmp(name, "x.tex") == 0);
  assert(split("/name", dir, &name) == 1 && strcmp(dir, "/") == 0);
  assert(split("//Android/main.obb", dir, &name) > 0 &&
         strcmp(dir, "//Android") == 0);
  assert(split("bare", dir, &name) == 0);
  printf("device root  : 'ux0:name' is in 'ux0:', '/name' is in '/'  OK\n");
}

// A directory it cannot read must not be turned into "nothing in here exists".
static void test_an_unreadable_directory_falls_back(void) {
  dir_cache_init(&HOST);
  refuse_open_dir = 1;
  char p[512];
  snprintf(p, sizeof(p), DIRA "/a0000.bin");
  assert(dir_cache_lookup(p) == DIR_CACHE_UNKNOWN && "open it and see");
  snprintf(p, sizeof(p), DIRA "/nothere.bin");
  for (int i = 0; i < 10; i++) {
    assert(dir_cache_lookup(p) == DIR_CACHE_UNKNOWN);
    dir_cache_observe(p, 0); // agreement cannot make an unreadable one trusted
  }
  int before = open_dir_calls;
  dir_cache_lookup(p);
  assert(open_dir_calls == before && "the failed listing is not retried");
  refuse_open_dir = 0;
  DirCacheStats s;
  dir_cache_stats(&s);
  assert(s.unreadable == 1);
  printf("unreadable   : answers unknown, does not retry, never says no  OK\n");
}

// More names than a slot can hold. The listing is incomplete, so it may not be
// used to say no.
static void test_an_overfull_directory_never_says_no(void) {
  dir_cache_init(&HOST);
  Dir *d = &dirs[0];
  memset(d, 0, sizeof(*d));
  d->hash = hash_of(DIRB, DC_PATH);
  memcpy(d->path, DIRB, strlen(DIRB) + 1);
  d->complete = 0; // as list_dir marks one that did not fit
  d->verified = DC_VERIFY;
  assert(dir_cache_lookup(DIRB "/b0000.bin") == DIR_CACHE_UNKNOWN);
  assert(dir_cache_lookup(DIRB "/absent.bin") == DIR_CACHE_UNKNOWN);
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
      assert(dir_cache_lookup(p) == DIR_CACHE_PRESENT &&
             "still there after the slot was reused");
      snprintf(p, sizeof(p), "out/dc_m%02d/m9999.bin", i);
      assert(dir_cache_lookup(p) != DIR_CACHE_PRESENT);
    }
  }
  for (int i = 0; i < DC_DIRS + 6; i++) {
    snprintf(dir, sizeof(dir), "out/dc_m%02d", i);
    wipe(dir);
  }
  printf("many dirs    : %d directories through %d slots, every answer right  OK\n",
         DC_DIRS + 6, DC_DIRS);
}

// Once a name cannot be stored, the listing has to stop being trusted, or that
// one name becomes a file that exists and reads as absent.
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

// A file created after the listing was taken. Whoever creates it says so, and
// the listing is dropped -- or it would go on saying "absent" about the file.
static void test_a_write_drops_the_listing(void) {
  dir_cache_init(&HOST);
  earn_trust(DIRA);
  assert(dir_cache_lookup(DIRA "/new.bin") == DIR_CACHE_ABSENT);
  dir_cache_forget(DIRA "/new.bin"); // as the open path does for a "w" open
  touch(DIRA "/new.bin");
  assert(dir_cache_lookup(DIRA "/new.bin") == DIR_CACHE_PRESENT);
  DirCacheStats s;
  dir_cache_stats(&s);
  assert(s.forgotten == 1 && s.listed == 2);
  unlink(DIRA "/new.bin");
  printf("write        : the listing is dropped and the new file found  OK\n");
}

// A path with no directory in it at all must not be mishandled.
static void test_a_bare_name(void) {
  dir_cache_init(&HOST);
  assert(dir_cache_lookup("nosuchfile") == DIR_CACHE_UNKNOWN);
  assert(dir_cache_lookup("") == DIR_CACHE_UNKNOWN);
  assert(dir_cache_lookup(NULL) == DIR_CACHE_UNKNOWN);
  dir_cache_observe("nosuchfile", 1);
  dir_cache_forget("nosuchfile");
  printf("bare name    : a path with no directory is unknown  OK\n");
}

// Before init there is nothing to call. Every answer must be "unknown".
static void test_before_init(void) {
  memset(&io, 0, sizeof(io));
  assert(dir_cache_lookup(DIRA "/a0000.bin") == DIR_CACHE_UNKNOWN);
  dir_cache_observe(DIRA "/a0000.bin", 1);
  dir_cache_forget(DIRA "/a0000.bin");
  printf("before init  : unknown, and nothing is called  OK\n");
}

// Four threads open files while listings come and go under them. A reader
// that catches a slot half cleared answers "absent" for a file that is there.
#define READERS 4
static volatile int stop_readers;
static int reader_false_absent[READERS];

static void *reader(void *arg) {
  int me = (int)(long)arg;
  char p[512];
  unsigned n = 0;
  while (!stop_readers) {
    snprintf(p, sizeof(p), DIRA "/a%04d.bin", (int)(n++ % 300));
    if (dir_cache_lookup(p) == DIR_CACHE_ABSENT)
      reader_false_absent[me]++;
  }
  return NULL;
}

static void test_readers_never_see_a_half_cleared_slot(void) {
  dir_cache_init(&HOST);
  earn_trust(DIRA);
  pthread_t t[READERS];
  stop_readers = 0;
  for (long i = 0; i < READERS; i++)
    pthread_create(&t[i], NULL, reader, (void *)i);
  for (int i = 0; i < 400; i++) {
    dir_cache_forget(DIRA "/anything");
    // Relisting is what clears and refills the slot under the readers.
    dir_cache_lookup(DIRA "/a0000.bin");
  }
  stop_readers = 1;
  int wrong = 0;
  for (int i = 0; i < READERS; i++) {
    pthread_join(t[i], NULL);
    wrong += reader_false_absent[i];
  }
  assert(wrong == 0 && "a present file read as absent during a relisting");
  printf("concurrent   : %d readers through 400 relistings, 0 false absents  OK\n",
         READERS);
}

int main(void) {
  // Start clean: an earlier run that died mid-test leaves its files behind, and
  // a stray new.bin then fails the write test for a reason that is not the code.
  wipe(DIRA);
  wipe(DIRB);
  make(DIRA, 300, "a");
  make(DIRB, 4, "b");

  test_present_files_are_found();
  test_absent_files_are_absent();
  test_one_listing_serves_everything();
  test_a_listing_must_earn_the_right_to_say_no();
  test_case_does_not_make_a_file_absent();
  test_a_long_name_matches_itself();
  test_a_missing_directory_answers_for_everything_in_it();
  test_a_device_root_keeps_its_colon();
  test_an_unreadable_directory_falls_back();
  test_an_overfull_directory_never_says_no();
  test_more_directories_than_slots();
  test_a_name_that_will_not_fit_spoils_the_listing();
  test_a_write_drops_the_listing();
  test_a_bare_name();
  test_readers_never_see_a_half_cleared_slot();
  test_before_init();

  wipe(DIRA);
  wipe(DIRB);
  printf("PASS\n");
  return 0;
}
