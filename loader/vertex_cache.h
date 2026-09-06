/* vertex_cache.h -- release the CPU-side copies of vertex data the game keeps.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __VERTEX_CACHE_H__
#define __VERTEX_CACHE_H__

// Hooks VertexBufferES::Lock. Call after the .so is relocated and resolved.
void vertex_cache_init(void);

// Frees the staging copies of buffers the game has not locked recently. Call
// once a frame.
void vertex_cache_tick(void);

typedef struct {
  int tracked;       // buffers seen
  int held_kb;       // staging copies still held
  int released_kb;   // freed so far
  int relocked;      // buffers locked again after being released
  int installed;
} VertexCacheStats;

void vertex_cache_stats(VertexCacheStats *out);

#endif
