/* alloc_trace.h -- accounting for the heap the game allocates from.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __ALLOC_TRACE_H__
#define __ALLOC_TRACE_H__

#include <stddef.h>

#include "config.h"

// The game's allocators. Every allocation the .so makes comes through these,
// because the loader is what resolves malloc and friends for it.
void *bully_malloc(size_t size);
void *bully_calloc(size_t count, size_t size);
void *bully_realloc(void *ptr, size_t size);
void *bully_memalign(size_t alignment, size_t size);
void bully_free(void *ptr);

// operator new / operator new[]. The game defines its own on top of malloc, so
// these are hooked over it -- otherwise every C++ allocation is credited to
// operator new rather than to whoever wrote "new".
void *bully_operator_new(size_t size);
void *bully_operator_new_array(size_t size);

// Writes what the heap is holding and who asked for it to the trace.
void alloc_trace_report(void);

// The same for the memory the eboot allocates -- vitaGL, OpenAL, the loader
// itself -- which the game-side wrappers never saw.
#ifdef LOADER_ALLOC_TRACE
void alloc_trace_loader_report(void);
#else
#define alloc_trace_loader_report() ((void)0)
#endif

#endif
