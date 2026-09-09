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
 * FOUR THREADS READ AT ONCE, AND EACH OWNS ITS BUFFERS.
 *
 * The first version of this shipped with none of that and corrupted the game on
 * launch. GameMain, CDStreamThread, Sound and avPlayer all call fread, and
 * refill() overwrote a slot's buffer while that slot's file, start and len
 * still described the previous occupant -- so another thread reading the
 * previous file matched the slot and memcpy'd out of a buffer being rewritten
 * underneath it. The game read a string length of 1684633471 out of the
 * wreckage and asked for 1.6 GB.
 *
 * A lock would fix it and would also put four threads in a queue on the hottest
 * path in the process, which is the mistake the allocation tracer already made
 * once here. Instead every slot belongs to exactly one thread. A thread only
 * ever reads or writes its own slots, so there is nothing to serialise. The one
 * cross-thread write is claiming an unowned slot, which happens a handful of
 * times at startup and goes through a compare-and-exchange; a thread that finds
 * no slot of its own simply reads through uncached, which is always correct.
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
// Eight slots across all threads: enough for the two archives on the thread
// that matters plus whatever the background threads touch.
#define RC_SLOTS 8
#define RC_BUFFER (64 * 1024)

// Above this, a read is already a decent card transaction and reading ahead
// would only move bytes twice.
#define RC_MAX_SERVED (32 * 1024)

// How much to read ahead, sized to the run the game is actually doing.
//
// The first version always fetched the full 64 KB, and on hardware that cost
// 263 MB of card reads to serve 19 MB: 4211 read aheads, 1.7 hits each, 4.7 KB
// of every 64 KB used. Reading went from 124 s to 191 s and GameMain's share
// doubled. The game's sequential runs are about two reads long, so a fixed
// fetch that big can only ever be waste.
//
// Twice the run so far, so a run that keeps going keeps earning more, and one
// that stops after two reads costs almost nothing.
#define RC_MIN_AHEAD (8 * 1024)

