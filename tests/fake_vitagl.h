/* fake_vitagl.h -- the parts of the fake driver a test needs to inspect.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __FAKE_VITAGL_H__
#define __FAKE_VITAGL_H__

#include <stdint.h>
#include <vitaGL.h>

#define FAKE_SLOTS 16384
// Guard bytes written past the end of every texture buffer.
#define FAKE_CANARY 32
#define FAKE_CANARY_BYTE 0x5a

// What the driver actually holds for each texture name, as opposed to what the
// cache believes. slot_content is a fingerprint of the pixels, so a test can
// tell a genuinely restored texture from a white placeholder that merely
// happens to be bound.
extern size_t fake_slot_bytes[FAKE_SLOTS];
extern void *fake_slot_data[FAKE_SLOTS];

// The real allocation behind a vglGetTexDataPointer result.
size_t vglMallocUsableSize(void *ptr);

// How vitaGL itself sizes a pixel, which is not always what the loader's budget
// estimate says.
size_t fake_bpp(GLint internalformat, GLenum type);

// 0 if every texture buffer's guard bytes are intact, else an overrun name.
GLuint fake_first_overrun(void);
extern uint32_t fake_slot_content[FAKE_SLOTS];
extern int fake_slot_alive[FAKE_SLOTS];
extern GLuint fake_bound;

// Memory the driver has left. Running it dry is the crash this cache exists to
// prevent, so the driver aborts rather than letting a test pass through it.
// 0 CDRAM, 1 RAM, 2 phycont, matching vglMemFree's enum.
#define FAKE_POOLS 3
extern size_t fake_free_memory;
extern size_t fake_pool_free[FAKE_POOLS];
extern size_t fake_pool_start[FAKE_POOLS];
// Split the driver's memory across pools instead of putting it all in CDRAM.
void fake_set_pools(size_t cdram, size_t ram, size_t phycont);
extern size_t fake_low_water;

// Set to make the next upload behave the way vitaGL does when it rejects one:
// return having allocated nothing.
extern int fake_reject_next_upload;

// The fingerprint a texture uploaded with this tag should read back as.
uint32_t fake_fingerprint_of(uint32_t tag);
// What a draw call would sample from this texture right now.
uint32_t fake_sampled(GLuint id);

void fake_reset(size_t free_memory);

// Holds the writer thread inside sceIoWrite while set.
extern volatile int fake_stall_writes;

// Free space the loader will see on ux0.
extern uint64_t fake_card_free;

#endif
