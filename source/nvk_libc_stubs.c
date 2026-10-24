/* nvk_libc_stubs.c -- POSIX entry points Mesa needs that Horizon's newlib lacks.
 *
 * libnvk_support.a from the nxvk portlib package pulls in Mesa's disk cache,
 * drirc XML config and a few util paths which were written for a POSIX host.
 * Linking it against devkitA64 leaves these fifteen undefined:
 *
 *   dirfd flock fstatat getegid geteuid getgid getpwuid_r getuid
 *   posix_memalign pthread_sigmask regcomp regexec regfree sysconf writev
 *
 * None of them has a meaning on Horizon: there is one user, no signals, no
 * advisory file locks, no drirc file to parse and no <regex.h>. They only have
 * to exist and behave so Mesa takes its graceful fallbacks. The application
 * explicitly enables the disk cache with MESA_SHADER_CACHE_DIR and sets its
 * size limit before NVK initializes.
 *
 * Only compiled for the pkg-config link path: the vendored 23-archive tree
 * carries its own stubs for these, so defining them there would collide.
 *
 * Types: <regex.h>, <pwd.h> and <sys/uio.h> are not guaranteed to exist in a
 * devkitA64 sysroot, so each is probed with __has_include and a local,
 * ABI-compatible declaration is used when it is missing. Only the symbol name
 * matters at link time and the stubs never dereference those objects.
 */
#ifdef NETHERSX2_NVK_ICD_ONLY

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <signal.h> /* sigset_t */
#include <switch.h> /* svcGetInfo, InfoType_*, CUR_PROCESS_HANDLE */
#include "config.h" /* GPU_RESERVE_MB */

#ifdef NETHERSX2_VK_DIAGNOSTIC
#include "hooks/vk.h" /* vk_diag_note; this whole file is the VK link path */
#endif

/* NVK_STUBS_NO_SYS_HEADERS forces the fallback declarations even on a host that
 * has these headers, which is the only way to compile-check the branch a
 * devkitA64 sysroot without <regex.h>/<pwd.h>/<sys/uio.h> actually takes. */
#if defined(__has_include) && !defined(NVK_STUBS_NO_SYS_HEADERS)
#if __has_include(<pwd.h>)
#include <pwd.h>
#define NVK_HAVE_PWD_H 1
#endif
#if __has_include(<sys/uio.h>)
#include <sys/uio.h>
#define NVK_HAVE_UIO_H 1
#endif
#if __has_include(<regex.h>)
#include <regex.h>
#define NVK_HAVE_REGEX_H 1
#endif
#if __has_include(<fcntl.h>)
#include <fcntl.h>
#endif
#endif

/* Horizon runs a single process as one user. Anything but a non-zero uid would
 * make Mesa's disk_cache_enabled() take its "running as root, refuse" branch. */
#define NVK_FAKE_UID 1000

uid_t getuid(void) { return NVK_FAKE_UID; }
uid_t geteuid(void) { return NVK_FAKE_UID; }
gid_t getgid(void) { return NVK_FAKE_UID; }
gid_t getegid(void) { return NVK_FAKE_UID; }

/* Mesa asks for the home directory to place the shader cache. Report "no such
 * user" so the explicit MESA_SHADER_CACHE_DIR supplied by main.c is used rather
 * than writing under an unintended home directory. */
#ifdef NVK_HAVE_PWD_H
int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen,
               struct passwd **result) {
  (void)uid; (void)pwd; (void)buf; (void)buflen;
  if (result) *result = NULL;
  return ENOENT;
}
#else
int getpwuid_r(unsigned int uid, void *pwd, char *buf, unsigned long buflen,
               void **result) {
  (void)uid; (void)pwd; (void)buf; (void)buflen;
  if (result) *result = NULL;
  return ENOENT;
}
#endif

/* NVK uses os_get_total_physical_memory() as the input to its heap-size
 * heuristic. On this port, report twice the memory deliberately left unmapped
 * by main.c's newlib heap: the pinned NXVK Switch WSI/Mesa build uses the 50%
 * heuristic for this 2 GiB figure, yielding a 1 GiB VkMemoryHeap when the
 * configured GPU_RESERVE_MB is 1024. This keeps the heap estimate aligned with
 * the existing reservation without taking another GiB from newlib.
 *
 * This is a targeted Mesa sizing hint, NOT the console's actual physical-memory
 * total. Do not use it as a general-purpose system-memory query. The former
 * free-remainder approach reported only a few MiB after svcMapProcessCodeMemory
 * and made VMA reject allocations; reporting the full console total made NVK's
 * estimate much larger than the memory explicitly held aside for the GPU. */
