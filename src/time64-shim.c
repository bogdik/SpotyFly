/*
 * libspotiya-time-shim.so — LD_PRELOAD for Soloist (bundled glibc 2.38, armhf, _TIME_BITS=64).
 *
 * The Station kernel is 4.9 and runs Soloist as a 32-bit compat process without a
 * compat vDSO.  glibc's time64 entry points therefore try clock_gettime64 (syscall
 * 403, added in Linux 5.1) on EVERY call, get ENOSYS and only then fall back to the
 * 32-bit clock_gettime (263).  Soloist reads the clock thousands of times a second,
 * so each read costs an extra trap plus a rate-limited "syscall 403" kernel warning
 * (~2.6k/s, CPU pinned at max frequency).  These wrappers go straight to syscall 263.
 * 32-bit seconds are valid until 2038, same as the fallback glibc ends up using.
 *
 * Build (no libc link; __errno_location resolves from Soloist's libc at load time).
 * The version script is mandatory — see time64-shim.map:
 *   zig cc -target arm-linux-gnueabihf -mcpu=cortex_a53 -marm \
 *     -O2 -fPIC -fno-stack-protector -fomit-frame-pointer -ffunction-sections \
 *     -fdata-sections -shared -nostdlib -s -Wl,--gc-sections \
 *     -Wl,--version-script=src/time64-shim.map \
 *     -Wl,-soname,libspotiya-time-shim.so -o libspotiya-time-shim.so \
 *     src/time64-shim.c
 * Check: readelf --dyn-syms must list only the three functions + __errno_location.
 * run.sh preloads it next to libspotiya-math-shim.so.
 */
#include <stdint.h>

#define NR_CLOCK_GETTIME32   263  /* __NR_clock_gettime, ARM EABI */
#define CLOCK_REALTIME_       0
#define CLOCK_REALTIME_COARSE_ 5  /* what glibc's time() uses */
#define EFAULT_              14

struct ts32 { int32_t tv_sec; int32_t tv_nsec; };
/* struct timespec / timeval as seen by a _TIME_BITS=64 armhf (little-endian) caller. */
struct ts64 { int64_t tv_sec; int32_t tv_nsec; int32_t pad; };
struct tv64 { int64_t tv_sec; int64_t tv_usec; };
struct tz   { int32_t minuteswest; int32_t dsttime; };

extern int *__errno_location(void);

static long gettime32(long clk, struct ts32 *ts)
{
    register long r0 __asm__("r0") = clk;
    register long r1 __asm__("r1") = (long)ts;
    register long r7 __asm__("r7") = NR_CLOCK_GETTIME32;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r7) : "memory");
    return r0;
}

static int fail(long err)
{
    *__errno_location() = (int)err;
    return -1;
}

int __clock_gettime64(int clk, struct ts64 *ts)
{
    struct ts32 t;
    long r;

    if (!ts)
        return fail(EFAULT_);
    r = gettime32(clk, &t);
    if (r < 0)
        return fail(-r);
    ts->tv_sec = t.tv_sec;
    ts->tv_nsec = t.tv_nsec;
    ts->pad = 0;
    return 0;
}

int __gettimeofday64(struct tv64 *tv, void *tzp)
{
    struct ts32 t;
    long r;

    if (tzp) {
        struct tz *z = tzp;
        z->minuteswest = 0;
        z->dsttime = 0;
    }
    if (!tv)
        return 0;
    r = gettime32(CLOCK_REALTIME_, &t);
    if (r < 0)
        return fail(-r);
    tv->tv_sec = t.tv_sec;
    tv->tv_usec = t.tv_nsec / 1000;
    return 0;
}

int64_t __time64(int64_t *out)
{
    struct ts32 t;
    int64_t s = gettime32(CLOCK_REALTIME_COARSE_, &t) < 0 ? -1 : t.tv_sec;

    if (out)
        *out = s;
    return s;
}
