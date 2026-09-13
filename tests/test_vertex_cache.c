/* test_vertex_cache.c -- the sweep must never touch a buffer that is gone.
 *
 * This cache frees memory belonging to the game and writes NULL back into the
 * game's own object to say so. That is only safe while the object is alive. It
 * keeps raw pointers to VertexBufferES instances in a table, and if one is
 * destroyed and the heap hands its address to something else, a sweep that
 * still trusts the slot reads a pointer out of a stranger's memory and calls
 * free on it. So the tests are weighted towards that: what happens to a slot
 * when the buffer behind it dies, and what the sweep does with a slot it
 * should no longer believe.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_vertex_env.h"
#include "../loader/vertex_cache.c"

// A stand-in for the engine's object. Only the fields the loader reads matter,
// at the offsets it reads them from.
typedef struct {
  char raw[64];
} FakeBuffer;

static uint32_t *vb_count(FakeBuffer *b) { return (uint32_t *)(b->raw + VB_COUNT); }
static void **vb_data(FakeBuffer *b) { return (void **)(b->raw + VB_DATA); }
static uint8_t *vb_uploaded(FakeBuffer *b) { return (uint8_t *)(b->raw + VB_UPLOADED); }

static uint32_t fake_declaration_size(void *decl) { (void)decl; return 16; }

static FakeBuffer *new_buffer(unsigned count) {
  FakeBuffer *b = calloc(1, sizeof(FakeBuffer));
  assert(b);
  *vb_count(b) = count;
  return b;
}

// What the engine's Unlock does, as far as this cache can see: the data is on
// the GPU now, and the staging copy is fair game.
static void mark_uploaded(FakeBuffer *b) { *vb_uploaded(b) = 1; }

static void reset_cache(void) {
  memset(tracked, 0, sizeof(tracked));
  frame_counter = 0;
  held_bytes = churn_bytes = released_bytes = relocked_count = 0;
  declaration_size = fake_declaration_size;
  installed = 1;
}

static void idle(unsigned frames) {
  // The sweep walks a thirty-second of the table each frame, so a full pass
  // takes 32 frames; run enough of them to be sure every slot was visited.
  for (unsigned i = 0; i < frames; i++)
    vertex_cache_tick();
}

// The ordinary case: lock, upload, go quiet, get the memory back.
static void test_a_quiet_buffer_is_reclaimed(void) {
  reset_cache();
  FakeBuffer *b = new_buffer(100);
  void *data = VertexBufferES_Lock(b);
  assert(data && *vb_data(b) == data);
  mark_uploaded(b);
  assert(held_bytes == 1600);

  idle(VERTEX_CACHE_IDLE_FRAMES + 64);
  assert(*vb_data(b) == NULL && "the staging copy was freed and the field nulled");
  assert(held_bytes == 0);
  assert(released_bytes == 1600);
  free(b);
  printf("reclaim      : a buffer left alone for %d frames gives its copy back  OK\n",
         VERTEX_CACHE_IDLE_FRAMES);
}

// A buffer still being written to must not have the ground taken from under it.
static void test_a_buffer_mid_lock_is_left_alone(void) {
  reset_cache();
  FakeBuffer *b = new_buffer(100);
  void *data = VertexBufferES_Lock(b); // locked, never unlocked
  idle(VERTEX_CACHE_IDLE_FRAMES + 64);
  assert(*vb_data(b) == data && "not uploaded yet, so not the sweep's to free");
  free(*vb_data(b));
  free(b);
  printf("mid-lock     : a buffer that never unlocked keeps its copy  OK\n");
}

// The one that matters. The buffer is destroyed and its memory reused by
// something else; the sweep must not go anywhere near the old address.
static void test_a_destroyed_buffer_is_never_swept(void) {
  reset_cache();
  FakeBuffer *b = new_buffer(100);
  VertexBufferES_Lock(b);
  mark_uploaded(b);
  assert(held_bytes == 1600);

  // The engine tears it down. CleanUp runs first, as Delete does through the
  // vtable, and it frees the staging copy itself.
  VertexBufferES_CleanUp(b);
  assert(*vb_data(b) == NULL);
  assert(held_bytes == 0 && "the slot stopped claiming the memory");

  // Now something else is living at that address, with bytes that happen to
  // look like an uploaded buffer holding a pointer.
  void *impostor = malloc(64);
  memset(b->raw, 0, sizeof(b->raw));
  *vb_data(b) = impostor;
  *vb_uploaded(b) = 1;

  idle(VERTEX_CACHE_IDLE_FRAMES + 64);
  assert(*vb_data(b) == impostor && "the sweep freed memory it did not own");
  free(impostor);
  free(b);
  printf("destroyed    : a slot whose buffer was cleaned up is never swept  OK\n");
}

// Even with no CleanUp at all -- some path we have not found -- the sweep must
// refuse to free a pointer it did not hand out.
static void test_a_pointer_we_did_not_hand_out_is_never_freed(void) {
  reset_cache();
  FakeBuffer *b = new_buffer(100);
  VertexBufferES_Lock(b);
  mark_uploaded(b);

  // The game replaced the staging copy behind our back, as Allocate does.
  void *theirs = malloc(64);
  *vb_data(b) = theirs;

  idle(VERTEX_CACHE_IDLE_FRAMES + 64);
  assert(*vb_data(b) == theirs && "only the exact pointer this slot gave out");
  free(theirs);
  free(b);
  printf("not ours     : the sweep frees only the pointer it handed over  OK\n");
}

// A dead slot must not cut the probe chain, or every buffer stored past the gap
// is lost and gets tracked a second time.
//
// This one needs three addresses that genuinely collide, so it searches for
// them rather than placing entries by hand: slot_for starts from each buffer's
// own hash, and a chain the test built at some other index proves nothing.
// Nothing here is dereferenced, so synthetic addresses are safe.
static uint32_t slot_hash(void *p) {
  return ((uint32_t)(uintptr_t)p >> 4) * 0x9e3779b1u >> 20;
}

static void test_a_dead_slot_does_not_hide_the_ones_behind_it(void) {
  reset_cache();
  void *collide[3];
  int found = 0;
  uint32_t want = slot_hash((void *)(uintptr_t)0x10000000);
  for (uintptr_t p = 0x10000000; p < 0x30000000 && found < 3; p += 16)
    if (slot_hash((void *)p) == want)
      collide[found++] = (void *)p;
  assert(found == 3 && "three addresses hashing to one slot");

  // They land in consecutive slots, in the order they were inserted.
  for (int i = 0; i < 3; i++) {
    TrackedBuffer *t = slot_for(collide[i]);
    assert(t);
    t->buffer = collide[i];
  }
  assert(slot_for(collide[2]) == &tracked[(want + 2) & (MAX_BUFFERS - 1)]);

  // Empty the middle one the way CleanUp does.
  TrackedBuffer *middle = slot_for(collide[1]);
  middle->buffer = VB_TOMBSTONE;
  assert(!slot_live(middle));

  // The one stored past the gap must still be found from its own hash.
  assert(slot_for(collide[2]) == &tracked[(want + 2) & (MAX_BUFFERS - 1)] &&
         "a dead slot ended the search and lost the buffer behind it");
  assert(slot_for(collide[0]) == &tracked[want & (MAX_BUFFERS - 1)]);
  printf("chain        : a dead slot is walked past, not treated as the end  OK\n");
}

// A buffer that is not in the table yet must be given the dead slot rather than
// a fresh one, or the table only ever grows.
static void test_a_dead_slot_is_offered_to_a_newcomer(void) {
  reset_cache();
  void *collide[3];
  int found = 0;
  uint32_t want = slot_hash((void *)(uintptr_t)0x10000000);
  for (uintptr_t p = 0x10000000; p < 0x30000000 && found < 3; p += 16)
    if (slot_hash((void *)p) == want)
      collide[found++] = (void *)p;
  assert(found == 3);

  slot_for(collide[0])->buffer = collide[0];
  slot_for(collide[1])->buffer = collide[1];
  slot_for(collide[0])->buffer = VB_TOMBSTONE; // the first one dies

  TrackedBuffer *given = slot_for(collide[2]);
  assert(given == &tracked[want & (MAX_BUFFERS - 1)] &&
         "the newcomer was handed the dead slot, not a third one");
  printf("recycle      : a newcomer is given a dead slot before a fresh one  OK\n");
}

// And a dead slot has to be reusable, or a session with churn fills the table.
static void test_a_dead_slot_is_claimed_again(void) {
  reset_cache();
  FakeBuffer *b = new_buffer(100);
  VertexBufferES_Lock(b);
  mark_uploaded(b);
  TrackedBuffer *slot = slot_for(b);
  VertexBufferES_CleanUp(b);
  assert(slot->buffer == VB_TOMBSTONE);

  // A new buffer at the same address -- exactly what an allocator does.
  memset(b->raw, 0, sizeof(b->raw));
  *vb_count(b) = 50;
  void *data = VertexBufferES_Lock(b);
  assert(slot_for(b) == slot && "the same slot, claimed again");
  assert(slot->buffer == b && slot->data == data);
  assert(held_bytes == 800 && "sized for the new buffer, not the old one");
  free(data);
  free(b);
  printf("reuse        : a dead slot is claimed by the next buffer at that address  OK\n");
}

// A buffer that keeps being locked is never a candidate.
static void test_a_busy_buffer_keeps_its_copy(void) {
  reset_cache();
  FakeBuffer *b = new_buffer(100);
  void *data = VertexBufferES_Lock(b);
  mark_uploaded(b);
  for (unsigned i = 0; i < VERTEX_CACHE_IDLE_FRAMES * 3; i++) {
    vertex_cache_tick();
    if (i % 100 == 0)
      assert(VertexBufferES_Lock(b) == data);
  }
  assert(*vb_data(b) == data && "still in use, still holding its copy");
  free(data);
  free(b);
  printf("busy         : a buffer locked every hundred frames is never swept  OK\n");
}

int main(void) {
  test_a_quiet_buffer_is_reclaimed();
  test_a_buffer_mid_lock_is_left_alone();
  test_a_destroyed_buffer_is_never_swept();
  test_a_pointer_we_did_not_hand_out_is_never_freed();
  test_a_dead_slot_does_not_hide_the_ones_behind_it();
  test_a_dead_slot_is_offered_to_a_newcomer();
  test_a_dead_slot_is_claimed_again();
  test_a_busy_buffer_keeps_its_copy();
  printf("PASS\n");
  return 0;
}
