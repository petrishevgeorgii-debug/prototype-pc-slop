/* libkernel/libScePosix process services: clocks, sleeping, errno mapping,
 * pthread once/keys, signal bookkeeping and a narrow sysctl. Guest values
 * use FreeBSD numbering; host errno values never reach the guest directly. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#ifndef _WIN32
#include <sys/random.h>
#include <sys/resource.h>
#endif
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define PAGE 16384

typedef struct { int64_t sec, nsec; } GuestTimespec;
typedef struct { int64_t sec, usec; } GuestTimeval;

int32_t runtime_guest_errno(int e) {
    switch (e) {
    case 0: return 0;
    case EPERM: return 1; case ENOENT: return 2; case ESRCH: return 3; case EINTR: return 4;
    case EIO: return 5; case ENXIO: return 6; case E2BIG: return 7; case ENOEXEC: return 8;
    case EBADF: return 9; case ECHILD: return 10; case EDEADLK: return 11; case ENOMEM: return 12;
    case EACCES: return 13; case EFAULT: return 14; case EBUSY: return 16; case EEXIST: return 17;
    case EXDEV: return 18; case ENODEV: return 19; case ENOTDIR: return 20; case EISDIR: return 21;
    case EINVAL: return 22; case ENFILE: return 23; case EMFILE: return 24; case ENOTTY: return 25;
    case EFBIG: return 27; case ENOSPC: return 28; case ESPIPE: return 29; case EROFS: return 30;
    case EMLINK: return 31; case EPIPE: return 32; case ERANGE: return 34; case EAGAIN: return 35;
    case ENAMETOOLONG: return 63; case ENOTEMPTY: return 66; case ETIMEDOUT: return 60;
    case ELOOP: return 62; case ENOSYS: return 78; case EOVERFLOW: return 84; case ECANCELED: return 85;
    default: return 5; /* EIO: unmapped host error */
    }
}
static int32_t fail_posix(int e) { *runtime_errno()=runtime_guest_errno(e); return -1; }

/* ---- clocks and sleeping ---- */
static struct timespec process_start;
__attribute__((constructor)) static void remember_start(void) { clock_gettime(CLOCK_MONOTONIC,&process_start); }
static int host_clock(uint32_t id,clockid_t *out) {
    switch (id) {
    case 0: case 9: case 10: *out=CLOCK_REALTIME; return 1;            /* REALTIME(_PRECISE/_FAST) */
    case 4: case 11: case 12: case 5: case 7: case 8: *out=CLOCK_MONOTONIC; return 1; /* MONOTONIC/UPTIME */
    case 13: *out=CLOCK_REALTIME_COARSE; return 1;                    /* SECOND */
    case 14: *out=CLOCK_THREAD_CPUTIME_ID; return 1;
    case 2: case 15: *out=CLOCK_PROCESS_CPUTIME_ID; return 1;         /* PROF/PROCTIME */
    case 16: case 17: case 18: case 19: *out=CLOCK_MONOTONIC; return 1; /* PS4 network clocks */
    default: return 0;
    }
}
static int clock_read(uint32_t id,GuestTimespec *ts) {
    clockid_t host;
    if (!ts || !host_clock(id,&host)) return EINVAL;
    struct timespec t;
    if (clock_gettime(host,&t)) return errno;
    if (id==13) t.tv_nsec=0;
    ts->sec=t.tv_sec; ts->nsec=t.tv_nsec; return 0;
}
static ABI int32_t kernel_clock_gettime(uint32_t id,GuestTimespec *ts) {
    int e=clock_read(id,ts); return e ? ERR(runtime_guest_errno(e)) : 0;
}
static ABI int32_t posix_clock_gettime(uint32_t id,GuestTimespec *ts) {
    int e=clock_read(id,ts); return e ? fail_posix(e) : 0;
}
static ABI int32_t posix_clock_getres(uint32_t id,GuestTimespec *ts) {
    clockid_t host; struct timespec t;
    if (!host_clock(id,&host)) return fail_posix(EINVAL);
    if (clock_getres(host,&t)) return fail_posix(errno);
    if (ts) { ts->sec=t.tv_sec; ts->nsec=t.tv_nsec; }
    return 0;
}
static ABI uint64_t process_time(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)(t.tv_sec-process_start.tv_sec)*1000000+(uint64_t)((t.tv_nsec-process_start.tv_nsec)/1000);
}
static ABI uint64_t process_time_counter(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)(t.tv_sec-process_start.tv_sec)*1000000000+(uint64_t)(t.tv_nsec-process_start.tv_nsec);
}
static ABI uint64_t process_time_frequency(void) { return 1000000000; }
/* The guest's TSC is the process clock in nanoseconds. The raw rdtsc differs per core on some
 * hosts (3.9 s apart on a Xeon E5-2680 v4, #42), so a thread moving cores made the game jump. */
