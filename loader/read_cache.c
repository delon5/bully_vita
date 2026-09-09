/* read_cache.c -- coalesce the game's small file reads.
 *
 * Measured, on a 324 second session:
 *
 *   io thread: GameMain        75592 ms over 1624 sampled reads   23.3% of wall
 *   io thread: CDStreamThread  53521 ms                           16.5%
 *   io thread: Sound            3409 ms                            1.1%
 *
 * CDStreamThread is the streamer working in the background and costs nothing.
 * GameMain is the game itself stopped on a read, and it is the single largest
 * measured cost anywhere in this port.
 *
 * The reads are latency-bound rather than bandwidth-bound: 26068 of them,
 * 11.0 KB and 5.1 ms each, which is 2.2 MB/s off a card that manages fifteen.
 * 20606 are under 4 KB and 14937 continue from where the last one ended. That
 * shape -- small, mostly sequential, one card round trip each -- is what a
 * read-ahead buffer is for.
 *
 * THE POSITION IS NEVER OURS. This is the whole safety argument.
 *
 * The obvious design tracks the file offset itself and reimplements fseek,
 * ftell and feof on top. That is a rewrite of stdio buffering, and a bug in it
 * hands the game silently wrong bytes out of its archives -- corrupt geometry,
 * corrupt script, and no way to tell from a crash dump which it was.
 *
 * So this never takes ownership. Every call asks ftell where the file actually
 * is, and every call leaves the file exactly where a plain fread would have
 * left it. The buffer is only ever an accelerator: if anything about the
 * position does not match what we expect, the read falls through to the real
 * fread and the answer is identical. fseek, ftell, feof and fclose keep working
 * untouched because nothing here changes what they observe.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdint.h>
#include <string.h>

#include "read_cache.h"

// Four files buffered at once. The game streams from main.obb and patch.obb and
// opens a handful of loose files alongside them; four covers it without holding
// memory this port has repeatedly died for want of.
#define RC_SLOTS 4
#define RC_BUFFER (64 * 1024)

// Above this, a read is already a decent card transaction and reading ahead
// would only move bytes twice.
#define RC_MAX_SERVED (32 * 1024)

typedef struct {
  FILE *file;
  long start; // file offset of buffer[0]
  size_t len; // valid bytes
  unsigned last_used;
  unsigned char buffer[RC_BUFFER];
} Slot;

static Slot slots[RC_SLOTS];

// Where the last read on a file ended, so a refill only happens when the game
// is actually streaming.
//
// Reading ahead after every miss is right for a sequential run and badly wrong
// for a seeking one: the test that seeks before every read did 3729 refills of
// 64 KB -- 238 megabytes off the card -- to serve 385 KB. On a memory card that
// is not a small waste, it is a slowdown. So read ahead only when this read
// began exactly where the last one on the same file ended, which is the only
// evidence there is that more of the same is coming.
#define RC_TRACKED 8
static FILE *seq_file[RC_TRACKED];
static long seq_end[RC_TRACKED];

static unsigned rc_clock;
static ReadCacheOps io;
static unsigned rc_hits, rc_misses, rc_refills;
static size_t rc_bytes_served;

void read_cache_init(const ReadCacheOps *ops) {
  io = *ops;
  memset(slots, 0, sizeof(slots));
  memset(seq_file, 0, sizeof(seq_file));
  memset(seq_end, 0, sizeof(seq_end));
  rc_clock = 0;
  rc_hits = rc_misses = rc_refills = 0;
  rc_bytes_served = 0;
}

void read_cache_forget(FILE *stream) {
  for (int i = 0; i < RC_SLOTS; i++)
    if (slots[i].file == stream)
      slots[i].file = NULL;
  for (int i = 0; i < RC_TRACKED; i++)
    if (seq_file[i] == stream)
      seq_file[i] = NULL;
}

// Whether this read carries on from the last one, and record where it ends.
static int continues_a_run(FILE *stream, long from, long to) {
  int free_slot = -1;
  for (int i = 0; i < RC_TRACKED; i++) {
    if (seq_file[i] == stream) {
      int sequential = seq_end[i] == from;
      seq_end[i] = to;
      return sequential;
    }
    if (!seq_file[i] && free_slot < 0)
      free_slot = i;
  }
  if (free_slot < 0)
    free_slot = (int)((uintptr_t)stream >> 4) % RC_TRACKED;
  seq_file[free_slot] = stream;
  seq_end[free_slot] = to;
  return 0; // first sighting: nothing to continue yet
}

static Slot *slot_for(FILE *stream) {
  for (int i = 0; i < RC_SLOTS; i++)
    if (slots[i].file == stream)
      return &slots[i];
  return NULL;
}

static Slot *slot_to_claim(FILE *stream) {
  Slot *oldest = &slots[0];
  for (int i = 0; i < RC_SLOTS; i++) {
    if (slots[i].file == stream)
      return &slots[i];
    if (!slots[i].file)
      return &slots[i];
    if (slots[i].last_used < oldest->last_used)
      oldest = &slots[i];
  }
  return oldest;
}

// Fill a slot from `at`, and put the file back where it was. Best effort: a
// short read is fine, a failed one just leaves the slot empty.
static void refill(FILE *stream, long at) {
  Slot *s = slot_to_claim(stream);
  size_t got = io.fread(s->buffer, 1, RC_BUFFER, stream);
  if (io.fseek(stream, at, SEEK_SET) != 0) {
    // Could not restore the position, so the file is no longer where the caller
    // believes it is. Nothing may be served from this and nothing may be
    // trusted about the position; drop the slot and let the next read find out
    // from ftell.
    s->file = NULL;
    return;
  }
  if (!got) {
    s->file = NULL;
    return;
  }
  s->file = stream;
  s->start = at;
  s->len = got;
  s->last_used = ++rc_clock;
  rc_refills++;
}

size_t read_cache_fread(void *ptr, size_t size, size_t count, FILE *stream) {
  if (!size || !count)
    return 0;
  size_t want = size * count;

  long cur = io.ftell(stream);
  if (cur < 0) // no idea where we are, so no idea what to serve
    return io.fread(ptr, size, count, stream);

  Slot *s = slot_for(stream);
  if (s && want <= RC_MAX_SERVED && cur >= s->start &&
      (size_t)(cur - s->start) + want <= s->len) {
    memcpy(ptr, s->buffer + (cur - s->start), want);
    // Leave the file exactly where a real fread would have. If that fails the
    // caller's next ftell would disagree with reality, so fall back rather than
    // report a success we cannot stand behind.
    if (io.fseek(stream, cur + (long)want, SEEK_SET) == 0) {
      s->last_used = ++rc_clock;
      rc_hits++;
      rc_bytes_served += want;
      continues_a_run(stream, cur, cur + (long)want);
      return count;
    }
    s->file = NULL;
  }

  rc_misses++;
  size_t got = io.fread(ptr, size, count, stream);
  long now = io.ftell(stream);
  int streaming = continues_a_run(stream, cur, now);
  // Read ahead only for a small read that continued a run and got everything it
  // asked for. A short read is the end of the file, and there is nothing ahead
  // of it worth holding.
  if (streaming && got == count && want <= RC_MAX_SERVED && now >= 0)
    refill(stream, now);
  return got;
}

void read_cache_stats(ReadCacheStats *out) {
  out->hits = rc_hits;
  out->misses = rc_misses;
  out->refills = rc_refills;
  out->bytes_served_kb = (unsigned)(rc_bytes_served / 1024);
}
