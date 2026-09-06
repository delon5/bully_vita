/* game_memory.h -- the game's own memory accounting, read back out of it
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __GAME_MEMORY_H__
#define __GAME_MEMORY_H__

#include <stddef.h>

// Finds the engine's accounting tables. Call once, after the .so is relocated
// and before the game starts allocating.
void game_memory_init(void);

// Writes a breakdown of what the game thinks it is holding to the trace, one
// line for the categories in use and one for the ones that moved since the last
// call. Cheap enough to call every heartbeat: it reads 128 words.
void game_memory_report(void);

// The game's allocators. Registered in place of newlib's own so that a failure
// is recorded and survived rather than returned as a NULL the game will
// dereference. See the comment on the rescue reserve in game_memory.c.
void *game_malloc(size_t size);
void *game_calloc(size_t count, size_t size);
void *game_realloc(void *ptr, size_t size);
void *game_memalign(size_t alignment, size_t size);

#endif