/* PS4 TSC rate: PROTOTYPE hardcodes 1,593,749,508 Hz for its frame timing instead of asking
 * sceKernelGetTscFrequency, so the counter must tick at exactly that rate. */
#define PS4_TSC_HZ UINT64_C(1593749508)
static ABI uint64_t read_tsc(void) {
    uint64_t ns=process_time_counter();
    return (uint64_t)(((unsigned __int128)ns*PS4_TSC_HZ)/1000000000u);
}
static ABI uint64_t tsc_frequency(void) { return PS4_TSC_HZ; }
/* Shared with the GPU library so flip/label timestamps use the guest's clock. */
uint64_t runtime_process_time_us(void) { return process_time(); }
uint64_t runtime_process_time_counter(void) { return process_time_counter(); }
uint64_t runtime_tsc_frequency(void) { return tsc_frequency(); }
uint64_t runtime_read_tsc(void) { return read_tsc(); }
static int sleep_ns(uint64_t ns) {
#ifdef _WIN32
    compat_sleep_ns(ns);
#else
    struct timespec t={.tv_sec=(time_t)(ns/1000000000),.tv_nsec=(long)(ns%1000000000)};
    while (nanosleep(&t,&t)) if (errno!=EINTR) return errno;
#endif
    return 0;
}
static ABI int32_t kernel_usleep(uint32_t usec) { sleep_ns((uint64_t)usec*1000); return 0; }
static ABI int32_t posix_usleep(uint32_t usec) { sleep_ns((uint64_t)usec*1000); return 0; }
static ABI uint32_t posix_sleep(uint32_t seconds) { sleep_ns((uint64_t)seconds*1000000000); return 0; }
static ABI int32_t posix_nanosleep(const GuestTimespec *rq,GuestTimespec *rem) {
    if (!rq || rq->nsec<0 || rq->nsec>=1000000000 || rq->sec<0) return fail_posix(EINVAL);
    sleep_ns((uint64_t)rq->sec*1000000000+(uint64_t)rq->nsec);
    if (rem) { rem->sec=0; rem->nsec=0; }
    return 0;
}
static ABI int32_t kernel_nanosleep(const GuestTimespec *rq,GuestTimespec *rem) {
    return posix_nanosleep(rq,rem) ? ERR(*runtime_errno()) : 0;
}
typedef struct { int32_t minuteswest, dsttime; } GuestTimezone;
static ABI int32_t kernel_gettimezone(GuestTimezone *tz) {
    if (!tz) return ERR(22);
    time_t now=time(NULL);
#ifdef _WIN32
    tz->minuteswest=(int32_t)(-compat_utc_offset(now)/60); tz->dsttime=0;
#else
    struct tm local; localtime_r(&now,&local);
    tz->minuteswest=(int32_t)(-local.tm_gmtoff/60); tz->dsttime=0;
#endif
    return 0;
}
static ABI int32_t posix_gettimeofday(GuestTimeval *tv,GuestTimezone *tz) {
    struct timeval t;
    gettimeofday(&t,NULL);
    if (tv) { tv->sec=t.tv_sec; tv->usec=t.tv_usec; }
    if (tz) kernel_gettimezone(tz);
    return 0;
}
static ABI int32_t kernel_gettimeofday(GuestTimeval *tv) {
    if (!tv) return ERR(22);
    return posix_gettimeofday(tv,NULL);
}
static ABI int64_t posix_time(int64_t *out) { int64_t t=(int64_t)time(NULL); if (out) *out=t; return t; }

