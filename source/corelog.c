/* corelog.c -- the core's console, captured to the SD card.
 *
 * libemucore.so has exactly one output channel: Bionic's logcat. Every
 * Console.Error and Log::Write in PCSX2 lands in __android_log_print, and every
 * stdout/stderr write lands in a fake FILE that libc_shim.c absorbs. Horizon has
 * no console, so all of it was silently discarded -- which is why a GS that
 * fails to initialise dies with no message anywhere and no way to tell which of
 * a dozen checks tripped.
 *
 * This module gives the core its voice back: one line-buffered file under
 * DATA_ROOT, capped so it cannot fill the card, and fsynced when the core reports
 * an error or is about to die. The abort path records the caller as
 * core+0xOFFSET, which is enough to find the faulting instruction in
 * libemucore.so without a debugger.
 *
 * Always compiled and always on: an abort() that throws away its own reason is a
 * defect in any build, and the file is capped so it cannot fill the card.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include "corelog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"    // DATA_ROOT
#include "so_util.h"   // so_module
#include "libc_shim.h" // shim_describe_address()

// libemucore.so, loaded by main.c (same pattern as dl_emu.c).
extern so_module emu_mod;

#define CORELOG_PATH   DATA_ROOT "/nethersx2-core.log"
/* The shipping build stops writing chatter at CORELOG_LIMIT but keeps a
 * 128 KiB ring of later INFO context in RAM. A wrapper ERROR/FATAL or death note
 * flushes that recent context into the reserved hard-cap headroom before the
 * message itself. This retains useful context without letting an unlimited
 * stream fill the SD card. PCSX2 can flatten its own console levels to INFO, so
 * this is a recent-context guarantee, not a promise to retain every old core
 * error.
 *
 * The diagnostic build has no chatter cutoff and a larger cap: cutting INFO off
 * there would discard the core messages being investigated. Both builds keep a
 * bounded hard limit and a smaller additional budget for error-priority output. */
#ifdef NETHERSX2_VK_DIAGNOSTIC
#define CORELOG_LIMIT  (8 * 1024 * 1024)
#else
#define CORELOG_LIMIT  (2 * 1024 * 1024)
#endif
#define CORELOG_HARD   (CORELOG_LIMIT + 256 * 1024)
// ERROR/FATAL-priority messages may extend this far beyond CORELOG_HARD. The
// context ring itself is written only inside CORELOG_HARD's reserved headroom.
#define CORELOG_TAIL   (64 * 1024)
#ifndef NETHERSX2_VK_DIAGNOSTIC
// Recent INFO context is retained in RAM after the soft cutoff and appended
// when a wrapper error or death note arrives. This stays inside CORELOG_HARD's
// existing 256 KiB headroom, so the on-card log remains bounded.
#define CORELOG_CONTEXT_TAIL (128 * 1024)
#endif
#define CORELOG_LINE   1024

// A line at INFO or below whose exact text was already written within the last
// few distinct lines is counted instead of written. The core emits some things
// unconditionally at INFO during gameplay -- Timezone=-360 and SummerTime=0
// alternate about 110 times a second -- and PCSX2's console sink flattens every
// Log:: level to ANDROID_LOG_INFO, so priority cannot separate that from
// anything worth keeping. Matching on exact text within a small window is
// narrow enough to leave real repeated lines alone (the "# Initialize GS ..."
// block recurs three times, but always more than four distinct lines apart)
// without hardcoding strings that would break on the next core.
#define CORELOG_REPEAT_WINDOW  4
#define CORELOG_REPEAT_REPORT  500

static const char *const corelog_priority[] = {
  "?", "?", "V", "D", "I", "W", "E", "F", "S"
};

// Zero-initialised libnx Mutex is unlocked; hooks/vk.c relies on the same.
static Mutex corelog_mutex;
static FILE *corelog_file;
static char  corelog_abort_text[512];
static long  corelog_written;
static char  corelog_recent[CORELOG_REPEAT_WINDOW][CORELOG_LINE];
static int   corelog_recent_next;
static long  corelog_repeats;
static char  corelog_repeat_text[CORELOG_LINE];
#ifndef NETHERSX2_VK_DIAGNOSTIC
static int   corelog_chatter_stopped; // soft cap hit; later INFO is kept in RAM
static char  corelog_context_tail[CORELOG_CONTEXT_TAIL];
static size_t corelog_context_start;
static size_t corelog_context_size;
#endif

