/* fake_vita_os.c -- the handful of Vita OS calls the texture cache makes,
 * implemented on POSIX so the cache can be exercised on a build machine.
 *
 * Real files and real threads: the writer thread, the semaphore handshake and
 * the on-disk record format are genuinely exercised rather than mocked away.
 * Paths under ux0: are redirected into $TEXCACHE_DIR.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#define _GNU_SOURCE

// The Vita headers come first on purpose: SceIoStat has a field called st_ctime
// and glibc defines that as a macro, so whichever is parsed second loses.
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>

#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int debugPrintf(char *text, ...) { (void)text; return 0; }

// Set by a test to hold the writer thread inside its write, so that "this copy
// has not reached the card yet" is a state the test can actually observe.
volatile int fake_stall_writes;

static const char *host_path(const char *vita_path) {
  static char buf[512];
  const char *base = getenv("TEXCACHE_DIR");
  const char *slash = strrchr(vita_path, '/');
  snprintf(buf, sizeof(buf), "%s/%s", base ? base : ".", slash ? slash + 1 : vita_path);
  return buf;
}

SceUID sceIoOpen(const char *file, int flags, SceMode mode) {
  int f = 0;
  if ((flags & SCE_O_RDWR) == SCE_O_RDWR) f |= O_RDWR;
  else if (flags & SCE_O_WRONLY) f |= O_WRONLY;
  else f |= O_RDONLY;
  if (flags & SCE_O_CREAT) f |= O_CREAT;
  if (flags & SCE_O_TRUNC) f |= O_TRUNC;
  int fd = open(host_path(file), f, 0666);
  return fd < 0 ? -1 : fd;
}
int sceIoClose(SceUID fd) { return close(fd); }
int sceIoWrite(SceUID fd, const void *data, SceSize size) {
  while (__atomic_load_n(&fake_stall_writes, __ATOMIC_ACQUIRE))
    usleep(500);
  return (int)write(fd, data, size);
}
int sceIoRead(SceUID fd, void *data, SceSize size) {
  size_t got = 0;
  while (got < size) {
    ssize_t r = read(fd, (char *)data + got, size - got);
    if (r <= 0) break;
    got += r;
  }
  return (int)got;
}
SceOff sceIoLseek(SceUID fd, SceOff offset, int whence) { return lseek(fd, offset, SEEK_SET); }
int sceIoMkdir(const char *dir, SceMode mode) { return mkdir(host_path(dir), 0777); }
int sceIoRemove(const char *file) { return unlink(host_path(file)); }

// The cache checks for a file that turns it off. Tests want it on, so report
// that it is not there -- unless a test deliberately creates it.
int sceIoGetstat(const char *file, SceIoStat *out) {
  struct stat probe;
  return stat(host_path(file), &probe) == 0 ? 0 : -1;
}

#define MAX_SEMA 8
static sem_t semas[MAX_SEMA];
static int sema_count;

SceUID sceKernelCreateSema(const char *name, SceUInt attr, int initVal, int maxVal,
                           SceKernelSemaOptParam *option) {
  if (sema_count >= MAX_SEMA) return -1;
  sem_init(&semas[sema_count], 0, initVal);
  return sema_count++;
}
int sceKernelSignalSema(SceUID semaid, int signal) {
  while (signal-- > 0) sem_post(&semas[semaid]);
  return 0;
}
int sceKernelWaitSema(SceUID semaid, int signal, SceUInt *timeout) {
  while (signal-- > 0) sem_wait(&semas[semaid]);
  return 0;
}
int sceKernelDeleteSema(SceUID semaid) { return 0; }
int sceKernelDelayThread(SceUInt delay) { return usleep(delay); }

#define MAX_THREADS 8
typedef struct { SceKernelThreadEntry entry; pthread_t thread; } HostThread;
static HostThread threads[MAX_THREADS];
static int thread_count;

static void *trampoline(void *arg) {
  HostThread *t = arg;
  t->entry(0, NULL);
  return NULL;
}
SceUID sceKernelCreateThread(const char *name, SceKernelThreadEntry entry, int initPriority,
                             SceSize stackSize, SceUInt attr, int cpuAffinityMask,
                             const SceKernelThreadOptParam *option) {
  if (thread_count >= MAX_THREADS) return -1;
  threads[thread_count].entry = entry;
  return thread_count++;
}
int sceKernelStartThread(SceUID thid, SceSize arglen, void *argp) {
  return pthread_create(&threads[thid].thread, NULL, trampoline, &threads[thid]);
}
int sceKernelExitDeleteThread(int status) { pthread_exit(NULL); return 0; }