/* ---- process ---- */
static ABI int32_t get_pagesize(void) { return PAGE; }
static ABI int32_t get_pid(void) { return 1000; }
static ABI int32_t yield(void) { sched_yield(); return 0; }
static ABI __attribute__((noreturn)) void hard_exit(int status) {
    printf("Runtime: guest requested _exit(%d)\n",status);
    runtime_report();
    fflush(stdout);
    _exit(status);
}
static ABI __attribute__((noreturn)) void raise_exception(uint32_t code,uint64_t argument) {
    fprintf(stderr,"STOP: guest raised debug exception code=0x%x argument=0x%llx\n",code,(unsigned long long)argument);
    runtime_report();
    exit(23);
}
static const char *guest_argv_storage[64] = {"eboot.bin", NULL};
static int guest_argc_storage = 1;
void runtime_set_guest_args(int argc, const char **argv) {
    if (argc > 0 && argv) {
        guest_argc_storage = argc < 63 ? argc : 63;
        for (int i = 0; i < guest_argc_storage; ++i) {
            guest_argv_storage[i] = argv[i];
        }
        guest_argv_storage[guest_argc_storage] = NULL;
    }
}
static ABI int32_t get_argc(void) { return guest_argc_storage; }
static ABI const char **get_argv(void) { return guest_argv_storage; }
/* Only the game and its bundled modules exist; nothing was loaded at run time. */
static ABI int32_t stop_unload_module(int32_t handle,uint64_t args,const void *argp,uint32_t flags,const void *opt,int32_t *result) {
    (void)args; (void)argp; (void)flags; (void)opt; (void)result;
    printf("Runtime: sceKernelStopUnloadModule(%d): no such module\n",handle);
    return ERR(3); /* ESRCH */
}
static ABI int32_t module_info_from_addr(uint64_t address,int32_t flags,void *info) {
    (void)address;
    if (flags>=3) return ERR(22);
    if (!info) return ERR(14);
    return ERR(3); /* ESRCH: the runtime keeps no module records */
}
static ABI int32_t kernel_mlock(const void *address,uint64_t size) { (void)address; (void)size; return 0; }
/* libc's C++ exception unwinder. The game does not throw (no __cxa_throw import): unwinding
 * finds no frame data, and resuming a context is a stop, never a silent success. */
