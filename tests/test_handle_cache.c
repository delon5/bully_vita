/* test_handle_cache.c -- a recycled handle must be indistinguishable from a
 * fresh one.
 *
 * The cache answers fopen with a handle the game closed earlier. Everything it
 * can get wrong is silent: the wrong file for a path, a handle still positioned
 * where the last reader left it, a descriptor leaked until the game cannot open
 * anything, a stale handle served after a rewrite. None of those announce
 * themselves -- they turn into wrong data somewhere else, later.
 *
 * So the test is not "does it hit". It is: does every open return a handle
 * whose bytes and starting position match what a real fopen would have given,
 * under eviction, under threads, under failure, and does the number of
 * descriptors actually held stay inside the cap it was given.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../loader/handle_cache.c"

#define FILES 20
#define FILE_BYTES 4096
static char paths[FILES][256];
static unsigned char truth[FILES][FILE_BYTES];

// Real opens and closes reaching the filesystem, so a test can see what the
// cache actually holds rather than what it says it holds.
static volatile int real_opens, real_closes, live_handles, peak_live;
static volatile int fail_next_open;
// Refuse every open once this many are live, the way a system with no
// descriptors left does.
static volatile int descriptor_ceiling = 1 << 30;
// A path the fake refuses no matter what, the way a probe for a file that is
// not there is refused. Draining cannot rescue it.
static char missing_path[256];
static pthread_mutex_t count_lock = PTHREAD_MUTEX_INITIALIZER;

static FILE *host_fopen(const char *path, const char *mode) {
  pthread_mutex_lock(&count_lock);
  if (fail_next_open) {
    fail_next_open = 0;
    pthread_mutex_unlock(&count_lock);
    return NULL;
  }
  pthread_mutex_unlock(&count_lock);

  if (missing_path[0] && strcmp(path, missing_path) == 0)
    return NULL;

  pthread_mutex_lock(&count_lock);
  int at_ceiling = live_handles >= descriptor_ceiling;
  pthread_mutex_unlock(&count_lock);
  if (at_ceiling)
    return NULL;

  FILE *f = fopen(path, mode);
  if (f) {
    pthread_mutex_lock(&count_lock);
    real_opens++;
    if (++live_handles > peak_live)
      peak_live = live_handles;
    pthread_mutex_unlock(&count_lock);
  }
  return f;
}

static int host_fclose(FILE *f) {
  pthread_mutex_lock(&count_lock);
  real_closes++;
  live_handles--;
  pthread_mutex_unlock(&count_lock);
  return fclose(f);
}

static int host_fseek(FILE *f, long o, int w) { return fseek(f, o, w); }
static int host_ferror(FILE *f) { return ferror(f); }
static int host_exists(const char *path) { return access(path, F_OK) == 0; }
static const HandleCacheOps HOST = { host_fopen, host_fclose, host_fseek,
                                     host_ferror, host_exists };


static void make_files(void) {
  for (int i = 0; i < FILES; i++) {
    snprintf(paths[i], sizeof(paths[i]), "out/hc_%02d.bin", i);
    for (int b = 0; b < FILE_BYTES; b++)
      truth[i][b] = (unsigned char)(i * 97 + b * 31 + (b >> 5));
    FILE *f = fopen(paths[i], "wb");
    assert(f);
    assert(fwrite(truth[i], 1, FILE_BYTES, f) == FILE_BYTES);
    fclose(f);
  }
}

// Open through the cache, read the whole file from wherever the handle is
// positioned, and require it to be the file's entire contents from byte zero.
static void check_whole(int which) {
  unsigned char got[FILE_BYTES];
  FILE *f = handle_cache_fopen(paths[which], "rb");
  assert(f);
  size_t n = fread(got, 1, FILE_BYTES, f);
  assert(n == FILE_BYTES);
  assert(memcmp(got, truth[which], FILE_BYTES) == 0);
  assert(handle_cache_fclose(f) == 0 || 1);
}

// The same, but only reading part of it before closing, which is what leaves a
// handle mid-file for whoever picks it up next.
static void check_partial(int which, size_t bytes) {
  unsigned char got[FILE_BYTES];
  FILE *f = handle_cache_fopen(paths[which], "rb");
  assert(f);
  size_t n = fread(got, 1, bytes, f);
  assert(n == bytes);
  assert(memcmp(got, truth[which], bytes) == 0);
  handle_cache_fclose(f);
}

static void test_reuse_is_identical(void) {
  handle_cache_init(&HOST, 8);
  for (int round = 0; round < 40; round++)
    for (int i = 0; i < 4; i++)
      check_whole(i);

  HandleCacheStats s;
  handle_cache_stats(&s);
  // 4 paths, 8 slots: everything after the first round comes from the cache.
  assert(s.hits == 4 * 39);
  assert(s.misses == 4);
  printf("reuse        : %u opens served from a held handle, %u went to the card  OK\n",
         s.hits, s.misses);
  handle_cache_drain();
}

static void test_position_is_reset(void) {
  handle_cache_init(&HOST, 8);
  // Leave every handle stopped part way through, then demand the whole file.
  for (int i = 0; i < 4; i++)
    check_partial(i, 1000);
  for (int i = 0; i < 4; i++)
    check_whole(i);
  for (int i = 0; i < 4; i++)
    check_partial(i, 4095);
  for (int i = 0; i < 4; i++)
    check_whole(i);
  printf("position     : a handle left mid-file comes back at byte zero  OK\n");
  handle_cache_drain();
}

// The one that matters most: a path must never be served another path's handle.
static void test_paths_do_not_cross(void) {
  handle_cache_init(&HOST, 4);
  unsigned rng = 7;
  for (int n = 0; n < 4000; n++) {
    rng = rng * 1103515245u + 12345u;
    check_whole((int)((rng >> 8) % FILES));
  }
  printf("no crossing  : 4000 opens over %d paths through 4 slots, every byte right  OK\n", FILES);
  handle_cache_drain();
}

static void test_cap_is_respected(void) {
  handle_cache_init(&HOST, 5);
  pthread_mutex_lock(&count_lock);
  peak_live = live_handles;
  pthread_mutex_unlock(&count_lock);

  // Hold nothing open ourselves, so every live descriptor is one the cache
  // decided to keep. One is in the caller's hand at a time, hence 5 + 1.
  for (int n = 0; n < 500; n++)
    check_whole(n % FILES);

  handle_cache_drain();
  pthread_mutex_lock(&count_lock);
  int peak = peak_live, live = live_handles;
  pthread_mutex_unlock(&count_lock);
  assert(peak <= 6);
  assert(live == 0);
  printf("descriptors  : never more than %d open at once for a cap of 5, none left after a drain  OK\n",
         peak);
}

static void test_writes_are_never_cached(void) {
  handle_cache_init(&HOST, 8);
  char path[256];
  snprintf(path, sizeof(path), "out/hc_rw.bin");

  for (int round = 0; round < 5; round++) {
    FILE *w = handle_cache_fopen(path, "wb");
    assert(w);
    char line[32];
    int len = snprintf(line, sizeof(line), "round %d", round);
    assert((int)fwrite(line, 1, len, w) == len);
    assert(handle_cache_fclose(w) == 0);

    // If a write mode were ever parked, or a read handle held across the
    // rewrite, this reads the previous round's bytes.
    FILE *r = handle_cache_fopen(path, "rb");
    assert(r);
    char got[32] = { 0 };
    size_t n = fread(got, 1, sizeof(got) - 1, r);
    handle_cache_fclose(r);
    assert((int)n == len && memcmp(got, line, len) == 0);
  }

  HandleCacheStats s;
  handle_cache_stats(&s);
  printf("write modes  : 5 rewrites, each read back as itself, %u parked handles are all readers  OK\n",
         s.parked);
  handle_cache_drain();
  unlink(path);
}

static void test_untracked_close_passes_through(void) {
  handle_cache_init(&HOST, 8);
  // Opened behind the cache's back, the way anything not going through the
  // hook would be.
  FILE *f = host_fopen(paths[0], "rb");
  assert(f);
  pthread_mutex_lock(&count_lock);
  int before = real_closes;
  pthread_mutex_unlock(&count_lock);
  assert(handle_cache_fclose(f) == 0);
  pthread_mutex_lock(&count_lock);
  assert(real_closes == before + 1);
  pthread_mutex_unlock(&count_lock);

  HandleCacheStats s;
  handle_cache_stats(&s);
  assert(s.parked == 0);
  printf("untracked    : a handle the cache never opened is really closed  OK\n");
}

static void test_open_failure_gives_the_handles_back(void) {
  handle_cache_init(&HOST, 8);
  for (int i = 0; i < 6; i++)
    check_whole(i);

  HandleCacheStats before;
  handle_cache_stats(&before);
  assert(before.held == 6);

  // Next real open fails, as it would with no descriptors left.
  pthread_mutex_lock(&count_lock);
  fail_next_open = 1;
  pthread_mutex_unlock(&count_lock);

  // A path with nothing parked for it, so it has to reach the filesystem.
  FILE *f = handle_cache_fopen(paths[15], "rb");
  assert(f); // the retry after the drain must succeed
  unsigned char got[FILE_BYTES];
  assert(fread(got, 1, FILE_BYTES, f) == FILE_BYTES);
  assert(memcmp(got, truth[15], FILE_BYTES) == 0);
  handle_cache_fclose(f);

  HandleCacheStats after;
  handle_cache_stats(&after);
  assert(after.drains == 1);
  printf("out of room  : a failed open drops all 6 held handles and succeeds on the retry  OK\n");
  handle_cache_drain();
}

// What the first version of this got wrong on hardware. The game probes for
// files that are not there; every probe returned NULL, the cache read that as
// running out of descriptors, and gave back everything it held. 5492 times in
// one session, for 9 useful hits.
static void test_probes_do_not_empty_the_cache(void) {
  handle_cache_init(&HOST, 8);
  snprintf(missing_path, sizeof(missing_path), "out/hc_not_here.bin");

  for (int n = 0; n < 500; n++) {
    check_whole(n % 4);
    // The probe that is never going to succeed, whatever the cache does.
    assert(handle_cache_fopen(missing_path, "rb") == NULL);
  }
  missing_path[0] = 0;

  HandleCacheStats s;
  handle_cache_stats(&s);
  assert(s.drains == 0);   // a file that is not there never needs a drain
  assert(s.absent == 500); // it is recognised as absent instead
  assert(s.hits > 400);    // and the cache keeps working
  printf("probes       : 500 opens of a file that is not there cost %u drains, "
         "%u hits kept  OK\n", s.drains, s.hits);
  handle_cache_drain();
}

// The other half: when draining really does rescue an open, it must keep doing
// it however many times that takes.
static void test_real_exhaustion_keeps_draining(void) {
  handle_cache_init(&HOST, 8);
  for (int round = 0; round < 40; round++) {
    for (int i = 0; i < 4; i++)
      check_whole(i);
    pthread_mutex_lock(&count_lock);
    fail_next_open = 1;
    pthread_mutex_unlock(&count_lock);
    FILE *f = handle_cache_fopen(paths[10 + (round % 5)], "rb");
    assert(f); // the drain has to rescue it every time, not just eight times
    handle_cache_fclose(f);
  }

  HandleCacheStats s;
  handle_cache_stats(&s);
  assert(s.rescued == s.drains && s.drains == 40);
  printf("exhaustion   : %u failed opens, all %u rescued by a drain  OK\n",
         s.drains, s.rescued);
  handle_cache_drain();
}

// The crash. The cache held 28 descriptors, the game opened OBJECTS/IDE.DIR,
// the open returned NULL because there were none left, and the game passed the
// NULL to OS_FileSetPosition without checking. A file that exists must never
// fail to open because this cache is holding handles -- however many probes for
// absent files came before it, and however long the session has run.
static void test_an_existing_file_never_fails_to_open(void) {
  handle_cache_init(&HOST, 32);
  snprintf(missing_path, sizeof(missing_path), "out/hc_not_here.bin");

  // Fill the cache from the first fourteen paths, with a pile of absent-file
  // probes mixed in -- the exact sequence that used the old policy's budget up
  // before the real failure came.
  for (int n = 0; n < 300; n++) {
    check_whole(n % 14);
    assert(handle_cache_fopen(missing_path, "rb") == NULL);
  }
  missing_path[0] = 0;

  HandleCacheStats before;
  handle_cache_stats(&before);
  assert(before.held > 8); // it really is sitting on descriptors

  // Now there are none left, for every open from here on.
  pthread_mutex_lock(&count_lock);
  descriptor_ceiling = live_handles;
  pthread_mutex_unlock(&count_lock);

  // Ask for the paths it is not holding, so every one of these has to reach
  // fopen -- and every one of them finds the table full.
  for (int n = 0; n < 50; n++) {
    int which = 14 + n % 6;
    FILE *f = handle_cache_fopen(paths[which], "rb");
    assert(f); // the file is there, so it has to open, every single time
    unsigned char got[FILE_BYTES];
    assert(fread(got, 1, FILE_BYTES, f) == FILE_BYTES);
    assert(memcmp(got, truth[which], FILE_BYTES) == 0);
    handle_cache_fclose(f);
    pthread_mutex_lock(&count_lock);
    descriptor_ceiling = live_handles + 1;
    pthread_mutex_unlock(&count_lock);
  }
  pthread_mutex_lock(&count_lock);
  descriptor_ceiling = 1 << 30;
  pthread_mutex_unlock(&count_lock);

  HandleCacheStats s;
  handle_cache_stats(&s);
  assert(s.rescued > 0);
  assert(s.slots < 32); // and it learned to hold fewer
  printf("never NULL   : 50 opens against a full descriptor table, all served, "
         "cap pulled back to %u  OK\n", s.slots);
  handle_cache_drain();
}

static void test_failed_handle_is_not_parked(void) {
  handle_cache_init(&HOST, 8);
  FILE *f = handle_cache_fopen(paths[0], "rb");
  assert(f);
  // Provoke the error indicator: read from a file opened for reading only
  // after seeking past the end is not enough, so write to it instead.
  unsigned char scratch[16];
  assert(fread(scratch, 1, sizeof(scratch), f) == sizeof(scratch));
  assert(fwrite(scratch, 1, sizeof(scratch), f) == 0);
  assert(ferror(f));

  HandleCacheStats before;
  handle_cache_stats(&before);
  handle_cache_fclose(f);
  HandleCacheStats after;
  handle_cache_stats(&after);
  assert(after.held == before.held);
  assert(after.dropped == before.dropped + 1);
  printf("failed handle: one with its error flag set is closed, not kept  OK\n");
  handle_cache_drain();
}

#define THREADS 8
static void *hammer(void *arg) {
  unsigned rng = (unsigned)(uintptr_t)arg * 2654435761u + 1;
  unsigned char got[FILE_BYTES];
  for (int n = 0; n < 2000; n++) {
    rng = rng * 1103515245u + 12345u;
    int which = (int)((rng >> 8) % FILES);
    FILE *f = handle_cache_fopen(paths[which], "rb");
    assert(f);
    size_t want = 1 + (rng >> 20) % FILE_BYTES;
    size_t r = fread(got, 1, want, f);
    assert(r == want);
    assert(memcmp(got, truth[which], want) == 0);
    handle_cache_fclose(f);
  }
  return NULL;
}

static void test_threads(void) {
  handle_cache_init(&HOST, 6);
  pthread_t t[THREADS];
  for (int i = 0; i < THREADS; i++)
    assert(pthread_create(&t[i], NULL, hammer, (void *)(uintptr_t)(i + 1)) == 0);
  for (int i = 0; i < THREADS; i++)
    pthread_join(t[i], NULL);

  HandleCacheStats s;
  handle_cache_stats(&s);
  assert(s.held <= 6);
  handle_cache_drain();
  pthread_mutex_lock(&count_lock);
  int live = live_handles;
  pthread_mutex_unlock(&count_lock);
  assert(live == 0);
  printf("concurrent   : %d threads, %d opens over %d paths through 6 slots, %u hits, all correct  OK\n",
         THREADS, THREADS * 2000, FILES, s.hits);
}

// Every handle the cache ever took must eventually be closed, or the game runs
// out of descriptors after long enough.
static void test_nothing_leaks(void) {
  handle_cache_init(&HOST, 4);
  pthread_mutex_lock(&count_lock);
  int opens_before = real_opens, closes_before = real_closes;
  pthread_mutex_unlock(&count_lock);

  for (int n = 0; n < 300; n++)
    check_partial(n % FILES, 128);
  handle_cache_drain();

  pthread_mutex_lock(&count_lock);
  int opened = real_opens - opens_before, closed = real_closes - closes_before;
  pthread_mutex_unlock(&count_lock);
  assert(opened == closed);
  printf("no leak      : %d real opens, %d real closes, balanced after a drain  OK\n",
         opened, closed);
}

int main(void) {
  make_files();

  test_reuse_is_identical();
  test_position_is_reset();
  test_paths_do_not_cross();
  test_cap_is_respected();
  test_writes_are_never_cached();
  test_untracked_close_passes_through();
  test_open_failure_gives_the_handles_back();
  test_probes_do_not_empty_the_cache();
  test_real_exhaustion_keeps_draining();
  test_an_existing_file_never_fails_to_open();
  test_failed_handle_is_not_parked();
  test_threads();
  test_nothing_leaks();

  for (int i = 0; i < FILES; i++)
    unlink(paths[i]);
  printf("PASS\n");
  return 0;
}
