/* fios.c -- use FIOS2 for optimized I/O
 *
 * Copyright (C) 2021 Andy Nguyen
 *
 * There used to be two 16 MB FIOS RAM caches here, registered over main.obb and
 * patch.obb with sceFiosIOFilterAdd, and 32 MB of the game's heap went into
 * them for the whole of every session.
 *
 * They were never in the path of the game's reads. sceFiosIOFilterAdd puts a
 * filter in front of files opened through the FIOS file API, and nothing in
 * this loader opens a file that way: the game reads through fopen and fread,
 * which resolve to sceLibc's and go to sceIo, not through FIOS's filter chain.
 *
 * Measured rather than argued, two sessions on the same route:
 *
 *   registered      76679 ms reading, 186 MB over 16243 reads   412 ms/MB
 *   not registered  78122 ms reading, 200 MB over 16865 reads   391 ms/MB
 *
 * The run without them was slightly faster per megabyte, and its heap ended
 * 35 MB lower. Thirty-two megabytes held all session, on a port that has spent
 * this long dying of heap exhaustion, buying nothing measurable.
 *
 * sceFiosInitialize stays. The thread affinities and priorities it sets below
 * are worth keeping, and the game's reads still go through SceFiosIO threads.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "config.h"
#include "fios.h"
#include "so_util.h"

#define MAX_PATH_LENGTH 128

static int64_t g_OpStorage[SCE_FIOS_OP_STORAGE_SIZE(64, MAX_PATH_LENGTH) / sizeof(int64_t) + 1];
static int64_t g_ChunkStorage[SCE_FIOS_CHUNK_STORAGE_SIZE(1024) / sizeof(int64_t) + 1];
static int64_t g_FHStorage[SCE_FIOS_FH_STORAGE_SIZE(32, MAX_PATH_LENGTH) / sizeof(int64_t) + 1];
static int64_t g_DHStorage[SCE_FIOS_DH_STORAGE_SIZE(32, MAX_PATH_LENGTH) / sizeof(int64_t) + 1];

int fios_init(void) {
  int res;

  SceFiosParams params = SCE_FIOS_PARAMS_INITIALIZER;
  params.opStorage.pPtr = g_OpStorage;
  params.opStorage.length = sizeof(g_OpStorage);
  params.chunkStorage.pPtr = g_ChunkStorage;
  params.chunkStorage.length = sizeof(g_ChunkStorage);
  params.fhStorage.pPtr = g_FHStorage;
  params.fhStorage.length = sizeof(g_FHStorage);
  params.dhStorage.pPtr = g_DHStorage;
  params.dhStorage.length = sizeof(g_DHStorage);
  params.pathMax = MAX_PATH_LENGTH;

  params.threadAffinity[SCE_FIOS_IO_THREAD] = 0x40000;
  params.threadAffinity[SCE_FIOS_CALLBACK_THREAD] = 0;
  params.threadAffinity[SCE_FIOS_DECOMPRESSOR_THREAD] = 0;

  params.threadPriority[SCE_FIOS_IO_THREAD] = 64;
  params.threadPriority[SCE_FIOS_CALLBACK_THREAD] = 191;
  params.threadPriority[SCE_FIOS_DECOMPRESSOR_THREAD] = 191;

  res = sceFiosInitialize(&params);
  if (res < 0)
    return res;

  return 0;
}

void fios_terminate(void) {
  sceFiosTerminate();
}