static ABI int32_t elf_phdr_match_addr(void *info,uint64_t address) { (void)info; (void)address; return 0; }
static ABI int32_t is_signal_return(uint64_t pc) { (void)pc; return 0; }
static ABI void pthread_cxa_finalize(void *dso) { (void)dso; }
static ABI __attribute__((noreturn)) void unwind_resume(void) {
    fputs("STOP: guest C++ exception unwinding (sigreturn/__Ux86_64_setcontext) is not supported\n",stderr);
    runtime_report();
    exit(23);
}
static ABI int32_t print_backtrace(void) {
    fputs("Runtime: guest requested a backtrace (not available)\n",stderr);
    return 0;
}
typedef struct { uint32_t bits[4]; } GuestSigset;
static _Thread_local GuestSigset signal_mask;
static uintptr_t handlers[128];
static ABI uintptr_t guest_signal(int sig,uintptr_t handler) {
    if (sig<=0 || sig>=128) { *runtime_errno()=22; return (uintptr_t)-1; } /* SIG_ERR */
    uintptr_t old=handlers[sig]; handlers[sig]=handler;
    printf("Runtime: guest signal(%d) handler recorded (host signals are not forwarded)\n",sig);
    return old;
}
static ABI int32_t guest_sigprocmask(int how,const GuestSigset *set,GuestSigset *old) {
    if (old) *old=signal_mask;
    if (!set) return 0;
    for (int i=0;i<4;++i) {
        if (how==1) signal_mask.bits[i]|=set->bits[i];        /* SIG_BLOCK */
        else if (how==2) signal_mask.bits[i]&=~set->bits[i];  /* SIG_UNBLOCK */
        else if (how==3) signal_mask.bits[i]=set->bits[i];    /* SIG_SETMASK */
        else return fail_posix(EINVAL);
    }
    return 0;
}
static ABI int32_t guest_sigfillset(GuestSigset *set) { if (!set) return fail_posix(EINVAL); memset(set,0xff,sizeof(*set)); return 0; }
static ABI int32_t guest_sigemptyset(GuestSigset *set) { if (!set) return fail_posix(EINVAL); memset(set,0,sizeof(*set)); return 0; }
typedef struct { GuestTimeval utime, stime; int64_t rest[14]; } GuestRusage;
static ABI int32_t guest_getrusage(int who,GuestRusage *out) {
    if (!out || (who!=0 && who!=1)) return fail_posix(EINVAL);
    memset(out,0,sizeof(*out));
#ifdef _WIN32
    int64_t user=0,kernel=0;
    compat_cpu_times(who,&user,&kernel);
    out->utime=(GuestTimeval){user/1000000,user%1000000};
    out->stime=(GuestTimeval){kernel/1000000,kernel%1000000};
#else
    struct rusage r;
    getrusage(who==0 ? RUSAGE_SELF : RUSAGE_THREAD,&r);
    out->utime=(GuestTimeval){r.ru_utime.tv_sec,r.ru_utime.tv_usec};
    out->stime=(GuestTimeval){r.ru_stime.tv_sec,r.ru_stime.tv_usec};
#endif
    return 0;
}
static ABI int32_t guest_sysctl(const int32_t *name,uint32_t namelen,void *old,uint64_t *oldlen,const void *new_value,uint64_t newlen) {
    (void)newlen;
    if (!name || namelen<2 || new_value) return fail_posix(EINVAL);
    if (name[0]==1 && name[1]==37) { /* kern.arandom */
        if (!old || !oldlen) return fail_posix(EINVAL);
#ifdef _WIN32
        if (compat_random(old,(size_t)*oldlen)) return fail_posix(EIO);
#else
        if (getrandom(old,(size_t)*oldlen,0)<0) return fail_posix(errno);
#endif
        return 0;
    }
    if (name[0]==6 && (name[1]==7 || name[1]==3)) { /* hw.pagesize / hw.ncpu */
        if (!oldlen) return fail_posix(EINVAL);
        int32_t value=name[1]==7 ? PAGE : 7;
        if (old) { if (*oldlen<4) return fail_posix(ENOMEM); memcpy(old,&value,4); }
        *oldlen=4; return 0;
    }
    fprintf(stderr,"STOP: unsupported sysctl mib");
    for (uint32_t i=0;i<namelen && i<8;++i) fprintf(stderr," %d",name[i]);
    fputc('\n',stderr);
    exit(21);
}

