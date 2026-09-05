/* main.c -- Bully .so loader
 *
 * Copyright (C) 2021 Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <psp2/rtc.h>
#include <psp2/touch.h>
#include <psp2/display.h>
#include <psp2/gxm.h>
#include <kubridge.h>

#include <vitashark.h>
#include <vitashark.h>
#include <vitaGL.h>

#include <malloc.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <wchar.h>
#include <wctype.h>

#include <math.h>
#include <math_neon.h>

#include <errno.h>
#include <ctype.h>
#include <setjmp.h>
#include <sys/time.h>
#include <sys/stat.h>

#include "main.h"
#include "config.h"
#include "dialog.h"
#include "fios.h"
#include "so_util.h"
#include "jni_patch.h"
#include "movie_patch.h"
#include "openal_patch.h"
#include "alloc_trace.h"
#include "streaming_patch.h"
#include "texture_cache.h"

#include "sha1.h"

#include "libc_bridge.h"

int sceLibcHeapSize = MEMORY_SCELIBC_MB * 1024 * 1024;
int _newlib_heap_size_user = MEMORY_NEWLIB_MB * 1024 * 1024;

unsigned int _oal_thread_priority;
unsigned int _oal_thread_affinity;

int capunlocker_enabled = 0;

SceTouchPanelInfo panelInfoFront;

so_module bully_mod;

void *__wrap_memcpy(void *dest, const void *src, size_t n) {
  return sceClibMemcpy(dest, src, n);
}

void *__wrap_memmove(void *dest, const void *src, size_t n) {
  return sceClibMemmove(dest, src, n);
}

void *__wrap_memset(void *s, int c, size_t n) {
  return sceClibMemset(s, c, n);
}

int debugPrintf(char *text, ...) {
#ifdef DEBUG
  va_list list;
  char string[512];

  va_start(list, text);
  vsprintf(string, text, list);
  va_end(list);

  SceUID fd = sceIoOpen("ux0:data/bully_log.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
  if (fd >= 0) {
    sceIoWrite(fd, string, strlen(string));
    sceIoClose(fd);
  }
#endif
  return 0;
}

// vitaGL calls this for every error it reports when built with LOG_ERRORS.
// Its own vgl_log is sceClibPrintf, which goes to a debug console that is not
// attached on a retail Vita, so point it at the trace file instead.
void vgl_file_log(const char *fmt, ...) {
#ifdef LOADER_TRACE
  va_list list;
  char string[512];

  va_start(list, fmt);
  vsnprintf(string, sizeof(string), fmt, list);
  va_end(list);

  traceLog("vgl: %s", string);
#endif
}

int traceLog(char *text, ...) {
#ifdef LOADER_TRACE
  va_list list;
  char string[512];

  va_start(list, text);
  vsnprintf(string, sizeof(string), text, list);
  va_end(list);

  SceUID fd = sceIoOpen("ux0:data/bully_trace.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
  if (fd >= 0) {
    sceIoWrite(fd, string, strlen(string));
    sceIoClose(fd);
  }
#endif
  return 0;
}

int __android_log_assert(const char *cond, const char *tag, const char *fmt, ...) {
  return 0;
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
#ifdef DEBUG
  va_list list;
  char string[512];

  va_start(list, fmt);
  vsprintf(string, fmt, list);
  va_end(list);

  debugPrintf("[LOG] %s: %s\n", tag, string);
#endif
  return 0;
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
  return 0;
}

int __android_log_write(int prio, const char *tag, const char *text) {
  return 0;
}

int ret0(void) {
  return 0;
}

void glLinkProgramHook(GLuint prog) {
  glLinkProgram(prog);
#ifdef LOADER_TRACE
  static int links;
  if (links < 6) {
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    traceLog("program: %u link status %d, gl error 0x%x\n", prog, (int)linked, glGetError());
  }
  links++;
#endif
}

void glDrawElementsHook(GLenum mode, GLsizei count, GLenum type, const void *indices) {
  glDrawElements(mode, count, type, indices);
#ifdef LOADER_TRACE
  extern int trace_draws;
  if (trace_draws == 0 || trace_draws == 100 || trace_draws == 1000)
    traceLog("draw: glDrawElements %d, %d indices, gl error 0x%x\n", trace_draws, count, glGetError());
  trace_draws++;
#endif
}

int ret1(void) {
  return 1;
}

int OS_ScreenGetHeight(void) {
  return SCREEN_H;
}

int OS_ScreenGetWidth(void) {
  return SCREEN_W;
}

#ifdef LOADER_TRACE
// Counted rather than logged individually: the question after startup is
// whether the game is still doing work at all, and five numbers on each
// heartbeat answer that without thousands of lines.
int trace_files, trace_textures, trace_buffers, trace_clears, trace_draws;
#endif

int ProcessEvents(void) {
#ifdef LOADER_TRACE
  // The game's main loop calls this every iteration, so it tells apart a loop
  // that is running but never drawing from one that is not running at all.
  static int events;
  if (events % 600 == 0) {
    TextureCacheStats cache;
    texture_cache_stats(&cache);
    // Heap used as well as the GPU pools. The cache parks evicted textures in
    // the newlib heap, so a crash could now be the heap running out rather than
    // the pools, and the two look nothing alike from a coredump.
    //
    // Broken out rather than taken as one number, because the allocation trace
    // accounts for 76 MB of what this reports as 155 MB, and the shape of that
    // gap decides what the fix even is. arena is what has been taken from the
    // system and never given back; uordblks is what is actually live; fordblks
    // is what has been freed and is sitting in the free lists. A large fordblks
    // is fragmentation, not a leak, and no amount of freeing things fixes it.
    struct mallinfo heap = mallinfo();
    traceLog("loop: %d | tex %d draw %d | vgl ram %d cdram %d phycont %d MB | heap %d MB | "
             "cache %d MB parked %d ev %d re %d lost %d spill %d starve %d defer %d block %d\n",
             events, trace_textures, trace_draws,
             (int)(vglMemFree(VGL_MEM_RAM) / (1024 * 1024)),
             (int)(vglMemFree(VGL_MEM_VRAM) / (1024 * 1024)),
             (int)(vglMemFree(VGL_MEM_PHYCONT) / (1024 * 1024)),
             (int)(heap.uordblks / (1024 * 1024)),
             cache.tracked_mb, cache.parked_mb, cache.evicted, cache.restored, cache.failed,
             cache.spilled, cache.starved, cache.deferred, cache.blocked);
    // Frames actually presented since the last heartbeat, over the wall clock
    // between them. vsync is disabled, so this is what the hardware managed.
    static int last_frames;
    static uint32_t last_us;
    SceKernelSysClock now;
    sceKernelGetProcessTime(&now);
    uint32_t us = (uint32_t)now;
    int drawn = frames_swapped - last_frames;
    int fps = 0;
    if (last_us && us > last_us)
      fps = (int)(((uint64_t)drawn * 1000000u) / (us - last_us));
    last_frames = frames_swapped;
    last_us = us;
    traceLog("fps: %d over the last %d frames\n", fps, drawn);
    traceLog("heapinfo: arena %d MB, live %d MB, free-listed %d MB, top %d KB\n",
             (int)(heap.arena / (1024 * 1024)), (int)(heap.uordblks / (1024 * 1024)),
             (int)(heap.fordblks / (1024 * 1024)), (int)(heap.keepcost / 1024));

    StreamingStats stream;
    streaming_patch_stats(&stream);
    traceLog("stream: gate %s, streamer holds %d MB of %d, %d refusals, %d backoffs\n",
             stream.installed ? "on" : "OFF", stream.memory_used_mb, stream.budget_mb,
             stream.refusals, stream.backoffs);

#ifdef LOADER_ALLOC_TRACE
    // Less often than the heartbeat: this one walks a table and prints several
    // lines, and the question it answers changes over minutes, not frames.
    if (events % 6000 == 0)
      alloc_trace_report();
#endif
  }
  events++;
#endif
  movie_draw_frame();
  return 0; // 1 is exit!
}

// The game is an Android binary and passes Bionic's clock ids, which do not
// match newlib's, so match on the raw numbers this port has always used rather
// than on whatever CLOCK_MONOTONIC happens to mean to the host headers.
#define ANDROID_CLOCK_ID_0 0
#define ANDROID_CLOCK_ID_1 1

int clock_gettime(clockid_t clk_id, struct timespec *tp) {
  if (clk_id == ANDROID_CLOCK_ID_0) {
    SceKernelSysClock ticks;
    sceKernelGetProcessTime(&ticks);

    tp->tv_sec = ticks / (1000 * 1000);
    tp->tv_nsec = (ticks * 1000) % (1000 * 1000 * 1000);

    return 0;
  } else if (clk_id == ANDROID_CLOCK_ID_1) {
    time_t seconds;
    SceDateTime time;
    sceRtcGetCurrentClockLocalTime(&time);

    sceRtcGetTime_t(&time, &seconds);

    tp->tv_sec = seconds;
    tp->tv_nsec = time.microsecond * 1000;

    return 0;
  }

  return -ENOSYS;
}

// only used for NVEventAppMain
int pthread_create_fake(int r0, int r1, int r2, void *arg) {
  int (* func)() = *(void **)(arg + 4);
  return func();
}

int pthread_mutex_init_fake(SceKernelLwMutexWork **work) {
  *work = (SceKernelLwMutexWork *)memalign(8, sizeof(SceKernelLwMutexWork));
  if (sceKernelCreateLwMutex(*work, "mutex", 0x2000 | SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL) < 0)
    return -1;
  return 0;
}

int pthread_mutex_destroy_fake(SceKernelLwMutexWork **work) {
  if (sceKernelDeleteLwMutex(*work) < 0)
    return -1;
  free(*work);
  return 0;
}

int pthread_mutex_lock_fake(SceKernelLwMutexWork **work) {
  if (!*work)
    pthread_mutex_init_fake(work);
  if (sceKernelLockLwMutex(*work, 1, NULL) < 0)
    return -1;
  return 0;
}

int pthread_mutex_unlock_fake(SceKernelLwMutexWork **work) {
  if (sceKernelUnlockLwMutex(*work, 1) < 0)
    return -1;
  return 0;
}

int thread_stub(SceSize args, uintptr_t *argp) {
  int (* func)(void *arg) = (void *)argp[0];
  void *arg = (void *)argp[1];
  char *out = (char *)argp[2];
  const char *name = (const char *)argp[3];
  out[0x41] = 1; // running
  traceLog("thread: %s entered\n", name ? name : "?");
  func(arg);
  // A thread that faults never gets here, so the absence of this line for a
  // given thread is what tells a trace apart from a clean shutdown.
  traceLog("thread: %s returned\n", name ? name : "?");
  return sceKernelExitDeleteThread(0);
}

// GameMain with cpu 0 and priority 3
// Sound with cpu 0 and priority 3
// RenderThread with cpu 2 and priority 3
// CDStreamThread with cpu 0 and priority 3
void *OS_ThreadLaunch(int (* func)(), void *arg, int cpu, char *name, int unused, int priority) {
  int vita_priority;
  int vita_affinity;

  if (capunlocker_enabled) {
    if (strcmp(name, "GameMain") == 0) {
      vita_priority = 64;
      vita_affinity = 0x10000;
    } else if (strcmp(name, "RenderThread") == 0) {
      vita_priority = 64;
      vita_affinity = 0x20000;
    } else if (strcmp(name, "CDStreamThread") == 0) {
      vita_priority = 65;
      vita_affinity = 0x40000;
    } else if (strcmp(name, "Sound") == 0) {
      vita_priority = 65;
      vita_affinity = 0x80000;
    } else {
      fatal_error("Error unknown thread %s\n", name);
      return NULL;
    }
  } else {
    if (strcmp(name, "GameMain") == 0) {
      vita_priority = 65;
      vita_affinity = 0x10000;
    } else if (strcmp(name, "RenderThread") == 0) {
      vita_priority = 64;
      vita_affinity = 0x20000;
    } else if (strcmp(name, "CDStreamThread") == 0) {
      vita_priority = 65;
      vita_affinity = 0x40000;
    } else if (strcmp(name, "Sound") == 0) {
      vita_priority = 65;
      vita_affinity = 0x20000;
    } else {
      fatal_error("Error unknown thread %s\n", name);
      return NULL;
    }
  }

  traceLog("thread: %s starting (priority %d, affinity 0x%x)\n", name, vita_priority, vita_affinity);
  SceUID thid = sceKernelCreateThread(name, (SceKernelThreadEntry)thread_stub, vita_priority, 128 * 1024, 0, vita_affinity, NULL);
  if (thid >= 0) {
    char *out = malloc(0x48);
    *(int *)(out + 0x24) = thid;

    uintptr_t args[4];
    args[0] = (uintptr_t)func;
    args[1] = (uintptr_t)arg;
    args[2] = (uintptr_t)out;
    args[3] = (uintptr_t)name;
    sceKernelStartThread(thid, sizeof(args), args);

    return out;
  }

  return NULL;
}

void OS_ThreadWait(void *thread) {
  if (thread)
    sceKernelWaitThreadEnd(*(int *)(thread + 0x24), NULL, NULL);
}

void *TouchSense__TouchSense(void *this) {
  return this;
}

typedef struct {
  uint32_t dataOffset;
  uint32_t dataSize;
  uint16_t nameLength;
  char name[0];
} __attribute__((__packed__)) IDXEntry;

typedef struct {
  uint32_t dataOffset;
  uint32_t dataSize;
  char *name;
} ZIPEntry;

typedef struct {
  void *vtable;
  uint32_t unk4;
  uint32_t numEntries;
  ZIPEntry *entries;
  char file[256];
  uint32_t unk110;
  uint32_t unk114;
} ZIPFile;

int (* ZIPFile__EntryCompare)(ZIPEntry *a, ZIPEntry *b);

void ZIPFile__SortEntries(ZIPFile *this) {
  if (this->numEntries > 1) {
    int unsorted = 0;
    for (int i = 0; i < this->numEntries - 1; i++) {
      if (ZIPFile__EntryCompare(&this->entries[i], &this->entries[i + 1]) > 0) {
        unsorted = 1;
        break;
      }
    }

    if (unsorted)
      qsort(this->entries, this->numEntries, sizeof(ZIPEntry), (__compar_fn_t)ZIPFile__EntryCompare);
  }

  char idx_path[512];
  snprintf(idx_path, sizeof(idx_path), "%s%s.idx", DATA_PATH, this->file);

  FILE *file = sceLibcBridge_fopen(idx_path, "w");
  if (file) {
    sceLibcBridge_fwrite(&this->numEntries, 1, sizeof(uint32_t), file);
    for (int i = 0; i < this->numEntries; i++) {
      IDXEntry entry;
      entry.dataOffset = this->entries[i].dataOffset;
      entry.dataSize = this->entries[i].dataSize;
      entry.nameLength = strlen(this->entries[i].name);
      sceLibcBridge_fwrite(&entry, 1, sizeof(IDXEntry), file);
      sceLibcBridge_fwrite(this->entries[i].name, 1, entry.nameLength, file);
    }
    sceLibcBridge_fclose(file);
  }
}

static void (* FadeLoadScene__Loading)(int result);
static int (* cMemCard__HasSave)(int save);
static int (* BullyApplication__OrigLoadSlot)(void **fadeload,int saveSlot);
static uint64_t (* OS_FileGetDate)(int area, const char *path);

int BullyApplication__OrigContinue(void *this, int a2, int a3, int a4) {
  FadeLoadScene__Loading(*((uintptr_t *)this + 29));

  uint64_t latestDate = 0;
  int latestSave = 0;
  if (!*((char *)this + 140)) {
    for (int i = 0; i < 5; i++) {
      char filename[11];
      if (cMemCard__HasSave(i)) {
        sprintf(filename, "BullyFile%d", i);
        uint64_t date = OS_FileGetDate(1, filename);
        if (latestDate < date) {
          latestDate = date;
          latestSave = i;
        }
      }
    }
  }

  return BullyApplication__OrigLoadSlot(this,latestSave);
}

int Application__Exit(void *this) {
  texture_cache_shutdown();
  return sceKernelExitProcess(0);
}

extern void *__cxa_guard_acquire;
extern void *__cxa_guard_release;

void patch_game(void) {
#ifdef LOADER_ALLOC_TRACE
  hook_addr(so_symbol(&bully_mod, "_Znwj"), (uintptr_t)&bully_operator_new);
  hook_addr(so_symbol(&bully_mod, "_Znaj"), (uintptr_t)&bully_operator_new_array);
#endif
  streaming_patch_init();
  hook_addr(so_symbol(&bully_mod, "__cxa_guard_acquire"), (uintptr_t)&__cxa_guard_acquire);
  hook_addr(so_symbol(&bully_mod, "__cxa_guard_release"), (uintptr_t)&__cxa_guard_release);

  hook_addr(so_symbol(&bully_mod, "_Z24NVThreadGetCurrentJNIEnvv"), (uintptr_t)NVThreadGetCurrentJNIEnv);

  // do not use pthread
  hook_addr(so_symbol(&bully_mod, "_Z15OS_ThreadLaunchPFjPvES_jPKci16OSThreadPriority"), (uintptr_t)OS_ThreadLaunch);
  hook_addr(so_symbol(&bully_mod, "_Z13OS_ThreadWaitPv"), (uintptr_t)OS_ThreadWait);

  hook_addr(so_symbol(&bully_mod, "_Z17OS_ScreenGetWidthv"), (uintptr_t)OS_ScreenGetWidth);
  hook_addr(so_symbol(&bully_mod, "_Z18OS_ScreenGetHeightv"), (uintptr_t)OS_ScreenGetHeight);

  // TODO: set deviceChip, definedDevice
  hook_addr(so_symbol(&bully_mod, "_Z20AND_SystemInitializev"), (uintptr_t)ret0);

  // TODO: implement touch here
  hook_addr(so_symbol(&bully_mod, "_Z13ProcessEventsb"), (uintptr_t)ProcessEvents);

  // no touch sense.
  hook_addr(so_symbol(&bully_mod, "_ZN10TouchSenseC2Ev"), (uintptr_t)TouchSense__TouchSense);
  hook_addr(so_symbol(&bully_mod, "_ZN10TouchSense20stopContinuousEffectEv"), (uintptr_t)ret0);
  hook_addr(so_symbol(&bully_mod, "_ZN10TouchSense14stopAllEffectsEv"), (uintptr_t)ret0);
  hook_addr(so_symbol(&bully_mod, "_ZN10TouchSense28startContinuousBuiltinEffectEiiii"), (uintptr_t)ret0);
  hook_addr(so_symbol(&bully_mod, "_ZN10TouchSense25playBuiltinEffectInternalEii"), (uintptr_t)ret0);
  hook_addr(so_symbol(&bully_mod, "_ZN10TouchSense17playBuiltinEffectEiiii"), (uintptr_t)ret0);

  ZIPFile__EntryCompare = (void *)so_symbol(&bully_mod, "_ZN7ZIPFile12EntryCompareEPKvS1_");
  hook_addr(so_symbol(&bully_mod, "_ZN7ZIPFile11SortEntriesEv"), (uintptr_t)ZIPFile__SortEntries);

  hook_addr(so_symbol(&bully_mod, "_ZN11Application4ExitEv"), (uintptr_t)Application__Exit);

  // load latest save
  FadeLoadScene__Loading = (void *)so_symbol(&bully_mod, "_ZN13FadeLoadScene7LoadingEv");
  cMemCard__HasSave = (void *)so_symbol(&bully_mod, "_ZN8cMemCard7HasSaveE11MemCardSlot");
  BullyApplication__OrigLoadSlot = (void *)so_symbol(&bully_mod, "_ZN16BullyApplication12OrigLoadSlotE11MemCardSlot");
  OS_FileGetDate = (void *)so_symbol(&bully_mod, "_Z14OS_FileGetDate14OSFileDataAreaPKc");
  hook_addr(so_symbol(&bully_mod, "_ZN16BullyApplication12OrigContinueEv"), (uintptr_t)BullyApplication__OrigContinue);
}

extern void *__cxa_atexit;
extern void *__cxa_finalize;

static const short _C_tolower_[] = {
  -1,
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
  0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
  0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
  0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
  0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
  0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
  0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
  0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
  0x40, 'a',  'b',  'c',  'd',  'e',  'f',  'g',
  'h',  'i',  'j',  'k',  'l',  'm',  'n',  'o',
  'p',  'q',  'r',  's',  't',  'u',  'v',  'w',
  'x',  'y',  'z',  0x5b, 0x5c, 0x5d, 0x5e, 0x5f,
  0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67,
  0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f,
  0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
  0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f,
  0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
  0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
  0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
  0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f,
  0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
  0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
  0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
  0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf,
  0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
  0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf,
  0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
  0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf,
  0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7,
  0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef,
  0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
  0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff
};

const short *_tolower_tab_ = _C_tolower_;

static char *__ctype_ = (char *)&_ctype_;

static FILE __sF_fake[0x100][3];

int __isfinitef(float d) {
  return isfinite(d);
}

int stat_hook(const char *pathname, void *statbuf) {
  struct stat st;
  int res = stat(pathname, &st);
  if (res == 0)
    *(int *)(statbuf + 0x50) = st.st_mtime;
  return res;
}






extern void *__aeabi_dcmplt;
extern void *__aeabi_dmul;
extern void *__aeabi_dsub;
extern void *__aeabi_f2d;
extern void *__aeabi_fcmplt;
extern void *__aeabi_idiv;
extern void *__aeabi_idivmod;
extern void *__aeabi_l2d;
extern void *__aeabi_l2f;
extern void *__aeabi_ui2d;
extern void *__aeabi_ldiv0;
extern void *__aeabi_uidiv;
extern void *__aeabi_uidivmod;
extern void *__aeabi_ul2d;
extern void *__aeabi_ul2f;
extern void *__aeabi_uldivmod;

extern void *__gnu_ldivmod_helper;

static FILE *stderr_fake;
static FILE *stdin_fake;

static so_default_dynlib default_dynlib[] = {
  { "__android_log_assert", (uintptr_t)&__android_log_assert },
  { "__android_log_print", (uintptr_t)&__android_log_print },
  { "__android_log_vprint", (uintptr_t)&__android_log_vprint },
  { "__android_log_write", (uintptr_t)&__android_log_write },

  { "__cxa_atexit", (uintptr_t)&__cxa_atexit },
  { "__cxa_finalize", (uintptr_t)&__cxa_finalize },
  { "__errno", (uintptr_t)&__errno },
  { "__isfinitef", (uintptr_t)&__isfinitef },
  { "__sF", (uintptr_t)&__sF_fake },
  { "_ctype_", (uintptr_t)&__ctype_ },
  { "_tolower_tab_", (uintptr_t)&_tolower_tab_ },

  { "AAsset_close", (uintptr_t)&ret0 },
  { "AAsset_getLength", (uintptr_t)&ret0 },
  { "AAsset_getRemainingLength", (uintptr_t)&ret0 },
  { "AAsset_read", (uintptr_t)&ret0 },
  { "AAsset_seek", (uintptr_t)&ret0 },
  { "AAssetManager_fromJava", (uintptr_t)&ret0 },
  { "AAssetManager_open", (uintptr_t)&ret0 },

  { "touchsensesdk_addResource", (uintptr_t)&ret0 },

  { "abort", (uintptr_t)&abort },
  { "exit", (uintptr_t)&exit },

  { "acosf", (uintptr_t)&acosf },
  { "asinf", (uintptr_t)&asinf },
  { "atan", (uintptr_t)&atan },
  { "atan2", (uintptr_t)&atan2 },
  { "atan2f", (uintptr_t)&atan2f },
  { "atanf", (uintptr_t)&atanf },
  { "cbrt", (uintptr_t)&cbrt },
  { "ceilf", (uintptr_t)&ceilf },
  { "cos", (uintptr_t)&cos },
  { "cosf", (uintptr_t)&cosf },
  { "exp", (uintptr_t)&exp },
  { "floor", (uintptr_t)&floor },
  { "floorf", (uintptr_t)&floorf },
  { "fmod", (uintptr_t)&fmod },
  { "fmodf", (uintptr_t)&fmodf },
  { "ldexp", (uintptr_t)&ldexp },
  { "log", (uintptr_t)&log },
  { "log10f", (uintptr_t)&log10f },
  { "logf", (uintptr_t)&logf },
  { "pow", (uintptr_t)&pow },
  { "powf", (uintptr_t)&powf },
  { "sin", (uintptr_t)&sin },
  { "sinf", (uintptr_t)&sinf },
  { "sqrtf", (uintptr_t)&sqrtf },
  { "tan", (uintptr_t)&tan },
  { "tanf", (uintptr_t)&tanf },

#ifdef LOADER_ALLOC_TRACE
  // Same allocators, wrapped so the trace can say who is holding the heap.
  { "calloc", (uintptr_t)&bully_calloc },
  { "free", (uintptr_t)&bully_free },
  { "malloc", (uintptr_t)&bully_malloc },
  { "memalign", (uintptr_t)&bully_memalign },
  { "realloc", (uintptr_t)&bully_realloc },
#else
  { "calloc", (uintptr_t)&calloc },
  { "free", (uintptr_t)&free },
  { "malloc", (uintptr_t)&malloc },
  { "memalign", (uintptr_t)&memalign },
  { "realloc", (uintptr_t)&realloc },
#endif

  { "atoi", (uintptr_t)&atoi },

  { "clock_gettime", (uintptr_t)&clock_gettime },
  { "gettimeofday", (uintptr_t)&gettimeofday },
  { "localtime_r", (uintptr_t)&localtime_r },
  { "time", (uintptr_t)&time },

  // { "close", (uintptr_t)&close },
  // { "closedir", (uintptr_t)&closedir },
  // { "ioctl", (uintptr_t)&ioctl },
  // { "lseek", (uintptr_t)&lseek },
  { "mkdir", (uintptr_t)&mkdir },
  // { "open", (uintptr_t)&open },
  // { "opendir", (uintptr_t)&opendir },
  // { "read", (uintptr_t)&read },
  // { "readdir", (uintptr_t)&readdir },
  { "stat", (uintptr_t)&stat_hook },
  // { "write", (uintptr_t)&write },

  { "eglGetCurrentContext", (uintptr_t)&ret0 },
  // { "eglGetDisplay", (uintptr_t)&eglGetDisplay },
  // { "eglGetProcAddress", (uintptr_t)&eglGetProcAddress },
  // { "eglQueryString", (uintptr_t)&eglQueryString },

  { "fclose", (uintptr_t)&sceLibcBridge_fclose },
  // { "fdopen", (uintptr_t)&fdopen },
  // { "fflush", (uintptr_t)&fflush },
  // { "fgetc", (uintptr_t)&fgetc },
  // { "fgets", (uintptr_t)&fgets },

  { "fopen", (uintptr_t)&sceLibcBridge_fopen },

  { "fprintf", (uintptr_t)&sceLibcBridge_fprintf },
  // { "fputc", (uintptr_t)&sceLibcBridge_fputc },
  // { "fputs", (uintptr_t)&sceLibcBridge_fputs },
  { "fread", (uintptr_t)&sceLibcBridge_fread },
  { "fseek", (uintptr_t)&sceLibcBridge_fseek },
  { "ftell", (uintptr_t)&sceLibcBridge_ftell },
  { "fwrite", (uintptr_t)&sceLibcBridge_fwrite },

  // { "getc", (uintptr_t)&getc },
  // { "ungetc", (uintptr_t)&ungetc },

  { "getenv", (uintptr_t)&getenv },
  // { "gettid", (uintptr_t)&gettid },

  { "glActiveTexture", (uintptr_t)&glActiveTextureHook },
  { "glAttachShader", (uintptr_t)&glAttachShader },

  { "glBindAttribLocation", (uintptr_t)&glBindAttribLocation },
  { "glBindBuffer", (uintptr_t)&glBindBuffer },
  { "glBindFramebuffer", (uintptr_t)&glBindFramebuffer },
  { "glBindRenderbuffer", (uintptr_t)&ret0 },
  { "glBindTexture", (uintptr_t)&glBindTextureHook },
  { "glBlendFunc", (uintptr_t)&glBlendFunc },
  { "glBlendFuncSeparate", (uintptr_t)&glBlendFuncSeparate },
  { "glBufferData", (uintptr_t)&glBufferData },

  { "glClear", (uintptr_t)&glClear },

  { "glClearColor", (uintptr_t)&glClearColor },
  { "glClearDepthf", (uintptr_t)&glClearDepthf },
  { "glClearStencil", (uintptr_t)&glClearStencil },
  { "glColorMask", (uintptr_t)&glColorMask },
  { "glCompileShader", (uintptr_t)&glCompileShader },
  { "glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2DHook },
  { "glCompressedTexSubImage2D", (uintptr_t)&ret0 }, // TODO
  { "glCreateProgram", (uintptr_t)&glCreateProgram },

  { "glCreateShader", (uintptr_t)&glCreateShader },
  { "glCullFace", (uintptr_t)&glCullFace },
  { "glDeleteBuffers", (uintptr_t)&glDeleteBuffers },
  { "glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers },
  { "glDeleteProgram", (uintptr_t)&glDeleteProgram },
  { "glDeleteRenderbuffers", (uintptr_t)&ret0 },
  { "glDeleteShader", (uintptr_t)&glDeleteShader },
  { "glDeleteTextures", (uintptr_t)&glDeleteTexturesHook },
  { "glDepthFunc", (uintptr_t)&glDepthFunc },
  { "glDepthMask", (uintptr_t)&glDepthMask },
  { "glDisable", (uintptr_t)&glDisable },
  { "glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray },
  { "glDrawElements", (uintptr_t)&glDrawElementsHook },
  { "glEnable", (uintptr_t)&glEnable },
  { "glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray },
  { "glFinish", (uintptr_t)&glFinish },
  { "glFramebufferRenderbuffer", (uintptr_t)&ret0 },
  { "glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2DHook },
  { "glFrontFace", (uintptr_t)&glFrontFace },
  { "glGenBuffers", (uintptr_t)&glGenBuffers },
  { "glGenFramebuffers", (uintptr_t)&glGenFramebuffers },
  { "glGenRenderbuffers", (uintptr_t)&ret0 },
  { "glGenTextures", (uintptr_t)&glGenTexturesHook },
  { "glGetAttribLocation", (uintptr_t)&glGetAttribLocation },
  { "glGetBooleanv", (uintptr_t)&glGetBooleanv },
  { "glGetError", (uintptr_t)&glGetError },
  { "glGetIntegerv", (uintptr_t)&glGetIntegerv },
  { "glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLog },
  { "glGetProgramiv", (uintptr_t)&glGetProgramiv },

  { "glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLog },
  { "glGetShaderiv", (uintptr_t)&glGetShaderiv },
  { "glGetString", (uintptr_t)&glGetString },
  { "glGetUniformLocation", (uintptr_t)&glGetUniformLocation },
  { "glLineWidth", (uintptr_t)&glLineWidth },
  { "glLinkProgram", (uintptr_t)&glLinkProgramHook },
  { "glPolygonOffset", (uintptr_t)&glPolygonOffset },
  { "glReadPixels", (uintptr_t)&glReadPixels },
  { "glRenderbufferStorage", (uintptr_t)&ret0 },
  { "glScissor", (uintptr_t)&glScissor },
  { "glShaderSource", (uintptr_t)&glShaderSource },
  { "glStencilFunc", (uintptr_t)&glStencilFunc },
  { "glStencilMask", (uintptr_t)&glStencilMask },
  { "glStencilOp", (uintptr_t)&glStencilOp },
  { "glTexImage2D", (uintptr_t)&glTexImage2DHook },
  { "glTexParameterf", (uintptr_t)&glTexParameterfHook },
  { "glTexParameteri", (uintptr_t)&glTexParameteriHook },
  { "glTexSubImage2D", (uintptr_t)&glTexSubImage2DHook },
  { "glUniform1i", (uintptr_t)&glUniform1i },
  { "glUniform4fv", (uintptr_t)&glUniform4fv },
  { "glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv },
  { "glUseProgram", (uintptr_t)&glUseProgram },

  { "glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer },

  { "glViewport", (uintptr_t)&glViewport },


  { "longjmp", (uintptr_t)&longjmp },
  { "setjmp", (uintptr_t)&setjmp },

  { "lrand48", (uintptr_t)&lrand48 },
  { "srand48", (uintptr_t)&srand48 },

  { "memchr", (uintptr_t)&sceClibMemchr },
  { "memcmp", (uintptr_t)&sceClibMemcmp },
  { "memcpy", (uintptr_t)&sceClibMemcpy },
  { "memmove", (uintptr_t)&sceClibMemmove },
  { "memset", (uintptr_t)&sceClibMemset },

  // { "nanosleep", (uintptr_t)&nanosleep },
  { "usleep", (uintptr_t)&usleep },

  { "pthread_attr_destroy", (uintptr_t)&ret0 },
  // { "pthread_attr_getschedparam", (uintptr_t)&pthread_attr_getschedparam },
  // { "pthread_attr_getstacksize", (uintptr_t)&pthread_attr_getstacksize },
  // { "pthread_attr_init", (uintptr_t)&pthread_attr_init },
  // { "pthread_attr_setschedparam", (uintptr_t)&pthread_attr_setschedparam },
  // { "pthread_attr_setstacksize", (uintptr_t)&pthread_attr_setstacksize },
  // { "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast },
  // { "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy },
  { "pthread_cond_init", (uintptr_t)&ret0 },
  // { "pthread_cond_signal", (uintptr_t)&pthread_cond_signal },
  // { "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait },
  // { "pthread_cond_timeout_np", (uintptr_t)&pthread_cond_timeout_np },
  // { "pthread_cond_wait", (uintptr_t)&pthread_cond_wait },
  { "pthread_create", (uintptr_t)&pthread_create_fake },
  { "pthread_getspecific", (uintptr_t)&pthread_getspecific },
  // { "pthread_join", (uintptr_t)&pthread_join },
  { "pthread_key_create", (uintptr_t)&pthread_key_create },
  // { "pthread_key_delete", (uintptr_t)&pthread_key_delete },
  { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake },
  { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake },
  { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake },
  { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake },
  { "pthread_mutexattr_destroy", (uintptr_t)&ret0 },
  { "pthread_mutexattr_init", (uintptr_t)&ret0 },
  { "pthread_mutexattr_settype", (uintptr_t)&ret0 },
  // { "pthread_once", (uintptr_t)&pthread_once },
  // { "pthread_self", (uintptr_t)&pthread_self },
  // { "pthread_setname_np", (uintptr_t)&pthread_setname_np },
  // { "pthread_setschedparam", (uintptr_t)&pthread_setschedparam },
  { "pthread_setspecific", (uintptr_t)&pthread_setspecific },

  { "printf", (uintptr_t)&printf },
  { "putchar", (uintptr_t)&putchar },
  { "puts", (uintptr_t)&puts },

  { "bsearch", (uintptr_t)&bsearch },
  { "qsort", (uintptr_t)&qsort },

  { "sigaction", (uintptr_t)&ret0 },

  { "snprintf", (uintptr_t)&snprintf },
  { "sprintf", (uintptr_t)&sprintf },

  { "sscanf", (uintptr_t)&sscanf },

  { "strcasecmp", (uintptr_t)&strcasecmp },
  { "strcat", (uintptr_t)&strcat },
  { "strchr", (uintptr_t)&strchr },
  { "strcmp", (uintptr_t)&strcmp },
  { "strcoll", (uintptr_t)&strcoll },
  { "strcpy", (uintptr_t)&strcpy },
  { "strcspn", (uintptr_t)&strcspn },
  { "strerror", (uintptr_t)&strerror },
  { "strftime", (uintptr_t)&strftime },
  { "strlen", (uintptr_t)&strlen },
  { "strncasecmp", (uintptr_t)&sceClibStrncasecmp },
  { "strncat", (uintptr_t)&sceClibStrncat },
  { "strncmp", (uintptr_t)&sceClibStrncmp },
  { "strncpy", (uintptr_t)&sceClibStrncpy },
  { "strpbrk", (uintptr_t)&strpbrk },
  { "strrchr", (uintptr_t)&sceClibStrrchr },
  { "strspn", (uintptr_t)&strspn },
  { "strstr", (uintptr_t)&sceClibStrstr },
  { "strtod", (uintptr_t)&strtod },
  { "strtok", (uintptr_t)&strtok },
  { "strtol", (uintptr_t)&strtol },
  { "strtoul", (uintptr_t)&strtoul },
  { "strxfrm", (uintptr_t)&strxfrm },

  { "vprintf", (uintptr_t)&vprintf },
  { "vsnprintf", (uintptr_t)&vsnprintf },
  { "vsprintf", (uintptr_t)&vsprintf },

  { "btowc", (uintptr_t)&btowc },
  { "iswctype", (uintptr_t)&iswctype },
  { "mbrtowc", (uintptr_t)&mbrtowc },
  { "towlower", (uintptr_t)&towlower },
  { "towupper", (uintptr_t)&towupper },

  { "wcrtomb", (uintptr_t)&wcrtomb },
  { "wcscoll", (uintptr_t)&wcscoll },
  { "wcsftime", (uintptr_t)&wcsftime },
  { "wcslen", (uintptr_t)&wcslen },
  { "wcsxfrm", (uintptr_t)&wcsxfrm },
  { "wctob", (uintptr_t)&wctob },
  { "wctype", (uintptr_t)&wctype },
  { "wmemchr", (uintptr_t)&wmemchr },
  { "wmemcmp", (uintptr_t)&wmemcmp },
  { "wmemcpy", (uintptr_t)&wmemcpy },
  { "wmemmove", (uintptr_t)&wmemmove },
  { "wmemset", (uintptr_t)&wmemset },

  // 1.0.0.18 imports

  { "__aeabi_dcmplt", (uintptr_t)&__aeabi_dcmplt },
  { "__aeabi_dmul", (uintptr_t)&__aeabi_dmul },
  { "__aeabi_dsub", (uintptr_t)&__aeabi_dsub },
  { "__aeabi_f2d", (uintptr_t)&__aeabi_f2d },
  { "__aeabi_fcmplt", (uintptr_t)&__aeabi_fcmplt },
  { "__aeabi_idiv", (uintptr_t)&__aeabi_idiv },
  { "__aeabi_idivmod", (uintptr_t)&__aeabi_idivmod },
  { "__aeabi_l2d", (uintptr_t)&__aeabi_l2d },
  { "__aeabi_l2f", (uintptr_t)&__aeabi_l2f },
  { "__aeabi_ldiv0", (uintptr_t)&__aeabi_ldiv0 },
  { "__aeabi_ui2d", (uintptr_t)&__aeabi_ui2d },
  { "__aeabi_uidiv", (uintptr_t)&__aeabi_uidiv },
  { "__aeabi_uidivmod", (uintptr_t)&__aeabi_uidivmod },
  { "__aeabi_ul2d", (uintptr_t)&__aeabi_ul2d },
  { "__aeabi_ul2f", (uintptr_t)&__aeabi_ul2f },
  { "__aeabi_uldivmod", (uintptr_t)&__aeabi_uldivmod },

  { "__gnu_ldivmod_helper", (uintptr_t)&__gnu_ldivmod_helper },

  { "islower", (uintptr_t)&islower },
  { "isprint", (uintptr_t)&isprint },
  { "isspace", (uintptr_t)&isspace },

  { "atof", (uintptr_t)&atof },
  { "tolower", (uintptr_t)&tolower },

  { "rand", (uintptr_t)&rand },
  { "srand", (uintptr_t)&srand },

  { "feof", (uintptr_t)&feof },
  { "ferror", (uintptr_t)&ferror },

  { "sigemptyset", (uintptr_t)&ret0 },

  { "clearerr", (uintptr_t)&clearerr },
  { "stderr", (uintptr_t)&stderr_fake },
  { "stdin", (uintptr_t)&stdin_fake },

  { "stpcpy", (uintptr_t)&stpcpy },
  { "strtof", (uintptr_t)&strtof },
};

int check_capunlocker(void) {
  int search_unk[2];
  return _vshKernelSearchModuleByName("CapUnlocker", search_unk);
}

int check_kubridge(void) {
  int search_unk[2];
  return _vshKernelSearchModuleByName("kubridge", search_unk);
}

int file_exists(const char *path) {
  SceIoStat stat;
  return sceIoGetstat(path, &stat) >= 0;
}

int main(int argc, char *argv[]) {
  sceKernelChangeThreadPriority(0, 127);
  sceKernelChangeThreadCpuAffinityMask(0, 0x40000);

  sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);
  sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
  sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &panelInfoFront);

  scePowerSetArmClockFrequency(444);
  scePowerSetBusClockFrequency(222);
  scePowerSetGpuClockFrequency(222);
  scePowerSetGpuXbarClockFrequency(166);

  sceIoMkdir(GLSL_PATH, 0777);

  capunlocker_enabled = check_capunlocker() >= 0;
  if (capunlocker_enabled) {
    _oal_thread_priority = 64;
    _oal_thread_affinity = 0x80000;
  } else {
    _oal_thread_priority = 64;
    _oal_thread_affinity = 0x10000;
  }

  // Stamped so a trace file can never be ambiguous about which build wrote it.
  traceLog("---- loader built %s %s ----\n", __DATE__, __TIME__);
  traceLog("boot: reached kubridge check\n");
  if (check_kubridge() < 0)
    fatal_error("Error kubridge.skprx is not installed.");

  // The game's shaders are compiled at runtime now, so this module is required.
  // Without it every shader fails and the screen is simply black, which is not
  // a state anybody should have to diagnose.
  if (!file_exists("ur0:/data/libshacccg.suprx") && !file_exists("ur0:/data/external/libshacccg.suprx"))
    fatal_error("Error libshacccg.suprx is not installed.");

  traceLog("boot: loading %s\n", SO_PATH);
  if (so_load(&bully_mod, SO_PATH, LOAD_ADDRESS) < 0)
    fatal_error("Error could not load %s.", SO_PATH);

  stderr_fake = stderr;
  stdin_fake = stdin;
  traceLog("boot: so_load ok, relocating\n");
  so_relocate(&bully_mod);
  so_resolve(&bully_mod, default_dynlib, sizeof(default_dynlib), 0);

  traceLog("boot: resolved imports, patching\n");
  patch_openal();
  patch_game();
  patch_movie();
  so_flush_caches(&bully_mod);

  traceLog("boot: patched, running .so initializers\n");
  so_initialize(&bully_mod);

  traceLog("boot: initializers done, starting fios\n");
  if (fios_init() < 0)
    fatal_error("Error could not initialize fios.");

  traceLog("boot: fios ok, starting texture cache\n");
  texture_cache_init();

  traceLog("boot: texture cache done, initialising vitaGL\n");
  // The game ships GLSL and this hands it to vitaGL's runtime compiler, which
  // caches the compiled result on the card. The precompiled .gxp route this
  // port used instead only works with the 2021 vitaGL: current versions read a
  // glShaderBinary payload as their own serialized container, and a bare GXP
  // wrapped to satisfy that parser registers a program that links, draws
  // without error and rasterises nothing. Requires libshacccg.suprx.
  // Semantics can only be resolved accurately when vitaGL can see a vertex and
  // fragment shader together. This engine compiles some forty shaders during
  // startup and does not create a program from any of them until much later,
  // so the pair mode layton3-vita uses does not hold here and the global pool
  // vitaGL defaults to is the least accurate option. Postponing compilation to
  // glLinkProgram always gives it the right couple.
  vglSetTextureCacheFrequency(TEXTURE_CACHE_IDLE_FRAMES);
  vglSetSemanticBindingMode(VGL_MODE_POSTPONED);
  vglSetupRuntimeShaderCompiler(SHARK_OPT_UNSAFE, SHARK_ENABLE, SHARK_ENABLE, SHARK_ENABLE);
  vglSetupGarbageCollector(127, 0x20000);
  vglInitExtended(0, SCREEN_W, SCREEN_H, MEMORY_VITAGL_THRESHOLD_MB * 1024 * 1024, SCE_GXM_MULTISAMPLE_2X);

  // Do not let textures overflow into the newlib heap.
  //
  // vitaGL's texture allocator tries CDRAM, then RAM, then phycont, then the
  // CDLG budget, and then -- because use_extra_mem defaults to GL_TRUE -- it
  // calls memalign and takes the memory out of the game's own heap. And
  // vgl_mem_get_free_space(VGL_MEM_EXTERNAL) returns 0 unconditionally, so none
  // of it is visible to anything that asks vitaGL how much memory is left.
  //
  // That is the leak this port has been dying of. It explains a heap that
  // climbed to 155 MB while every pool reading said there was room to spare, a
  // texture cache that never evicted because it could not see any pressure, and
  // 79 MB of live allocations that the allocation trace could not account for
  // -- vitaGL's own mallocs do not come through the wrappers the game's do.
  //
  // With this off the allocation fails instead, which the loader is set up for:
  // an upload vitaGL rejects allocates nothing, texture_is_allocated notices,
  // and the pressure shows up in the pool figures where the texture cache can
  // act on it.
  vglUseExtraMem(GL_FALSE);


  traceLog("boot: vitaGL up (%s / %s), setting up movie player\n",
           (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER));

  movie_setup_player();

  traceLog("boot: handing over to the game\n");
  jni_load();

  return 0;
}