typedef struct {
  unsigned owner; // the only thread that may touch the rest of this
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
#define RC_TRACKED 16
static unsigned seq_owner[RC_TRACKED];
static FILE *seq_file[RC_TRACKED];
static long seq_end[RC_TRACKED];
static long seq_run[RC_TRACKED]; // bytes read consecutively in the current run

static unsigned rc_clock;
static ReadCacheOps io;
static unsigned rc_hits, rc_misses, rc_refills, rc_unowned;
static size_t rc_fetched; // bytes pulled off the card by read-ahead
static size_t rc_bytes_served;

void read_cache_init(const ReadCacheOps *ops) {
  io = *ops;
  memset(slots, 0, sizeof(slots));
  memset(seq_owner, 0, sizeof(seq_owner));
  memset(seq_file, 0, sizeof(seq_file));
  memset(seq_end, 0, sizeof(seq_end));
  memset(seq_run, 0, sizeof(seq_run));
  rc_clock = 0;
  rc_hits = rc_misses = rc_refills = rc_unowned = 0;
  rc_fetched = 0;
  rc_bytes_served = 0;
}

void read_cache_forget(FILE *stream) {
  // Any thread may close a file another thread buffered, so this is the one
  // place a slot is cleared from outside its owner. Clearing `file` only ever
  // turns a hit into a miss, which is safe from any thread.
  for (int i = 0; i < RC_SLOTS; i++)
    if (slots[i].file == stream)
      slots[i].file = NULL;
  for (int i = 0; i < RC_TRACKED; i++)
    if (seq_file[i] == stream)
      seq_file[i] = NULL;
}

// A thread that will never read again should not hold entries for ever. Nothing
// calls this yet -- the four reading threads live for the whole session -- but
// the tables are small enough that a leak in them is what disabled the cache
// once already.
void read_cache_release(unsigned thread) {
  for (int i = 0; i < RC_SLOTS; i++)
    if (slots[i].owner == thread) {
      slots[i].file = NULL;
      __atomic_store_n(&slots[i].owner, 0u, __ATOMIC_RELEASE);
    }
  for (int i = 0; i < RC_TRACKED; i++)
    if (seq_owner[i] == thread) {
      seq_file[i] = NULL;
      __atomic_store_n(&seq_owner[i], 0u, __ATOMIC_RELEASE);
    }
}

// Whether this read carries on from the last one on the same file and thread,
// and record where it ends.
static size_t ahead_for(long run) {
  size_t ahead = (size_t)(run * 2);
  if (ahead < RC_MIN_AHEAD)
    ahead = RC_MIN_AHEAD;
  if (ahead > RC_BUFFER - RC_MAX_SERVED)
    ahead = RC_BUFFER - RC_MAX_SERVED;
  return ahead;
}

// How long the run on this file would be if a read started at `from`, without
// recording anything. The combined fetch has to know before it reads.
static long peek_run(unsigned me, FILE *stream, long from) {
  for (int i = 0; i < RC_TRACKED; i++)
    if (seq_owner[i] == me && seq_file[i] == stream)
      return seq_end[i] == from ? seq_run[i] + 1 : 0;
  return 0;
}

// Returns how many bytes the current run has covered, or 0 if this read did not
// continue one. The size of the next read-ahead comes from that.
static long note_run(unsigned me, FILE *stream, long from, long to) {
  int mine = -1, unclaimed = -1;
  for (int i = 0; i < RC_TRACKED; i++) {
    if (seq_owner[i] == me) {
      if (seq_file[i] == stream) {
        if (seq_end[i] == from)
          seq_run[i] += to - from;
        else
          seq_run[i] = 0; // the run broke; start counting again
        seq_end[i] = to;
        return seq_run[i];
      }
      // Already ours and holding nothing: reuse it directly. Looking only for
      // entries with no owner at all is what broke this on hardware -- a closed
      // file left its entry owned but empty, so after sixteen files had been
      // opened and closed there was nothing left an exchange could claim, and
      // the cache served 0 of 24196 reads for the rest of the session.
      if (!seq_file[i] && mine < 0)
        mine = i;
    } else if (!seq_owner[i] && unclaimed < 0) {
      unclaimed = i;
    }
  }

  int at = mine;
  if (at < 0) {
    if (unclaimed < 0)
      return 0; // nothing to track this with; never read ahead on a guess
    unsigned unowned = 0;
    if (!__atomic_compare_exchange_n(&seq_owner[unclaimed], &unowned, me, 0, __ATOMIC_ACQ_REL,
                                     __ATOMIC_RELAXED))
      return 0; // lost the race; the next read will find one
    at = unclaimed;
  }

  seq_file[at] = stream;
  seq_end[at] = to;
  seq_run[at] = 0;
  return 0; // first sighting of this file: nothing to continue yet
}

static Slot *slot_for(unsigned me, FILE *stream) {
  for (int i = 0; i < RC_SLOTS; i++)
    if (slots[i].owner == me && slots[i].file == stream)
      return &slots[i];
  return NULL;
}

// A slot this thread may write. Never steals another thread's -- a thread with
// none of its own reads through uncached, which costs nothing but a miss.
static Slot *slot_to_claim(unsigned me, FILE *stream) {
  Slot *oldest = NULL;
  for (int i = 0; i < RC_SLOTS; i++) {
    if (slots[i].owner == me) {
      if (slots[i].file == stream)
        return &slots[i];
      if (!oldest || slots[i].last_used < oldest->last_used)
        oldest = &slots[i];
    }
  }
  if (oldest)
    return oldest;

  // Nothing owned yet: take an unowned slot. Contended only in the first
  // moments of a run, so the exchange costs nothing worth measuring.
  for (int i = 0; i < RC_SLOTS; i++) {
    unsigned unowned = 0;
    if (__atomic_compare_exchange_n(&slots[i].owner, &unowned, me, 0, __ATOMIC_ACQ_REL,
                                    __ATOMIC_RELAXED)) {
      slots[i].file = NULL;
      return &slots[i];
    }
  }
  rc_unowned++;
  return NULL;
}

size_t read_cache_fread(void *ptr, size_t size, size_t count, FILE *stream) {
  if (!size || !count)
    return 0;
  size_t want = size * count;

  long cur = io.ftell(stream);
  if (cur < 0) // no idea where we are, so no idea what to serve
    return io.fread(ptr, size, count, stream);

  unsigned me = io.thread_id();
  Slot *s = slot_for(me, stream);
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
      note_run(me, stream, cur, cur + (long)want);
      return count;
    }
    s->file = NULL;
  }

  rc_misses++;

  // A miss in the middle of a run: fetch what the game asked for and what it is
  // about to ask for in ONE card read, rather than doing the game's read and
  // then a second read of our own.
  //
  // The first version did the two separately, and that is why it lost. A run of
  // three reads became read, read, fetch, hit -- three trips to the card to
  // serve three reads, exactly what it cost without a cache, plus the extra
  // bytes. Combined, the same run is read, read-and-fetch, hit: two trips.
  long run = peek_run(me, stream, cur);
  if (run > 0 && want <= RC_MAX_SERVED) {
    Slot *slot = slot_to_claim(me, stream);
    if (slot) {
      size_t ahead = ahead_for(run);
      slot->file = NULL; // disown before the bytes move
      size_t fetched = io.fread(slot->buffer, 1, want + ahead, stream);
      rc_fetched += fetched > want ? fetched - want : 0;

      size_t deliver = fetched < want ? fetched : want;
      memcpy(ptr, slot->buffer, deliver);
      // Where a plain fread would have left it: after the bytes it delivered.
      if (io.fseek(stream, cur + (long)deliver, SEEK_SET) == 0) {
        if (fetched > deliver) {
          slot->file = stream;
          slot->start = cur;
          slot->len = fetched;
          slot->last_used = ++rc_clock;
          rc_refills++;
        }
        note_run(me, stream, cur, cur + (long)deliver);
        return deliver / size;
      }
      // The position could not be restored, so nothing here can be trusted.
      // Fall through and let the plain path do it properly.
      io.fseek(stream, cur, SEEK_SET);
    }
  }

  size_t got = io.fread(ptr, size, count, stream);
  long now = io.ftell(stream);
  note_run(me, stream, cur, now);
  return got;
}

void read_cache_stats(ReadCacheStats *out) {
  out->hits = rc_hits;
  out->misses = rc_misses;
  out->refills = rc_refills;
  out->bytes_served_kb = (unsigned)(rc_bytes_served / 1024);
  out->unowned = rc_unowned;
  out->fetched_kb = (unsigned)(rc_fetched / 1024);
}