static uint64_t nvk_reported_physical_bytes(void) {
  return (uint64_t)GPU_RESERVE_MB * 2u * 1024u * 1024u;
}

long sysconf(int name) {
#ifdef _SC_PAGESIZE
  if (name == _SC_PAGESIZE) return 4096;
#endif
#ifdef _SC_PAGE_SIZE
  if (name == _SC_PAGE_SIZE) return 4096;
#endif
#ifdef _SC_PHYS_PAGES
  if (name == _SC_PHYS_PAGES) {
    const uint64_t bytes = nvk_reported_physical_bytes();
    const long pages = bytes >= 4096 ? (long)(bytes / 4096) : -1L;
#ifdef NETHERSX2_VK_DIAGNOSTIC
    /* Once: distinguish the intentional sysconf sizing hint from actual
     * process use so device logs do not mistake it for total console RAM. */
    static int logged;
    if (!logged) {
      logged = 1;
      uint64_t used = 0;
      (void)svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
      const int pct = bytes <= 1024ull * 1024 * 1024 ? 25
                    : bytes <= 4096ull * 1024 * 1024 ? 50 : 75;
      vk_diag_note("sysconf(_SC_PHYS_PAGES): reported=%llu MiB used=%llu MiB "
                   "-> %ld pages; NVK heap = %d%% = %llu MiB; "
                   "GPU_RESERVE_MB=%u",
                   (unsigned long long)(bytes >> 20),
                   (unsigned long long)(used >> 20),
                   pages, pct,
                   (unsigned long long)((bytes * pct / 100) >> 20),
                   (unsigned)GPU_RESERVE_MB);
    }
#endif
    if (pages > 0) return pages;
  }
#endif
  errno = EINVAL;
  return -1;
}

/* No file descriptor backs a DIR stream on Horizon, and nothing contends for
 * the cache directory, so both are reported as "nothing to do, succeeded". */
int dirfd(DIR *dirp) { (void)dirp; return -1; }
int flock(int fd, int operation) { (void)fd; (void)operation; return 0; }

/* dirfd is always -1 above, so every caller is effectively path-relative. */
int fstatat(int fd, const char *path, struct stat *st, int flags) {
  (void)fd;
  if (!path || !st) { errno = EFAULT; return -1; }
#ifdef AT_SYMLINK_NOFOLLOW
  if ((flags & AT_SYMLINK_NOFOLLOW) && lstat(path, st) == 0) return 0;
#else
  (void)flags;
#endif
  return stat(path, st);
}

int posix_memalign(void **memptr, size_t alignment, size_t size) {
  if (!memptr) return EINVAL;
  if (alignment < sizeof(void *) || (alignment & (alignment - 1))) return EINVAL;
  /* C11 aligned_alloc wants size to be a multiple of alignment. */
  size_t rounded = (size + alignment - 1) & ~(alignment - 1);
  void *p = aligned_alloc(alignment, rounded ? rounded : alignment);
  if (!p) return ENOMEM;
  *memptr = p;
  return 0;
}

/* Signals do not exist on Horizon; Mesa only masks them around thread setup. */
int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset) {
  (void)how; (void)set;
  if (oldset) memset(oldset, 0, sizeof(*oldset));
  return 0;
}

#ifdef NVK_HAVE_UIO_H
ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
#else
struct iovec { void *iov_base; size_t iov_len; };
ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
#endif
  if (!iov || iovcnt < 0) { errno = EFAULT; return -1; }
  ssize_t total = 0;
  for (int i = 0; i < iovcnt; i++) {
    ssize_t n = write(fd, iov[i].iov_base, iov[i].iov_len);
    if (n < 0) return total ? total : -1;
    total += n;
    if ((size_t)n < iov[i].iov_len) break; /* short write: nothing more fits */
  }
  return total;
}

/* drirc pattern matching. There is no config file to read on Horizon, so a
 * pattern never compiles and never matches; Mesa then applies no overrides. */
#ifndef NVK_HAVE_REGEX_H
typedef struct { void *opaque; } regex_t;
typedef struct { size_t rm_so; size_t rm_eo; } regmatch_t;
#define NVK_REG_NOMATCH 1
#else
#define NVK_REG_NOMATCH REG_NOMATCH
#endif

int regcomp(regex_t *preg, const char *regex, int cflags) {
  (void)preg; (void)regex; (void)cflags;
  return NVK_REG_NOMATCH;
}
int regexec(const regex_t *preg, const char *string, size_t nmatch,
            regmatch_t pmatch[], int eflags) {
  (void)preg; (void)string; (void)nmatch; (void)pmatch; (void)eflags;
  return NVK_REG_NOMATCH;
}
void regfree(regex_t *preg) { (void)preg; }

#endif /* NETHERSX2_NVK_ICD_ONLY */
