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
#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>

#include <dirent.h>
#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int debugPrintf(char *text, ...) { (void)text; return 0; }
int traceLog(char *text, ...) { (void)text; return 0; }
// The loader's heartbeat counters, which live in main.c on the console. Defined
// here so the tests compile the cache with the trace on, exactly as it ships.
int trace_textures;

// Set by a test to hold the writer thread inside its write, so that "this copy
// has not reached the card yet" is a state the test can actually observe.
volatile int fake_stall_writes;

// Textures live one per file in hashed subdirectories now, so the mapping has
// to keep the path shape rather than flattening to a basename. Everything under
// the game's data directory is rooted at TEXCACHE_DIR.
static const char *host_path(const char *vita_path) {
  static char buf[512];
  const char *base = getenv("TEXCACHE_DIR");
  const char *rel = strstr(vita_path, "Bully/");
  if (rel)
    rel += strlen("Bully/");
  else {
    const char *slash = strrchr(vita_path, '/');
    rel = slash ? slash + 1 : vita_path;
  }
  snprintf(buf, sizeof(buf), "%s/%s", base ? base : ".", rel);
  return buf;
}

// What the store is doing, so a test can assert on it rather than on an
// allocator's internals.
static unsigned store_writes;
unsigned fake_store_writes(void) { return store_writes; }

unsigned fake_store_files(void) {
  const char *base = getenv("TEXCACHE_DIR");
  char cmd[512];
  snprintf(cmd, sizeof(cmd), "find '%s' -name '*.tex' 2>/dev/null | wc -l", base ? base : ".");
  FILE *p = popen(cmd, "r");
  unsigned n = 0;
  if (p) {
    if (fscanf(p, "%u", &n) != 1)
      n = 0;
    pclose(p);
  }
  return n;
}

SceUID sceIoOpen(const char *file, int flags, SceMode mode) {
  int f = 0;
  if ((flags & SCE_O_RDWR) == SCE_O_RDWR) f |= O_RDWR;
  else if (flags & SCE_O_WRONLY) f |= O_WRONLY;
  else f |= O_RDONLY;
  if (flags & SCE_O_CREAT) f |= O_CREAT;
  if (flags & SCE_O_TRUNC) f |= O_TRUNC;
  if (flags & SCE_O_APPEND) f |= O_APPEND;
  if (f & O_CREAT)
    store_writes++;
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
int sceIoMkdir(const char *dir, SceMode mode) { (void)mode; return mkdir(host_path(dir), 0777); }
int sceIoRemove(const char *file) { return unlink(host_path(file)); }
int sceIoRmdir(const char *dir) { return rmdir(host_path(dir)); }

// Directory listing, used by the store's purge. SceUID is an int and DIR* is a
// pointer, so the handles live in a small table rather than being cast.
static DIR *open_dirs[16];
SceUID sceIoDopen(const char *dirname) {
  DIR *d = opendir(host_path(dirname));
  if (!d)
    return -1;
  for (int i = 0; i < 16; i++) {
    if (!open_dirs[i]) {
      open_dirs[i] = d;
      return i;
    }
  }
  closedir(d);
  return -1;
}
int sceIoDread(SceUID fd, SceIoDirent *dir) {
  if (fd < 0 || fd >= 16 || !open_dirs[fd])
    return -1;
  struct dirent *e = readdir(open_dirs[fd]);
  if (!e)
    return 0;
  snprintf(dir->d_name, sizeof(dir->d_name), "%s", e->d_name);
  return 1;
}
int sceIoDclose(SceUID fd) {
  if (fd < 0 || fd >= 16 || !open_dirs[fd])
    return -1;
  closedir(open_dirs[fd]);
  open_dirs[fd] = NULL;
  return 0;
}

// Free space on the card. Tests set fake_card_free to exercise both the
// plenty-of-room path and the too-tight-to-bother path.
uint64_t fake_card_free = 4ull * 1024 * 1024 * 1024;
int sceAppMgrGetDevInfo(const char *dev, uint64_t *max_size, uint64_t *free_size) {
  if (max_size) *max_size = 8ull * 1024 * 1024 * 1024;
  if (free_size) *free_size = fake_card_free;
  return 0;
}

// The cache checks for a file that turns it off. Tests want it on, so report
// that it is not there -- unless a test deliberately creates it.
int sceIoGetstat(const char *file, SceIoStat *out) {
  struct stat probe;
  if (stat(host_path(file), &probe) != 0)
    return -1;
  // st_size matters: the cache uses it to decide whether a texture it is about
  // to write is already stored, so leaving it unset made every hit look like a
  // miss.
  if (out)
    out->st_size = probe.st_size;
  return 0;
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

// The cache asks the heap how full it is before parking a texture there. On the
// host that answer would be the test process's own heap, which says nothing
// about the console, so a test sets it directly.
size_t fake_heap_used;
struct mallinfo mallinfo(void) {
  struct mallinfo m;
  memset(&m, 0, sizeof(m));
  m.uordblks = (int)fake_heap_used;
  return m;
}
