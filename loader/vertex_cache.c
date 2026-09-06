/* vertex_cache.c -- release the CPU-side copies of vertex data the game keeps.
 *
 * This is the leak. Read out of the binary:
 *
 *   void *VertexBufferES::Lock() {
 *     if (m_uploaded)                          // [this+0x2c]
 *       return m_data;                         // [this+0x28]
 *     m_data = memalign(8, m_count * VertexDeclaration::Size());
 *     return m_data;
 *   }
 *
 *   void VertexBufferES::Unlock() {
 *     glBindBuffer(GL_ARRAY_BUFFER, m_glBuffer);   // [this+0x24]
 *     glBufferData(GL_ARRAY_BUFFER, size, m_data, GL_STATIC_DRAW);
 *     glBindBuffer(GL_ARRAY_BUFFER, 0);
 *     m_uploaded = 1;
 *     // m_data is not freed
 *   }
 *
 * So every vertex buffer keeps a full copy of its mesh in the heap after the
 * data has been handed to the GPU, and keeps it until the buffer is destroyed
 * -- which for world geometry is never. CleanUp and Allocate both free it, so
 * this is not an oversight in the ordinary sense: it is a deliberate trade,
 * memory for the ability to re-lock without reading back from the GPU, and it
 * is a perfectly good trade on a phone with gigabytes to spare.
 *
 * On hardware it was the largest single holder in the process: 20 MB across
 * sampled small blocks plus 13 MB in large ones, climbing 1 MB -> 33 MB over a
 * session and never falling, and the allocation that finally failed was this
 * one, asking for 42752 bytes.
 *
 * The loader frees those copies once the game has left a buffer alone for a
 * while, and hands back a fresh one if it is ever locked again. Unlock is left
 * strictly alone -- the GPU upload path is not something to reimplement from a
 * disassembly -- so the only behaviour that changes is that a buffer locked
 * after a long silence gets uninitialised memory rather than its previous
 * contents. That is correct for anything the game fills before unlocking, which
 * is the normal way to use this class, and wrong for anything that locks to
 * amend a few vertices in place. Buffers touched every frame are never swept,
 * so the dynamic ones this could hurt are the ones it never reaches.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <psp2/io/stat.h>

#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "main.h"
#include "so_util.h"
#include "vertex_cache.h"

// Field offsets, read out of VertexBufferES::Lock, Unlock, CleanUp and
// Allocate. All four agree on them.
#define VB_COUNT 0x0c
#define VB_DECLARATION 0x10
#define VB_DATA 0x28
#define VB_UPLOADED 0x2c

#define MAX_BUFFERS 4096

typedef struct {
  void *buffer;
  uint32_t last_lock;
  uint32_t bytes;
} TrackedBuffer;

static TrackedBuffer tracked[MAX_BUFFERS];
static uint32_t frame_counter;
static uint32_t held_bytes, released_bytes;
static uint32_t relocked_count;
static int installed;

static uint32_t (*declaration_size)(void *declaration);

static uint32_t buffer_size(void *self) {
  if (!declaration_size)
    return 0;
  uint32_t count = *(uint32_t *)((char *)self + VB_COUNT);
  return count * declaration_size((char *)self + VB_DECLARATION);
}

// One slot per buffer, found by address. The game reuses these objects, so a
// slot is claimed by whichever buffer is living at that address now.
static TrackedBuffer *slot_for(void *buffer) {
  uint32_t i = ((uint32_t)(uintptr_t)buffer >> 4) * 0x9e3779b1u >> 20;
  for (uint32_t probe = 0; probe < 32; probe++) {
    TrackedBuffer *t = &tracked[(i + probe) & (MAX_BUFFERS - 1)];
    if (t->buffer == buffer || !t->buffer)
      return t;
  }
  return NULL; // more live buffers than slots; the rest simply keep their copy
}

// Replaces VertexBufferES::Lock, which is short enough to reproduce exactly.
// The one difference is the case the original cannot meet: uploaded, but with
// no staging copy, because we freed it.
static void *VertexBufferES_Lock(void *self) {
  void **data = (void **)((char *)self + VB_DATA);
  uint8_t *uploaded = (uint8_t *)((char *)self + VB_UPLOADED);

  TrackedBuffer *t = slot_for(self);
  if (t) {
    if (!t->buffer)
      t->buffer = self;
    t->last_lock = frame_counter;
  }

  if (*uploaded && *data)
    return *data;

  if (*uploaded && !*data)
    relocked_count++; // we took this one back and the game wants it again

  uint32_t size = buffer_size(self);
  if (!size)
    return *data;

  *data = memalign(8, size);
  if (*data && t) {
    t->bytes = size;
    held_bytes += size;
  }
  return *data;
}

void vertex_cache_init(void) {
  SceIoStat stat;
  if (sceIoGetstat(VERTEX_CACHE_DISABLE_PATH, &stat) >= 0) {
    traceLog("vertex cache: disabled by %s\n", VERTEX_CACHE_DISABLE_PATH);
    return;
  }

  uintptr_t lock = so_symbol(&bully_mod, "_ZN14VertexBufferES4LockEv");
  declaration_size =
      (uint32_t (*)(void *))so_symbol(&bully_mod, "_ZNK17VertexDeclaration4SizeEv");
  if (!lock || !declaration_size) {
    traceLog("vertex cache: VertexBufferES::Lock not found, staging copies stay\n");
    return;
  }

  hook_addr(lock, (uintptr_t)&VertexBufferES_Lock);
  installed = 1;
  traceLog("vertex cache: holding staging copies for %d frames after a lock\n",
           VERTEX_CACHE_IDLE_FRAMES);
}

void vertex_cache_tick(void) {
  if (!installed)
    return;
  frame_counter++;

  // A slice of the table each frame rather than all of it: this runs in the
  // game's loop and the whole point is to stop costing it time.
  static uint32_t cursor;
  for (int n = 0; n < MAX_BUFFERS / 32; n++) {
    TrackedBuffer *t = &tracked[cursor++ & (MAX_BUFFERS - 1)];
    if (!t->buffer || frame_counter - t->last_lock < VERTEX_CACHE_IDLE_FRAMES)
      continue;

    void **data = (void **)((char *)t->buffer + VB_DATA);
    uint8_t *uploaded = (uint8_t *)((char *)t->buffer + VB_UPLOADED);
    // Only once the data is safely on the GPU. A buffer that is locked but not
    // yet unlocked is being filled right now.
    if (!*uploaded || !*data)
      continue;

    free(*data);
    *data = NULL;
    held_bytes -= t->bytes < held_bytes ? t->bytes : held_bytes;
    released_bytes += t->bytes;
    t->bytes = 0;
  }
}

void vertex_cache_stats(VertexCacheStats *out) {
  int n = 0;
  for (int i = 0; i < MAX_BUFFERS; i++)
    if (tracked[i].buffer)
      n++;
  out->tracked = n;
  out->held_kb = (int)(held_bytes / 1024);
  out->released_kb = (int)(released_bytes / 1024);
  out->relocked = (int)relocked_count;
  out->installed = installed;
}
