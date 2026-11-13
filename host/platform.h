#ifndef MENHIR_PLATFORM_H
#define MENHIR_PLATFORM_H
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
extern int __ulock_wait(uint32_t, void *, uint64_t, uint32_t);
extern int __ulock_wake(uint32_t, void *, uint64_t);
#else
#include <linux/futex.h>
#include <sys/auxv.h>
#include <asm/hwcap.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <limits.h>
#endif
/* Reject hosts missing instructions advertised by the fixed guest ID registers.
   PAC is rewritten, while SVE/SME are neither advertised nor translated. */
static inline int hv_host_check(void) {
#if defined(__ANDROID__)
    unsigned long need = HWCAP_FP | HWCAP_ASIMD | HWCAP_AES | HWCAP_PMULL |
        HWCAP_SHA1 | HWCAP_SHA2 | HWCAP_SHA512 | HWCAP_SHA3 | HWCAP_CRC32 |
        HWCAP_ATOMICS | HWCAP_FPHP | HWCAP_ASIMDHP | HWCAP_ASIMDRDM |
        HWCAP_JSCVT | HWCAP_FCMA | HWCAP_LRCPC | HWCAP_ILRCPC |
        HWCAP_DCPOP | HWCAP_ASIMDDP | HWCAP_ASIMDFHM | HWCAP_FLAGM | HWCAP_SB;
    unsigned long missing = need & ~getauxval(AT_HWCAP);
    if (missing || !(getauxval(AT_HWCAP2) & HWCAP2_FRINT)) {
        fprintf(stderr, "hengevm: unsupported host CPU: missing HWCAP %#lx, FRINT %s\n",
                missing, getauxval(AT_HWCAP2) & HWCAP2_FRINT ? "present" : "absent");
        return -1;
    }
    long page = sysconf(_SC_PAGESIZE);
    if (page != 4096 && page != 16384) {
        fprintf(stderr, "hengevm: unsupported host page size %ld\n", page);
        return -1;
    }
#endif
    return 0;
}
/* Guest timer units are always 1/24,000,000 s, independent of CNTFRQ. */
static inline uint64_t hv_host_counter(void) {
#if defined(__APPLE__)
    return mach_absolute_time();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 24000000 + (uint64_t)ts.tv_nsec * 24 / 1000;
#endif
}
static inline void hv_host_wait(void *p, uint32_t value) {
#if defined(__APPLE__)
    __ulock_wait(1u | 0x1000000u, p, value, 100);
#else
    struct timespec ts = {0, 100000};
    syscall(SYS_futex, p, FUTEX_WAIT_PRIVATE, value, &ts, NULL, 0);
#endif
}
static inline void hv_host_wake(void *p) {
#if defined(__APPLE__)
    __ulock_wake(1u | 0x100u | 0x1000000u, p, 0);
#else
    syscall(SYS_futex, p, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
#endif
}
static inline int hv_cond_wait_relative(pthread_cond_t *cv, pthread_mutex_t *mu, const struct timespec *rel) {
#if defined(__APPLE__)
    return pthread_cond_timedwait_relative_np(cv, mu, rel);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += rel->tv_sec;
    ts.tv_nsec += rel->tv_nsec;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    return pthread_cond_timedwait(cv, mu, &ts);
#endif
}
#endif