#ifndef NETHERSX2_VK_DIAGNOSTIC
// Caller holds corelog_mutex. Drop one complete oldest line from the context
// ring so the next append always preserves line boundaries.
static void
corelog_context_drop_oldest_locked(void) {
  if (!corelog_context_size)
    return;
  size_t dropped = 0;
  while (dropped < corelog_context_size) {
    const size_t index = (corelog_context_start + dropped) % CORELOG_CONTEXT_TAIL;
    dropped++;
    if (corelog_context_tail[index] == '\n')
      break;
  }
  corelog_context_start = (corelog_context_start + dropped) % CORELOG_CONTEXT_TAIL;
  corelog_context_size -= dropped;
}

static void
corelog_context_append_locked(const char *line, size_t length) {
  if (!length)
    return;
  if (length >= CORELOG_CONTEXT_TAIL) {
    line += length - (CORELOG_CONTEXT_TAIL - 1);
    length = CORELOG_CONTEXT_TAIL - 1;
    corelog_context_start = 0;
    corelog_context_size = 0;
  }
  while (CORELOG_CONTEXT_TAIL - corelog_context_size < length)
    corelog_context_drop_oldest_locked();

  size_t end = (corelog_context_start + corelog_context_size) % CORELOG_CONTEXT_TAIL;
  size_t first = CORELOG_CONTEXT_TAIL - end;
  if (first > length)
    first = length;
  memcpy(corelog_context_tail + end, line, first);
  if (length > first)
    memcpy(corelog_context_tail, line + first, length - first);
  corelog_context_size += length;
}

// Caller holds corelog_mutex. The 256 KiB hard-cap headroom reserves enough
// room for this 128 KiB context ring and the triggering error/death note.
static void
corelog_context_flush_locked(void) {
  size_t available = corelog_written < CORELOG_HARD ?
      (size_t)(CORELOG_HARD - corelog_written) : 0;
  while (corelog_context_size > available)
    corelog_context_drop_oldest_locked();

  if (corelog_context_size) {
    size_t first = CORELOG_CONTEXT_TAIL - corelog_context_start;
    if (first > corelog_context_size)
      first = corelog_context_size;
    size_t written = fwrite(corelog_context_tail + corelog_context_start,
                            1, first, corelog_file);
    if (written == first && corelog_context_size > first)
      written += fwrite(corelog_context_tail, 1,
                        corelog_context_size - first, corelog_file);
    corelog_written += (long)written;
    if (written) {
      fflush(corelog_file);
      fsync(fileno(corelog_file));
    }
  }
  corelog_context_start = 0;
  corelog_context_size = 0;
}

// Format a record into the bounded context buffer without allocating memory.
static void
corelog_context_record_locked(int priority, const char *tag, const char *text) {
  char line[CORELOG_LINE + 256];
  struct timespec time;
  clock_gettime(CLOCK_MONOTONIC, &time);
  const char *level =
      (priority >= 0 && priority <= 8) ? corelog_priority[priority] : "?";
  const int has_tag = (tag && *tag);
  int n = snprintf(line, sizeof(line), "[%lld.%03lld] [%s] %s%s%s\n",
                   (long long)time.tv_sec,
                   (long long)(time.tv_nsec / 1000000),
                   level, has_tag ? tag : "", has_tag ? ": " : "", text);
  if (n <= 0)
    return;
  size_t length = (size_t)n;
  if (length >= sizeof(line))
    length = sizeof(line) - 1;
  if (length && line[length - 1] != '\n') {
    if (length == sizeof(line) - 1)
      line[length - 1] = '\n';
    else
      line[length++] = '\n';
  }
  corelog_context_append_locked(line, length);
}
#endif

// Caller holds corelog_mutex. Count these summary bytes too; otherwise a
// continuously repeated INFO line bypasses the configured file-size ceiling.
static void
corelog_repeat_summary_locked(void) {
#ifndef NETHERSX2_VK_DIAGNOSTIC
  if (corelog_chatter_stopped) {
    char summary[CORELOG_LINE + 64];
    snprintf(summary, sizeof(summary), "-- %ld repeats suppressed, last: %s --",
             corelog_repeats, corelog_repeat_text);
    corelog_context_record_locked(CORELOG_INFO, NULL, summary);
    return;
  }
#endif
  const int n = fprintf(corelog_file, "-- %ld repeats suppressed, last: %s --\n",
                        corelog_repeats, corelog_repeat_text);
  if (n > 0)
    corelog_written += n;
}

