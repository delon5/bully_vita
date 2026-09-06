/* alloc_trace.h -- accounting for the heap the game allocates from.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __ALLOC_TRACE_H__
#define __ALLOC_TRACE_H__

#include <stddef.h>

#include "config.h"

// Accounting for an allocation whose caller the tracer cannot work out for
// itself.
//
// Everything the eboot allocates arrives through --wrap, where the return
// address is the code that asked. The game's allocations no longer do: they go
// through game_memory.c first, so that a failure can be reported and survived,
// and by the time they reach the allocator the return address is inside the
// loader. Passing the caller through explicitly keeps the game's blocks
// credited to the game -- without it every allocation in the .so was filed
// against the eboot and the report named nothing at all.
#ifdef LOADER_ALLOC_TRACE
void alloc_trace_alloc(void *ptr, size_t size, void *caller);
void alloc_trace_free(void *ptr);
#else
#define alloc_trace_alloc(ptr, size, caller) ((void)0)
#define alloc_trace_free(ptr) ((void)0)
#endif

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
