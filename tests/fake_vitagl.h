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

// What the driver actually holds for each texture name, as opposed to what the
// cache believes. slot_content is a fingerprint of the pixels, so a test can
// tell a genuinely restored texture from a white placeholder that merely
// happens to be bound.
extern size_t fake_slot_bytes[FAKE_SLOTS];
extern void *fake_slot_data[FAKE_SLOTS];
extern uint32_t fake_slot_content[FAKE_SLOTS];
extern int fake_slot_alive[FAKE_SLOTS];
extern GLuint fake_bound;

// Memory the driver has left. Running it dry is the crash this cache exists to
// prevent, so the driver aborts rather than letting a test pass through it.
extern size_t fake_free_memory;
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
