/* streaming_patch.h -- make the game's own streamer free what it loads.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __STREAMING_PATCH_H__
#define __STREAMING_PATCH_H__

// Hooks CStreaming's memory gate. Call after the .so is relocated and resolved.
void streaming_patch_init(void);

// For the trace: how much the streamer thinks it is holding, and how many times
// we have told it to make room.
typedef struct {
  int memory_used_mb; // heap the game is holding
  int refusals;       // times the gate said "no", each of which frees something
  int backoffs;       // times refusing stopped helping and we let it load again
  int installed;
} StreamingStats;

void streaming_patch_stats(StreamingStats *out);

#endif
