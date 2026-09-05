/* fake_alloc_env.h -- enough of the loader for the allocation tracer to run
 * on the host.
 *
 * The tracer is wired into the eboot at the linker, through --wrap, and decides
 * whose an allocation is from the return address of its caller. Neither can be
 * reproduced on a build machine, so the accounting is driven directly instead:
 * trace_alloc takes the return address it would have seen.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __FAKE_ALLOC_ENV_H__
#define __FAKE_ALLOC_ENV_H__

#include <stddef.h>
#include <stdint.h>

#include "../loader/alloc_trace.c"

// The tracer and its environment are pulled in as source, in one translation
// unit, so a test can see the counters it needs to assert on -- they are static
// to alloc_trace.c, as they should be.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>


// Counted so the report's own behaviour can be asserted on rather than read.
static int reported_loader_sites;

int traceLog(char *text, ...) {
  if (text && text[0] == 'l') // "loader heap:   eboot ..."
    reported_loader_sites++;
  return 0;
}

int debugPrintf(char *text, ...) { (void)text; return 0; }

void fatal_error(const char *fmt, ...) {
  va_list list;
  va_start(list, fmt);
  vfprintf(stderr, fmt, list);
  va_end(list);
  abort();
}

so_module bully_mod;
int frames_swapped;

void *__real_malloc(size_t size) { return malloc(size); }
void *__real_calloc(size_t count, size_t size) { return calloc(count, size); }
void *__real_realloc(void *ptr, size_t size) { return realloc(ptr, size); }
void *__real_aligned_alloc(size_t alignment, size_t size) {
  void *p = NULL;
  if (posix_memalign(&p, alignment < sizeof(void *) ? sizeof(void *) : alignment, size))
    return NULL;
  return p;
}
void *__real_memalign(size_t alignment, size_t size) {
  void *p = NULL;
  if (posix_memalign(&p, alignment < sizeof(void *) ? sizeof(void *) : alignment, size))
    return NULL;
  return p;
}
void __real_free(void *ptr) { free(ptr); }

void *trace_alloc(size_t size, void *return_address) {
  void *ptr = __real_malloc(size);
  account_alloc(ptr, return_address);
  return ptr;
}

void trace_free(void *ptr) {
  if (ptr)
    account_free(ptr);
  __real_free(ptr);
}

int count_reported_loader_sites(void) {
  reported_loader_sites = 0;
  alloc_trace_loader_report();
  return reported_loader_sites - 1; // the first line is the total, not a site
}

#endif