/* ---- pthread_once and thread-specific data ---- */
static ABI int32_t thread_once(int32_t *once,void (ABI *routine)(void)) {
    if (!once || !routine) return ERR(22);
    for (;;) {
        int32_t state=__atomic_load_n(once,__ATOMIC_ACQUIRE);
        if (state==1) return 0;
        if (state==0 && __atomic_compare_exchange_n(once,&state,2,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
            routine();
            __atomic_store_n(once,1,__ATOMIC_RELEASE);
            return 0;
        }
        sched_yield();
    }
}
static ABI int32_t posix_once(int32_t *once,void (ABI *routine)(void)) { return thread_once(once,routine) ? 22 : 0; }
#define KEYS 256
typedef void (ABI *KeyDestructor)(void *);
static struct { int used, is_mt_rng; KeyDestructor destructor; void *caller; } keys[KEYS];
static pthread_mutex_t key_lock=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local void *key_values[KEYS];
static _Thread_local uint32_t *lazy_mt_rng;
/* THEORDER_KEY_TRACE=1: key creations and stores with their callers and threads (first 400);
 * the thread is the system's id, as in fault reports. */
#ifdef _WIN32
__declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
#define TRACE_TID() ((unsigned long)GetCurrentThreadId())
#else
#define TRACE_TID() ((unsigned long)gettid())
#endif
static int key_trace(void) {
    static int trace=-1, events;
    if (trace<0) trace=getenv("THEORDER_KEY_TRACE")!=NULL;
    return trace && __atomic_fetch_add(&events,1,__ATOMIC_RELAXED)<400;
}
static int is_mt_rng_caller(void *caller) {
    uintptr_t low=(uintptr_t)caller & 0xfffu;
    return low==0xfa3u || low==0xf82u; /* sub_89fa60 -> 0x89ff9e / 0x89ff7d ([0x16a2640]) */
}
static int is_singleton_tls_caller(void *caller) {
    uintptr_t low=(uintptr_t)caller & 0xfffu;
    return is_mt_rng_caller(caller) || low==0xbedu || low==0xc0eu || low==0xdb3u || low==0xdd4u;
}
static int32_t key_create_by(uint32_t *key,KeyDestructor destructor,void *caller) {
    if (!key) return ERR(22);
    pthread_mutex_lock(&key_lock);
    if (is_singleton_tls_caller(caller)) {
        for (uint32_t i=1;i<KEYS;++i) if (keys[i].used && keys[i].caller==caller) {
            pthread_mutex_unlock(&key_lock);
            *key=i;
            if (key_trace()) printf("Key %u reused for singleton %p on thread %lu\n",i,caller,TRACE_TID());
            return 0;
        }
    }
    for (uint32_t i=1;i<KEYS;++i) if (!keys[i].used) {
        keys[i].used=1; keys[i].is_mt_rng=is_mt_rng_caller(caller);
        keys[i].destructor=destructor; keys[i].caller=caller;
        pthread_mutex_unlock(&key_lock);
        *key=i;
        if (key_trace()) printf("Key %u created by %p on thread %lu\n",i,caller,TRACE_TID());
        return 0;
    }
    pthread_mutex_unlock(&key_lock);
    return ERR(35);
}
static ABI int32_t key_create(uint32_t *key,KeyDestructor destructor) { return key_create_by(key,destructor,__builtin_return_address(0)); }
static ABI int32_t key_delete(uint32_t key) {
    if (!key || key>=KEYS) return ERR(22);
    pthread_mutex_lock(&key_lock);
    int32_t r=keys[key].used ? 0 : ERR(22);
    keys[key].used=0; keys[key].is_mt_rng=0; keys[key].destructor=NULL; keys[key].caller=NULL;
    pthread_mutex_unlock(&key_lock);
    return r;
}
static int32_t key_set_by(uint32_t key,void *value,void *caller) {
    if (!key || key>=KEYS || !keys[key].used) return ERR(22);
    if (key_trace()) printf("Key %u = %p by %p on thread %lu\n",key,value,caller,TRACE_TID());
    key_values[key]=value; return 0;
}
static ABI int32_t key_set(uint32_t key,void *value) { return key_set_by(key,value,__builtin_return_address(0)); }
static ABI void *key_get(uint32_t key) {
    if (!key || key>=KEYS) return NULL;
    void *v=key_values[key];
    if (v) return v;
    if (keys[key].used && keys[key].is_mt_rng) {
        void *caller=__builtin_return_address(0);
        if (((uintptr_t)caller & 0xfffu)==0x79du) return NULL; /* sub_8a0330 teardown free check */
        if (!lazy_mt_rng) {
            /* sub_814870 layout: 624 state words (+0), 624 tempered words (+0x9c0), index (+0x1380 = 0x270). */
            lazy_mt_rng=calloc(1,0x1384);
            if (lazy_mt_rng) {
                uint32_t s=(uint32_t)TRACE_TID() ^ 0xf7a0b5ecu;
                lazy_mt_rng[0]=s;
                for (uint32_t i=1;i<624;++i) {
                    s=1812433253u*(s^(s>>30))+i;
                    lazy_mt_rng[i]=s;
                }
                lazy_mt_rng[0x1380/4]=0x270;
                fprintf(stderr,"Runtime: lazy MT19937 TLS initialized for key %u at %p (thread %lu)\n",key,caller,TRACE_TID());
            }
        }
        return lazy_mt_rng;
    }
    return NULL;
}
void runtime_thread_keys_cleanup(void) {
    for (int round=0;round<4;++round) {
        int any=0;
        for (uint32_t i=1;i<KEYS;++i) {
            void *value=key_values[i];
            if (!value || !keys[i].used || !keys[i].destructor) continue;
            key_values[i]=NULL; any=1;
            keys[i].destructor(value);
        }
        if (!any) break;
    }
    if (lazy_mt_rng) { free(lazy_mt_rng); lazy_mt_rng=NULL; }
}
static ABI int32_t posix_key_create(uint32_t *k,KeyDestructor d) { int32_t r=key_create_by(k,d,__builtin_return_address(0)); return r ? r&0xffff : 0; }
static ABI int32_t posix_key_delete(uint32_t k) { int32_t r=key_delete(k); return r ? r&0xffff : 0; }
static ABI int32_t posix_key_set(uint32_t k,void *v) { int32_t r=key_set_by(k,v,__builtin_return_address(0)); return r ? r&0xffff : 0; }

#define MAX_EVENT_FLAGS 256
typedef struct {
    int used;
    uint32_t attr;
    uint64_t pattern;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} EventFlag;

static EventFlag event_flags[MAX_EVENT_FLAGS];
static pthread_mutex_t ef_table_lock = PTHREAD_MUTEX_INITIALIZER;

static ABI int32_t create_event_flag(uint64_t *pEventFlag, const char *name, uint32_t attr, uint64_t init_pattern, const void *opt) {
    (void)name; (void)opt;
    if (!pEventFlag) return ERR(22);
    pthread_mutex_lock(&ef_table_lock);
    for (uint32_t i = 1; i < MAX_EVENT_FLAGS; ++i) {
        if (!event_flags[i].used) {
            EventFlag *ef = &event_flags[i];
            ef->used = 1;
            ef->attr = attr;
            ef->pattern = init_pattern;
            pthread_mutex_init(&ef->mutex, NULL);
            pthread_cond_init(&ef->cond, NULL);
            pthread_mutex_unlock(&ef_table_lock);
            *pEventFlag = (uint64_t)i;
            return 0;
        }
    }
    pthread_mutex_unlock(&ef_table_lock);
    return ERR(35);
}

static ABI int32_t delete_event_flag(uint64_t id) {
    if (!id || id >= MAX_EVENT_FLAGS) return ERR(22);
    pthread_mutex_lock(&ef_table_lock);
    EventFlag *ef = &event_flags[id];
    if (!ef->used) {
        pthread_mutex_unlock(&ef_table_lock);
        return ERR(22);
    }
    pthread_mutex_lock(&ef->mutex);
    ef->used = 0;
    pthread_cond_broadcast(&ef->cond);
    pthread_mutex_unlock(&ef->mutex);
    pthread_cond_destroy(&ef->cond);
    pthread_mutex_destroy(&ef->mutex);
    pthread_mutex_unlock(&ef_table_lock);
    return 0;
}

static ABI int32_t set_event_flag(uint64_t id, uint64_t bit_pattern) {
    if (!id || id >= MAX_EVENT_FLAGS) return ERR(22);
    EventFlag *ef = &event_flags[id];
    if (!ef->used) return ERR(22);
    pthread_mutex_lock(&ef->mutex);
    ef->pattern |= bit_pattern;
    pthread_cond_broadcast(&ef->cond);
    pthread_mutex_unlock(&ef->mutex);
    return 0;
}

static ABI int32_t clear_event_flag(uint64_t id, uint64_t bit_pattern) {
    if (!id || id >= MAX_EVENT_FLAGS) return ERR(22);
    EventFlag *ef = &event_flags[id];
    if (!ef->used) return ERR(22);
    pthread_mutex_lock(&ef->mutex);
    ef->pattern &= bit_pattern;
    pthread_mutex_unlock(&ef->mutex);
    return 0;
}

static ABI int32_t wait_event_flag(uint64_t id, uint64_t bit_pattern, uint32_t wait_mode, uint64_t *result_pattern, const uint32_t *timeout_usec) {
    if (!id || id >= MAX_EVENT_FLAGS) return ERR(22);
    EventFlag *ef = &event_flags[id];
    if (!ef->used) return ERR(22);

    struct timespec ts;
    if (timeout_usec) {
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t nsec = (uint64_t)ts.tv_nsec + (uint64_t)(*timeout_usec) * 1000ull;
        ts.tv_sec += nsec / 1000000000ull;
        ts.tv_nsec = nsec % 1000000000ull;
    }

    pthread_mutex_lock(&ef->mutex);
    for (;;) {
        if (!ef->used) {
            pthread_mutex_unlock(&ef->mutex);
            return ERR(22);
        }
        int is_or = (wait_mode & 1);
        int matched = is_or ? ((ef->pattern & bit_pattern) != 0) : ((ef->pattern & bit_pattern) == bit_pattern);
        if (matched) {
            if (result_pattern) *result_pattern = ef->pattern;
            if (wait_mode & 0x02) ef->pattern = 0;
            else if (wait_mode & 0x10) ef->pattern &= ~bit_pattern;
            pthread_mutex_unlock(&ef->mutex);
            return 0;
        }
        if (timeout_usec) {
            int r = pthread_cond_timedwait(&ef->cond, &ef->mutex, &ts);
            if (r == ETIMEDOUT) {
                pthread_mutex_unlock(&ef->mutex);
                return ERR(60);
            }
        } else {
            pthread_cond_wait(&ef->cond, &ef->mutex);
        }
    }
}

static ABI int32_t poll_event_flag(uint64_t id, uint64_t bit_pattern, uint32_t wait_mode, uint64_t *result_pattern) {
    if (!id || id >= MAX_EVENT_FLAGS) return ERR(22);
    EventFlag *ef = &event_flags[id];
    if (!ef->used) return ERR(22);
    pthread_mutex_lock(&ef->mutex);
    int is_or = (wait_mode & 1);
    int matched = is_or ? ((ef->pattern & bit_pattern) != 0) : ((ef->pattern & bit_pattern) == bit_pattern);
    if (matched) {
        if (result_pattern) *result_pattern = ef->pattern;
        if (wait_mode & 0x02) ef->pattern = 0;
        else if (wait_mode & 0x10) ef->pattern &= ~bit_pattern;
        pthread_mutex_unlock(&ef->mutex);
        return 0;
    }
    pthread_mutex_unlock(&ef->mutex);
    return ERR(35);
}
static ABI int32_t kernel_chmod(const char *path, uint32_t mode) { (void)path; (void)mode; return 0; }
static ABI int32_t kernel_utimes(const char *path, const void *times) { (void)path; (void)times; return 0; }
static ABI int32_t kernel_getdents(int fd, char *buf, size_t nbytes) { (void)fd; (void)buf; (void)nbytes; return 0; }
static ABI int32_t pthread_get_sched_param(uint64_t thread, int32_t *policy, void *param) {
    (void)thread;
    if (policy) *policy = 0;
    if (param) memset(param, 0, 8);
    return 0;
}
static ABI int32_t pthread_set_sched_param(uint64_t thread, int32_t policy, const void *param) {
    (void)thread; (void)policy; (void)param;
    return 0;
}
static ABI int32_t order_stub_nlq(void) { return 0; }

static const RuntimeExport exports[]={
    {"sceKernelClockGettime",kernel_clock_gettime}, {"clock_gettime",posix_clock_gettime},
    {"clock_getres",posix_clock_getres},
    {"sceKernelGetProcessTime",process_time}, {"sceKernelGetProcessTimeCounter",process_time_counter},
    {"sceKernelGetProcessTimeCounterFrequency",process_time_frequency},
    {"sceKernelReadTsc",read_tsc}, {"sceKernelGetTscFrequency",tsc_frequency},
    {"sceKernelUsleep",kernel_usleep}, {"usleep",posix_usleep}, {"sleep",posix_sleep},
    {"nanosleep",posix_nanosleep}, {"sceKernelNanosleep",kernel_nanosleep},
    {"sceKernelGettimezone",kernel_gettimezone}, {"gettimeofday",posix_gettimeofday},
    {"sceKernelGettimeofday",kernel_gettimeofday},
    {"time",posix_time},
    {"getpagesize",get_pagesize}, {"getpid",get_pid}, {"sched_yield",yield},
    {"_exit",hard_exit},
    {"sceKernelDebugRaiseException",raise_exception},
    {"sceKernelDebugRaiseExceptionOnReleaseMode",raise_exception},
    {"sceKernelPrintBacktraceWithModuleInfo",print_backtrace},
    {"getargc",get_argc}, {"getargv",get_argv}, {"sceKernelStopUnloadModule",stop_unload_module},
    {"sceKernelGetModuleInfoFromAddr",module_info_from_addr}, {"sceKernelMlock",kernel_mlock},
    {"__elf_phdr_match_addr",elf_phdr_match_addr}, {"_is_signal_return",is_signal_return},
    {"__pthread_cxa_finalize",pthread_cxa_finalize}, {"sigreturn",unwind_resume},
    {"__Ux86_64_setcontext",unwind_resume},
    {"signal",guest_signal}, {"sigprocmask",guest_sigprocmask}, {"_sigprocmask",guest_sigprocmask},
    {"sigfillset",guest_sigfillset}, {"sigemptyset",guest_sigemptyset},
    {"getrusage",guest_getrusage}, {"sysctl",guest_sysctl},
    {"scePthreadOnce",thread_once}, {"pthread_once",posix_once},
    {"scePthreadKeyCreate",key_create}, {"scePthreadKeyDelete",key_delete},
    {"scePthreadSetspecific",key_set}, {"scePthreadGetspecific",key_get},
    {"pthread_key_create",posix_key_create}, {"pthread_key_delete",posix_key_delete},
    {"pthread_setspecific",posix_key_set}, {"pthread_getspecific",key_get},
    {"sceKernelCreateEventFlag",create_event_flag}, {"sceKernelDeleteEventFlag",delete_event_flag},
    {"sceKernelSetEventFlag",set_event_flag}, {"sceKernelClearEventFlag",clear_event_flag},
    {"sceKernelWaitEventFlag",wait_event_flag}, {"sceKernelPollEventFlag",poll_event_flag},
    {"sceKernelChmod",kernel_chmod}, {"sceKernelUtimes",kernel_utimes}, {"sceKernelGetdents",kernel_getdents},
    {"scePthreadGetschedparam",pthread_get_sched_param}, {"scePthreadSetschedparam",pthread_set_sched_param},
    {"order_stub_NLq2b1jOaN0",order_stub_nlq},
};
uintptr_t runtime_kernel_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
