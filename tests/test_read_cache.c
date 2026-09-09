/* test_read_cache.c -- the read cache must be indistinguishable from fread.
 *
 * This is the one piece of the loader that can hand the game silently wrong
 * bytes. A texture cache bug shows up as a wrong picture; a read cache bug
 * shows up as corrupt geometry, or a script that does something impossible,
 * three minutes later and nowhere near the cause. So the test is not "does it
 * hit" -- it is "is every byte, every return value and every resulting file
 * position identical to what plain fread would have produced", over patterns
 * chosen to break it.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../loader/read_cache.c"

#define FILE_BYTES (1024 * 1024)
static unsigned char truth[FILE_BYTES];
static char path_a[256], path_b[256];

static size_t host_fread(void *p, size_t s, size_t n, FILE *f) { return fread(p, s, n, f); }
static int host_fseek(FILE *f, long o, int w) { return fseek(f, o, w); }
static long host_ftell(FILE *f) { return ftell(f); }
static const ReadCacheOps HOST = { host_fread, host_fseek, host_ftell };

// A cheap deterministic generator, so a failure is reproducible.
static unsigned rng_state = 12345;
static unsigned rng(void) {
  rng_state = rng_state * 1103515245u + 12345u;
  return rng_state >> 8;
}

static void make_file(const char *path) {
  for (int i = 0; i < FILE_BYTES; i++)
    truth[i] = (unsigned char)(i * 31 + (i >> 8) * 7);
  FILE *f = fopen(path, "wb");
  assert(f);
  assert(fwrite(truth, 1, FILE_BYTES, f) == FILE_BYTES);
  fclose(f);
}

// Run the same script of operations through the cache and through plain fread,
// and require that they agree on everything observable.
static void compare(const char *name, int rounds, int max_read, int seek_every) {
  FILE *cached = fopen(path_a, "rb");
  FILE *plain = fopen(path_b, "rb");
  assert(cached && plain);
  read_cache_init(&HOST);

  unsigned char got_a[70000], got_b[70000];
  rng_state = 999;

  for (int i = 0; i < rounds; i++) {
    if (seek_every && i % seek_every == 0) {
      long to = (long)(rng() % (FILE_BYTES - 1));
      assert(fseek(cached, to, SEEK_SET) == 0);
      assert(fseek(plain, to, SEEK_SET) == 0);
    }

    size_t size = 1 + rng() % 4;
    size_t count = 1 + rng() % (max_read / size);
    if (size * count > sizeof(got_a))
      count = sizeof(got_a) / size;

    memset(got_a, 0xAA, size * count);
    memset(got_b, 0x55, size * count);

    // Taken before the read, not derived from the position after it. On a short
    // read C leaves the file position indeterminate within the partial element,
    // so end-minus-bytes is not the start -- which is what the first version of
    // this test tripped over, against a cache that was agreeing with fread
    // perfectly.
    long began = ftell(cached);

    size_t ra = read_cache_fread(got_a, size, count, cached);
    size_t rb = fread(got_b, size, count, plain);

    assert(ra == rb && "the cache must return exactly what fread returns");
    assert(memcmp(got_a, got_b, ra * size) == 0 && "and exactly the same bytes");
    assert(ftell(cached) == ftell(plain) && "and leave the file in the same place");

    if (began >= 0 && ra)
      assert(memcmp(got_a, truth + began, ra * size) == 0 &&
             "which must be the bytes actually in the file");

    // Back to the start on EOF. Without this the sequential case spent most of
    // its rounds sitting past the end of a 1 MB file reading nothing, and
    // reported a hit rate that measured that rather than the cache.
    if (ra < count) {
      assert(fseek(cached, 0, SEEK_SET) == 0);
      assert(fseek(plain, 0, SEEK_SET) == 0);
    }
  }

  ReadCacheStats st;
  read_cache_stats(&st);
  printf("%-13s: %u hits, %u misses, %u refills, %u KB served  OK\n", name, st.hits, st.misses,
         st.refills, st.bytes_served_kb);
  read_cache_forget(cached);
  fclose(cached);
  fclose(plain);
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  const char *dir = getenv("TEXCACHE_DIR");
  snprintf(path_a, sizeof(path_a), "%s/rc_a.bin", dir ? dir : ".");
  snprintf(path_b, sizeof(path_b), "%s/rc_b.bin", dir ? dir : ".");
  make_file(path_a);
  make_file(path_b);

  // The case it exists for: small reads walking forward.
  compare("sequential", 4000, 3000, 0);
  // Seeks between reads, which is what invalidates a buffer.
  compare("seek+read", 4000, 3000, 1);
  // Occasional seeks, the real mix -- 57% of the game's reads are sequential.
  compare("mixed", 4000, 3000, 3);
  // Reads far bigger than the buffer, which must pass straight through.
  compare("large reads", 400, 65000, 5);
  // Tiny reads, where the per-call overhead matters most.
  compare("tiny reads", 6000, 8, 0);

  // Reading off the end must report exactly what fread reports.
  {
    FILE *a = fopen(path_a, "rb"), *b = fopen(path_b, "rb");
    read_cache_init(&HOST);
    assert(fseek(a, FILE_BYTES - 100, SEEK_SET) == 0);
    assert(fseek(b, FILE_BYTES - 100, SEEK_SET) == 0);
    unsigned char x[500], y[500];
    size_t ra = read_cache_fread(x, 1, 500, a);
    size_t rb = fread(y, 1, 500, b);
    assert(ra == rb && ra == 100);
    assert(memcmp(x, y, ra) == 0);
    assert(ftell(a) == ftell(b));
    assert(read_cache_fread(x, 1, 10, a) == 0 && "past the end stays past the end");
    assert(ftell(a) == ftell(b));
    printf("end of file  : short read and EOF match fread exactly    OK\n");
    read_cache_forget(a);
    fclose(a);
    fclose(b);
  }

  // A closed FILE * whose address is handed back by a later fopen must not be
  // served from the dead file's buffer.
  {
    read_cache_init(&HOST);
    FILE *f = fopen(path_a, "rb");
    unsigned char x[64];
    read_cache_fread(x, 1, 64, f);
    read_cache_forget(f);
    fclose(f);
    for (int i = 0; i < RC_SLOTS; i++)
      assert(slots[i].file != f && "a closed file may keep no buffer");
    printf("reuse guard  : closing a file drops its buffer          OK\n");
  }

  // More files open at once than there are slots: correctness must not depend
  // on winning a slot.
  {
    read_cache_init(&HOST);
    FILE *f[RC_SLOTS + 3];
    for (int i = 0; i < RC_SLOTS + 3; i++) {
      f[i] = fopen(path_a, "rb");
      assert(f[i]);
    }
    for (int round = 0; round < 200; round++) {
      for (int i = 0; i < RC_SLOTS + 3; i++) {
        long at = (long)((round * 977 + i * 4099) % (FILE_BYTES - 1000));
        assert(fseek(f[i], at, SEEK_SET) == 0);
        unsigned char x[300];
        size_t r = read_cache_fread(x, 1, 300, f[i]);
        assert(r == 300);
        assert(memcmp(x, truth + at, 300) == 0 && "every file must read its own bytes");
        assert(ftell(f[i]) == at + 300);
      }
    }
    for (int i = 0; i < RC_SLOTS + 3; i++) {
      read_cache_forget(f[i]);
      fclose(f[i]);
    }
    printf("more files   : %d files through %d slots, all correct    OK\n", RC_SLOTS + 3, RC_SLOTS);
  }

  remove(path_a);
  remove(path_b);
  printf("PASS\n");
  return 0;
}
