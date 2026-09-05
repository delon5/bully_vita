/* alloc_trace.h -- accounting for the heap the game allocates from.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __ALLOC_TRACE_H__
#define __ALLOC_TRACE_H__

#include <stddef.h>

// The game's allocators. Every allocation the .so makes comes through these,
// because the loader is what resolves malloc and friends for it.
void *bully_malloc(size_t size);
void *bully_calloc(size_t count, size_t size);
void *bully_realloc(void *ptr, size_t size);
void *bully_memalign(size_t alignment, size_t size);
void bully_free(void *ptr);

// Writes what the heap is holding and who asked for it to the trace.
void alloc_trace_report(void);

#endif
