/* fios.c -- use FIOS2 for optimized I/O
 *
 * Copyright (C) 2021 Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <psp2/io/stat.h>

#include "main.h"
#include "config.h"
#include "fios.h"
#include "so_util.h"

#define MAX_PATH_LENGTH 128
// The read cache over main.obb. This is 32 MB in one block, taken at startup
// and never released -- a fifth of the newlib heap, and the largest single
// allocation in the process. The game is dying of heap exhaustion with
// "system: 4 MB user free" and an OUT OF MEMORY on a 683 KB texture upload, so
// this is memory it needs more than we need the cache depth.
//
// Halved rather than cut to nothing: the cache is what keeps area streaming off
// the memory card, and the port already stutters when it has to read. The GTA
// SA port uses 64 MB for the same thing, so there is no reason to think 32 was
// a carefully chosen figure to begin with.
#define RAMCACHEBLOCKSIZE (32 * 1024)
#define RAMCACHEBLOCKNUM 512

// ...and a way to prove whether any of it is doing anything.
//
// These caches are registered with sceFiosIOFilterAdd, which puts them in front
// of files opened through the FIOS file API. Nothing in the loader opens a file
// that way. The game reads through fopen and fread, which the loader resolves
// to sceLibc's, and sceLibc goes to sceIo -- not through FIOS's filter chain at
// all. On that reading these two buffers are 32 MB of the game's heap held for
// a whole session and used by nothing.
//
// The measurement agrees: 375 MB arrived in 49894 reads averaging 2.8 ms each,
// which is 2.7 MB/s off a card that manages fifteen or more. Nothing served out
// of a RAM cache takes 2.8 ms.
//
// But "on that reading" has been wrong three times in this port already, and
// this is 32 MB. So rather than delete it on an argument, make it testable:
// with this file present the caches are allocated one block each instead of
// five hundred and twelve. If the read timings in the trace do not move, they
// were never in the path and the memory can go to the game.
// Cutting the buffer to a single block did not disable the caches, it made
// sceFiosIOFilterAdd reject them: FIOS needs room for its own bookkeeping as
// well as the blocks, so a 32 KB work buffer fails, fios_init returns the
// error, and the loader dies on "Error could not initialize fios." before the
// game ever starts. Skipping registration altogether is both the correct "off"
// and the state actually worth measuring, since it hands the whole 32 MB back.
static int ram_cache_wanted(void) {
  SceIoStat stat;
  return sceIoGetstat(FIOS_CACHE_DISABLE_PATH, &stat) < 0;
}

static int64_t g_OpStorage[SCE_FIOS_OP_STORAGE_SIZE(64, MAX_PATH_LENGTH) / sizeof(int64_t) + 1];
static int64_t g_ChunkStorage[SCE_FIOS_CHUNK_STORAGE_SIZE(1024) / sizeof(int64_t) + 1];
static int64_t g_FHStorage[SCE_FIOS_FH_STORAGE_SIZE(32, MAX_PATH_LENGTH) / sizeof(int64_t) + 1];
static int64_t g_DHStorage[SCE_FIOS_DH_STORAGE_SIZE(32, MAX_PATH_LENGTH) / sizeof(int64_t) + 1];

static SceFiosRamCacheContext g_MainRamCacheContext = SCE_FIOS_RAM_CACHE_CONTEXT_INITIALIZER;
static char *g_MainRamCacheWorkBuffer;

static SceFiosRamCacheContext g_PatchRamCacheContext = SCE_FIOS_RAM_CACHE_CONTEXT_INITIALIZER;
static char *g_PatchRamCacheWorkBuffer;

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

  if (!ram_cache_wanted()) {
    traceLog("fios: RAM caches not registered, %s is present; %d MB left to the game\n",
             FIOS_CACHE_DISABLE_PATH, 2 * RAMCACHEBLOCKNUM * RAMCACHEBLOCKSIZE / (1024 * 1024));
    return 0;
  }

  const size_t cache_bytes = (size_t)RAMCACHEBLOCKNUM * RAMCACHEBLOCKSIZE;

  g_MainRamCacheWorkBuffer = memalign(8, cache_bytes);
  if (!g_MainRamCacheWorkBuffer)
    return -1;

  g_MainRamCacheContext.pPath = DATA_PATH "/Android/main.obb";
  g_MainRamCacheContext.pWorkBuffer = g_MainRamCacheWorkBuffer;
  g_MainRamCacheContext.workBufferSize = cache_bytes;
  g_MainRamCacheContext.blockSize = RAMCACHEBLOCKSIZE;
  res = sceFiosIOFilterAdd(0, sceFiosIOFilterCache, &g_MainRamCacheContext);
  if (res < 0) {
    traceLog("fios: the main archive's cache was refused (0x%08x)\n", (unsigned)res);
    return res;
  }

  g_PatchRamCacheWorkBuffer = memalign(8, cache_bytes);
  if (!g_PatchRamCacheWorkBuffer)
    return -1;

  g_PatchRamCacheContext.pPath = DATA_PATH "/Android/patch.obb";
  g_PatchRamCacheContext.pWorkBuffer = g_PatchRamCacheWorkBuffer;
  g_PatchRamCacheContext.workBufferSize = cache_bytes;
  g_PatchRamCacheContext.blockSize = RAMCACHEBLOCKSIZE;
  res = sceFiosIOFilterAdd(1, sceFiosIOFilterCache, &g_PatchRamCacheContext);
  if (res < 0) {
    traceLog("fios: the patch archive's cache was refused (0x%08x)\n", (unsigned)res);
    return res;
  }

  traceLog("fios: two RAM caches of %d MB registered over the archives\n",
           (int)(cache_bytes / (1024 * 1024)));
  return 0;
}

void fios_terminate(void) {
  sceFiosTerminate();
  free(g_PatchRamCacheWorkBuffer);
  free(g_MainRamCacheWorkBuffer);
}