// Caller holds corelog_mutex.
static void
corelog_emit_locked(int priority, const char *tag, const char *text) {
  const int important = (priority >= CORELOG_ERROR);

  if (!corelog_file)
    return;

  if (!important) {
    int seen = 0;
    for (int i = 0; i < CORELOG_REPEAT_WINDOW; i++)
      if (corelog_recent[i][0] && !strcmp(corelog_recent[i], text)) { seen = 1; break; }
    if (seen) {
      // Counted, not written. The periodic report keeps a long flood visible
      // in the file without letting it fill the card.
      corelog_repeats++;
      snprintf(corelog_repeat_text, sizeof corelog_repeat_text, "%s", text);
      if (corelog_repeats % CORELOG_REPEAT_REPORT == 0)
        corelog_repeat_summary_locked();
      return;
    }
    if (corelog_repeats) {
      corelog_repeat_summary_locked();
      corelog_repeats = 0;
    }
    snprintf(corelog_recent[corelog_recent_next], CORELOG_LINE, "%s", text);
    corelog_recent_next = (corelog_recent_next + 1) % CORELOG_REPEAT_WINDOW;
  }

#ifndef NETHERSX2_VK_DIAGNOSTIC
  if (!important && corelog_chatter_stopped) {
    corelog_context_record_locked(priority, tag, text);
    return;
  }
#endif
  if (important && corelog_repeats) {
    corelog_repeat_summary_locked();
    corelog_repeats = 0;
  }
#ifndef NETHERSX2_VK_DIAGNOSTIC
  if (important && corelog_chatter_stopped)
    corelog_context_flush_locked();
#endif
  if (corelog_written >= CORELOG_HARD + (important ? CORELOG_TAIL : 0))
    return;

  struct timespec time;
  clock_gettime(CLOCK_MONOTONIC, &time);

  const char *level =
      (priority >= 0 && priority <= 8) ? corelog_priority[priority] : "?";
  const int has_tag = (tag && *tag);

  const int n = fprintf(corelog_file, "[%lld.%03lld] [%s] %s%s%s\n",
                        (long long)time.tv_sec,
                        (long long)(time.tv_nsec / 1000000),
                        level, has_tag ? tag : "", has_tag ? ": " : "", text);
  if (n > 0)
    corelog_written += n;

  // Errors, fatals and a full buffer must survive whatever happens next.
  if (important) {
    fflush(corelog_file);
    fsync(fileno(corelog_file));
  }

#ifndef NETHERSX2_VK_DIAGNOSTIC
  if (!important && !corelog_chatter_stopped && corelog_written >= CORELOG_LIMIT) {
    corelog_chatter_stopped = 1;
    const int notice = fprintf(corelog_file,
            "-- core log: %ld bytes written, INFO and below kept in a "
            "128 KiB recent-context buffer; wrapper errors and death notes "
            "are retained --\n",
            corelog_written);
    if (notice > 0)
      corelog_written += notice;
    fflush(corelog_file);
  }
#endif
}

/* Render an address as core+0xOFFSET when it falls inside libemucore.so, and
 * otherwise ask libc_shim.c which of the regions IT mapped the address belongs
 * to. Without the second half the death note is frequently uninterpretable: on
 * device abort()'s caller printed as a bare 0x41b1667c68, far outside the core
 * image, and there was no way to tell JIT'd recompiler code from a PS2 RAM
 * mirror from a value that was never a code address at all. */
void
corelog_describe_address(char *out, size_t size, const void *address) {
  const uintptr_t a = (uintptr_t)address;
  const uintptr_t base = (uintptr_t)emu_mod.load_base;
  char region[160];

  if (base && a >= base && a < base + emu_mod.load_size)
    snprintf(out, size, "core+0x%llx", (unsigned long long)(a - base));
  else if (shim_describe_address(a, region, sizeof(region)))
    snprintf(out, size, "0x%llx [%s]", (unsigned long long)a, region);
  else
    snprintf(out, size, "0x%llx (not in the core image or any mapped region)",
             (unsigned long long)a);
}

void
corelog_open(void) {
  mutexLock(&corelog_mutex);
  if (corelog_file) {
    fclose(corelog_file);
    corelog_file = NULL;
  }
  corelog_written = 0;
  memset(corelog_recent, 0, sizeof(corelog_recent));
  corelog_recent_next = 0;
#ifndef NETHERSX2_VK_DIAGNOSTIC
  corelog_context_start = 0;
  corelog_context_size = 0;
#endif
  corelog_repeats = 0;
  corelog_repeat_text[0] = '\0';
#ifndef NETHERSX2_VK_DIAGNOSTIC
  corelog_chatter_stopped = 0;
#endif
  corelog_abort_text[0] = '\0';

  mkdir(DATA_ROOT, 0777);
  corelog_file = fopen(CORELOG_PATH, "w");
  if (corelog_file) {
    /* Line-buffered, NOT fully buffered. main() ends in __libnx_exit(0), which
     * bypasses atexit, so a flush registered there never runs -- and the tail of
     * this log is exactly the part that explains the failure. The core writes
     * logcat at ANDROID_LOG_INFO (4), so waiting for an ERROR before flushing
     * loses everything. ERROR and FATAL are still fsynced below. */
    setvbuf(corelog_file, NULL, _IOLBF, 0);
    fprintf(corelog_file, "NetherSX2 core console\n");
    corelog_written = 24;
    fflush(corelog_file);
    fsync(fileno(corelog_file));
  }
  mutexUnlock(&corelog_mutex);
}

void
corelog_flush(void) {
  mutexLock(&corelog_mutex);
  if (corelog_file) {
    fflush(corelog_file);
    fsync(fileno(corelog_file));
  }
  mutexUnlock(&corelog_mutex);
}

void
corelog_close(void) {
  mutexLock(&corelog_mutex);
  if (corelog_file) {
    fflush(corelog_file);
    fsync(fileno(corelog_file));
    fclose(corelog_file);
    corelog_file = NULL;
  }
  mutexUnlock(&corelog_mutex);
}

void
corelog_vlog(int priority, const char *tag, const char *format, va_list args) {
  char line[CORELOG_LINE];
  if (!format)
    return;
  vsnprintf(line, sizeof(line), format, args);

  mutexLock(&corelog_mutex);
  corelog_emit_locked(priority, tag, line);
  mutexUnlock(&corelog_mutex);
}

void
corelog_log(int priority, const char *tag, const char *format, ...) {
  va_list args;
  va_start(args, format);
  corelog_vlog(priority, tag, format, args);
  va_end(args);
}

void
corelog_text(int priority, const char *tag, const char *text) {
  mutexLock(&corelog_mutex);
  corelog_emit_locked(priority, tag, text ? text : "(null)");
  mutexUnlock(&corelog_mutex);
}

void
corelog_abort_message(const char *message) {
  if (!message)
    return;
  mutexLock(&corelog_mutex);
  snprintf(corelog_abort_text, sizeof(corelog_abort_text), "%s", message);
  mutexUnlock(&corelog_mutex);
}

void
corelog_death(const char *reason, const void *called_from) {
  char where[256];  // room for a libc_shim region description
  char line[CORELOG_LINE];
  corelog_describe_address(where, sizeof(where), called_from);

  mutexLock(&corelog_mutex);
  snprintf(line, sizeof(line), "*** core terminated: %s, called from %s%s%s ***",
           reason ? reason : "(unknown reason)", where,
           corelog_abort_text[0] ? ", abort message: " : "",
           corelog_abort_text[0] ? corelog_abort_text : "");
  corelog_emit_locked(CORELOG_FATAL, "NetherSX2", line);
  mutexUnlock(&corelog_mutex);
}

static char *corelog_append_literal(char *out, const char *text) {
  while (*text)
    *out++ = *text++;
  return out;
}

static char *corelog_append_hex(char *out, uintptr_t value) {
  static const char digits[] = "0123456789abcdef";
  char reverse[sizeof(value) * 2];
  size_t count = 0;
  do {
    reverse[count++] = digits[value & 0xfu];
    value >>= 4;
  } while (value);
  while (count)
    *out++ = reverse[--count];
  return out;
}

static char *corelog_append_crash_address(char *out, uintptr_t address) {
  const uintptr_t base = (uintptr_t)emu_mod.load_base;
  if (base && address >= base && address - base < emu_mod.load_size) {
    out = corelog_append_literal(out, "core+0x");
    return corelog_append_hex(out, address - base);
  }
  out = corelog_append_literal(out, "0x");
  return corelog_append_hex(out, address);
}

void
corelog_crash_marker(unsigned long long pc, unsigned long long lr) {
  /* Keep this exception path independent of stdio, the corelog mutex and the
   * mmap-region table. If the fault involved the heap/stack, best-effort raw
   * addresses are safer than resolving a descriptive label before recording. */
  char line[160];
  char *out = line;
  out = corelog_append_literal(out, "[F] [NetherSX2]: *** CPU exception, pc=");
  out = corelog_append_crash_address(out, (uintptr_t)pc);
  out = corelog_append_literal(out, " lr=");
  out = corelog_append_crash_address(out, (uintptr_t)lr);
  out = corelog_append_literal(out, " ***\n");
  const size_t length = (size_t)(out - line);

  /* Raw and mutex-free: the crashing thread may itself be inside the logger. */
  const int fd = open(CORELOG_PATH, O_WRONLY | O_APPEND | O_CREAT, 0666);
  if (fd >= 0) {
    size_t written = 0;
    while (written < length) {
      const ssize_t n = write(fd, line + written, length - written);
      if (n <= 0)
        break;
      written += (size_t)n;
    }
    fsync(fd);
    close(fd);
  }
}
