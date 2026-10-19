/* PROTOTYPE BIOHAZARD BUNDLE (CUSA06545): the PS4 APIs this game uses that The Order: 1886
 * never did - fibers, POSIX semaphores, PlayGo language/to-do lists, trophy icons,
 * local-time conversion, module listing and LoadExec (Prototype 1 starting Prototype 2). */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <ucontext.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#endif

#define FIBER_ERROR_NULL       ((int32_t)0x80590001)
#define FIBER_ERROR_ALIGNMENT  ((int32_t)0x80590002)
#define FIBER_ERROR_INVALID    ((int32_t)0x80590004)
#define FIBER_ERROR_PERMISSION ((int32_t)0x80590005)
#define FIBER_ERROR_STATE      ((int32_t)0x80590006)

/* ---------------------------------------------------------------- fibers ---
 * Each guest fiber runs on its own host stack (1 MiB), not on the context memory the game
 * hands over: runtime functions called from a fiber (file I/O, Windows API) need far more
 * than the 16 KiB the save fiber gets (that overflow crashed saving under shadPS4).
 * Windows uses native fibers so the TEB stack bounds stay correct, with the whole stack
 * committed (guest code has no stack probes and would skip a guard page); elsewhere ucontext. */
typedef void (ABI *FiberEntry)(uint64_t arg_on_initialize, uint64_t arg_on_run);
enum { FIBER_IDLE=0, FIBER_RUNNING=1, FIBER_STACK=1<<20 };
typedef struct Fiber {
    uint32_t magic;
    int state;              /* FIBER_IDLE only once its context is saved (finish_switch) */
    void *guest;            /* the guest OrbisFiber, whose state field follows ours */
    uint32_t last_thread;   /* host thread that last ran it, for the hang report */
    uint64_t last_switch_ns;
    struct Fiber *registered_next;
    FiberEntry entry;
    uint64_t arg_init;
    uint64_t transfer;      /* argument handed to this fiber when it is (re)entered */
    char name[32];
#ifdef _WIN32
    LPVOID host;
#else
    ucontext_t context;
    void *stack;
#endif
} Fiber;
#define FIBER_MAGIC 0x50524f54u
/* Guest OrbisFiber: magic_start u32 @0, state u32 @4, entry @8, arg @0x10, addr_context @0x18,
 * size_context @0x20, name[32] @0x28, context pointer @0x48 (ours), flags @0x50 ... */
typedef struct {
    uint32_t magic_start, state;
    uint64_t entry, arg_on_initialize, addr_context, size_context;
    char name[32];
    Fiber *fiber;
} GuestFiber;

/* Thread-locals are read through noinline accessors: a fiber can be resumed on another
 * thread, and a TLS address cached across a switch would be the old thread's. */
static _Thread_local Fiber *tls_current;
static _Thread_local Fiber *tls_pending_idle;
static _Thread_local uint64_t tls_thread_transfer;
#ifdef _WIN32
static _Thread_local LPVOID tls_thread_host;
#else
static _Thread_local ucontext_t tls_thread_context;
#endif
__attribute__((noinline)) static Fiber **current_slot(void) { Fiber **p=&tls_current; __asm__ volatile("":"+r"(p)); return p; }
__attribute__((noinline)) static Fiber **pending_idle_slot(void) { Fiber **p=&tls_pending_idle; __asm__ volatile("":"+r"(p)); return p; }
__attribute__((noinline)) static uint64_t *thread_transfer_slot(void) { uint64_t *p=&tls_thread_transfer; __asm__ volatile("":"+r"(p)); return p; }
#ifdef _WIN32
__attribute__((noinline)) static LPVOID *thread_host_slot(void) { LPVOID *p=&tls_thread_host; __asm__ volatile("":"+r"(p)); return p; }
#else
__attribute__((noinline)) static ucontext_t *thread_context_slot(void) { ucontext_t *p=&tls_thread_context; __asm__ volatile("":"+r"(p)); return p; }
#endif
static size_t fibers_created, fiber_switches, fiber_errors;
static pthread_mutex_t fiber_registry_lock=PTHREAD_MUTEX_INITIALIZER;
static Fiber *fiber_registry;
static uint64_t fiber_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec; }
static uint32_t fiber_thread_id(void) {
#ifdef _WIN32
    return (uint32_t)GetCurrentThreadId();
#else
    return (uint32_t)syscall(SYS_gettid);
#endif
}
static void set_state(Fiber *f,int state) {
    __atomic_store_n(&f->state,state,__ATOMIC_RELEASE);
    if (f->guest) ((GuestFiber *)f->guest)->state=(uint32_t)state;
}
/* The fiber that switched away is idle only now: its context is saved. Marking it idle before
 * the switch let another thread run it on the context of its previous switch (PS4 libSceFiber
 * saves the context first too). Every place a switch lands calls this. */
static void finish_switch(void) {
    Fiber *previous=*pending_idle_slot();
    if (!previous) return;
    *pending_idle_slot()=NULL;
    set_state(previous,FIBER_IDLE);
}
static int32_t fiber_error(const char *call,const Fiber *f,int32_t error) {
    if (__atomic_fetch_add(&fiber_errors,1,__ATOMIC_RELAXED)<64)
        fprintf(stderr,"Runtime: %s(%s) -> 0x%08x\n",call,f ? f->name : "?",(unsigned)error);
    return error;
}
/* Claims an idle fiber for running; fails while it still runs (or another thread claimed it). */
static int claim(Fiber *f) {
    int idle=FIBER_IDLE;
    return __atomic_compare_exchange_n(&f->state,&idle,FIBER_RUNNING,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
}
static void note_running(Fiber *f) {
    if (f->guest) ((GuestFiber *)f->guest)->state=FIBER_RUNNING;
    f->last_thread=fiber_thread_id();
    f->last_switch_ns=fiber_now_ns();
}

static __attribute__((noreturn)) void fiber_returned(Fiber *f) {
    fprintf(stderr,"STOP: guest fiber '%s' returned from its entry function\n",f->name);
    runtime_report();
    exit(24);
}
#ifdef _WIN32
static VOID WINAPI fiber_main(LPVOID param) {
    Fiber *f=param;
    finish_switch();
    f->entry(f->arg_init,f->transfer);
    fiber_returned(f);
}
#else
static void fiber_main(unsigned lo,unsigned hi) {
    Fiber *f=(Fiber *)(((uintptr_t)hi<<32)|lo);
    finish_switch();
    f->entry(f->arg_init,f->transfer);
    fiber_returned(f);
}
#endif
static Fiber *fiber_of(void *guest) {
    GuestFiber *g=guest;
    if (!g) return NULL;
    Fiber *f=g->fiber;
    return f && f->magic==FIBER_MAGIC ? f : NULL;
}
static int fiber_host_create(Fiber *f) {
#ifdef _WIN32
    f->host=CreateFiberEx(FIBER_STACK,FIBER_STACK,FIBER_FLAG_FLOAT_SWITCH,fiber_main,f);
    return f->host ? 0 : -1;
#else
    f->stack=mmap(NULL,FIBER_STACK,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (f->stack==MAP_FAILED) { f->stack=NULL; return -1; }
    getcontext(&f->context);
    f->context.uc_stack.ss_sp=f->stack;
    f->context.uc_stack.ss_size=FIBER_STACK;
    f->context.uc_link=NULL;
    uintptr_t p=(uintptr_t)f;
    makecontext(&f->context,(void (*)(void))fiber_main,2,(unsigned)p,(unsigned)(p>>32));
    return 0;
#endif
}
static void fiber_host_destroy(Fiber *f) {
#ifdef _WIN32
    if (f->host) DeleteFiber(f->host);
#else
    if (f->stack) munmap(f->stack,FIBER_STACK);
#endif
}
static ABI int32_t fiber_initialize(void *guest,const char *name,FiberEntry entry,uint64_t arg,
                                    void *addr_context,uint64_t size_context,const void *opt,uint32_t build) {
    (void)opt; (void)build;
    if (!guest || !name || !entry) return FIBER_ERROR_NULL;
    if ((uintptr_t)guest&7 || (uintptr_t)addr_context&15) return FIBER_ERROR_ALIGNMENT;
    if ((!addr_context)!=(!size_context) || size_context&15) return FIBER_ERROR_INVALID;
    Fiber *f=calloc(1,sizeof(*f));
    if (!f) return FIBER_ERROR_INVALID;
    f->magic=FIBER_MAGIC; f->entry=entry; f->arg_init=arg;
    snprintf(f->name,sizeof f->name,"%s",name);
    if (fiber_host_create(f)) { free(f); fputs("STOP: cannot allocate a fiber stack\n",stderr); exit(24); }
    GuestFiber *g=guest;
    g->magic_start=0xdef1649c; g->state=FIBER_IDLE; g->entry=(uint64_t)(uintptr_t)entry;
    g->arg_on_initialize=arg; g->addr_context=(uint64_t)(uintptr_t)addr_context; g->size_context=size_context;
    memcpy(g->name,f->name,sizeof g->name);
    g->fiber=f;
    f->guest=g;
    pthread_mutex_lock(&fiber_registry_lock);
    f->registered_next=fiber_registry; fiber_registry=f;
    pthread_mutex_unlock(&fiber_registry_lock);
    ++fibers_created;
    printf("Runtime: fiber '%s' (guest context %llu bytes; host stack %u KiB)\n",f->name,
           (unsigned long long)size_context,FIBER_STACK/1024);
    return 0;
}
static ABI int32_t fiber_finalize(void *guest) {
    Fiber *f=fiber_of(guest);
    if (!guest) return FIBER_ERROR_NULL;
    if (!f) return FIBER_ERROR_INVALID;
    if (__atomic_load_n(&f->state,__ATOMIC_ACQUIRE)!=FIBER_IDLE) return fiber_error("sceFiberFinalize",f,FIBER_ERROR_STATE);
    pthread_mutex_lock(&fiber_registry_lock);
    for (Fiber **p=&fiber_registry;*p;p=&(*p)->registered_next) if (*p==f) { *p=f->registered_next; break; }
    pthread_mutex_unlock(&fiber_registry_lock);
    f->magic=0;
    fiber_host_destroy(f);
    ((GuestFiber *)guest)->fiber=NULL;
    free(f);
    return 0;
}
/* From a plain thread into fiber f; returns when some fiber calls ReturnToThread. */
static ABI int32_t fiber_run(void *guest,uint64_t arg,uint64_t *arg_on_return) {
    Fiber *f=fiber_of(guest);
    if (!guest) return FIBER_ERROR_NULL;
    if (!f) return FIBER_ERROR_INVALID;
    if (*current_slot()) return fiber_error("sceFiberRun",f,FIBER_ERROR_PERMISSION);
    if (!claim(f)) return fiber_error("sceFiberRun",f,FIBER_ERROR_STATE);
    note_running(f);
    f->transfer=arg;
    *current_slot()=f;
    ++fiber_switches;
#ifdef _WIN32
    if (!IsThreadAFiber()) *thread_host_slot()=ConvertThreadToFiberEx(NULL,FIBER_FLAG_FLOAT_SWITCH);
    else if (!*thread_host_slot()) *thread_host_slot()=GetCurrentFiber();
    SwitchToFiber(f->host);
#else
    swapcontext(thread_context_slot(),&f->context);
#endif
    finish_switch();
    if (arg_on_return) *arg_on_return=*thread_transfer_slot();
    return 0;
}
/* From the running fiber to another idle fiber. */
static ABI int32_t fiber_switch(void *guest,uint64_t arg,uint64_t *arg_on_run) {
    Fiber *f=fiber_of(guest);
    if (!guest) return FIBER_ERROR_NULL;
    if (!f) return FIBER_ERROR_INVALID;
    Fiber *self=*current_slot();
    if (!self) return fiber_error("sceFiberSwitch",f,FIBER_ERROR_PERMISSION);
    if (!claim(f)) return fiber_error("sceFiberSwitch",f,FIBER_ERROR_STATE);
    note_running(f);
    f->transfer=arg;
    *current_slot()=f;
    *pending_idle_slot()=self;
    ++fiber_switches;
#ifdef _WIN32
    SwitchToFiber(f->host);
#else
    swapcontext(&self->context,&f->context);
#endif
    finish_switch();
    if (arg_on_run) *arg_on_run=self->transfer;
    return 0;
}
static ABI int32_t fiber_return_to_thread(uint64_t arg_on_return,uint64_t *arg_on_run) {
    Fiber *self=*current_slot();
    if (!self) return fiber_error("sceFiberReturnToThread",NULL,FIBER_ERROR_PERMISSION);
    *current_slot()=NULL;
    *pending_idle_slot()=self;
    *thread_transfer_slot()=arg_on_return;
#ifdef _WIN32
    SwitchToFiber(*thread_host_slot());
#else
    swapcontext(&self->context,thread_context_slot());
#endif
    finish_switch();
    if (arg_on_run) *arg_on_run=self->transfer;
    return 0;
}
static ABI int32_t fiber_get_self(void **out) {
    if (!out) return FIBER_ERROR_NULL;
    Fiber *self=*current_slot();
    if (!self) return FIBER_ERROR_PERMISSION;
    *out=NULL; /* guest struct address is not tracked back; no caller in this game */
    return 0;
}
static ABI int32_t fiber_opt_init(uint32_t *opt) { if (!opt) return FIBER_ERROR_NULL; *opt=0xbb40e64d; return 0; }

/* -------------------------------------------------------- POSIX semaphores ---
 * The guest sem_t is a pointer-sized handle; it holds a host object. */
typedef struct { pthread_mutex_t lock; pthread_cond_t cond; uint32_t count; } HostSem;
static int32_t sem_fail(int e) { *runtime_errno()=runtime_guest_errno(e); return -1; }
static ABI int32_t posix_sem_init(HostSem **sem,int32_t pshared,uint32_t value) {
    (void)pshared;
    if (!sem) return sem_fail(EINVAL);
    HostSem *s=calloc(1,sizeof(*s));
    if (!s) return sem_fail(ENOMEM);
    pthread_mutex_init(&s->lock,NULL); pthread_cond_init(&s->cond,NULL);
    s->count=value; *sem=s;
    return 0;
}
static ABI int32_t posix_sem_destroy(HostSem **sem) {
    if (!sem || !*sem) return sem_fail(EINVAL);
    HostSem *s=*sem; *sem=NULL;
    pthread_cond_destroy(&s->cond); pthread_mutex_destroy(&s->lock); free(s);
    return 0;
}
static ABI int32_t posix_sem_post(HostSem **sem) {
    if (!sem || !*sem) return sem_fail(EINVAL);
    HostSem *s=*sem;
    pthread_mutex_lock(&s->lock); ++s->count; pthread_cond_signal(&s->cond); pthread_mutex_unlock(&s->lock);
    return 0;
}
static ABI int32_t posix_sem_wait(HostSem **sem) {
    if (!sem || !*sem) return sem_fail(EINVAL);
    HostSem *s=*sem;
    pthread_mutex_lock(&s->lock);
    while (!s->count) pthread_cond_wait(&s->cond,&s->lock);
    --s->count;
    pthread_mutex_unlock(&s->lock);
    return 0;
}
static ABI int32_t posix_sem_trywait(HostSem **sem) {
    if (!sem || !*sem) return sem_fail(EINVAL);
    HostSem *s=*sem; int ok;
    pthread_mutex_lock(&s->lock); ok=s->count>0; if (ok) --s->count; pthread_mutex_unlock(&s->lock);
    return ok ? 0 : sem_fail(EAGAIN);
}
/* abstime: guest struct timespec {int64 sec; int64 nsec}, CLOCK_REALTIME. */
static ABI int32_t posix_sem_timedwait(HostSem **sem,const int64_t *abstime) {
    if (!sem || !*sem || !abstime || abstime[1]<0 || abstime[1]>=1000000000) return sem_fail(EINVAL);
    HostSem *s=*sem;
    struct timespec end={.tv_sec=(time_t)abstime[0],.tv_nsec=(long)abstime[1]};
    int e=0;
    pthread_mutex_lock(&s->lock);
    while (!s->count && e!=ETIMEDOUT) e=pthread_cond_timedwait(&s->cond,&s->lock,&end);
    int ok=s->count>0;
    if (ok) --s->count;
    pthread_mutex_unlock(&s->lock);
    return ok ? 0 : sem_fail(ETIMEDOUT);
}
static ABI int32_t posix_sem_getvalue(HostSem **sem,int32_t *value) {
    if (!sem || !*sem || !value) return sem_fail(EINVAL);
    pthread_mutex_lock(&(*sem)->lock); *value=(int32_t)(*sem)->count; pthread_mutex_unlock(&(*sem)->lock);
    return 0;
}

/* ------------------------------------------------------------------ PlayGo --- */
static ABI int32_t playgo_set_language_mask(uint32_t handle,uint64_t mask) { (void)handle; (void)mask; return 0; }
/* Everything is installed: nothing left to do. */
static ABI int32_t playgo_get_todo_list(uint32_t handle,void *list,uint32_t count,uint32_t *entries) {
    (void)handle; (void)list; (void)count;
    if (!entries) return (int32_t)0x80b20002; /* SCE_PLAYGO_ERROR_BAD_POINTER */
    *entries=0;
    return 0;
}

/* -------------------------------------------------------------- trophies --- */
/* No icon files: the game shows its trophy list without pictures. */
static ABI int32_t trophy_icon(int32_t context,int32_t handle,void *buffer,uint64_t *size) {
    (void)context; (void)handle; (void)buffer; if (size) *size=0;
    return (int32_t)0x80551608;
}

/* ------------------------------------------------------------- local time --- */
static int64_t local_offset(int64_t utc,int *dst) {
    time_t t=(time_t)utc; struct tm lt, gt;
#ifdef _WIN32
    localtime_s(&lt,&t); gmtime_s(&gt,&t);
#else
    localtime_r(&t,&lt); gmtime_r(&t,&gt);
#endif
    if (dst) *dst=lt.tm_isdst>0;
    lt.tm_isdst=0; gt.tm_isdst=0;
    return (int64_t)difftime(mktime(&lt),mktime(&gt));
}
typedef struct { int64_t t; uint32_t west_sec, dst_sec; } GuestTimesec;
typedef struct { int32_t minuteswest, dsttime; } GuestTimezone;
static ABI int32_t utc_to_local(int64_t utc,int64_t *local,GuestTimesec *st,uint64_t *dst_seconds) {
    int dst; int64_t off=local_offset(utc,&dst);
    if (local) *local=utc+off;
    if (st) { st->t=utc; st->west_sec=(uint32_t)(off-(dst?3600:0)); st->dst_sec=dst?3600:0; }
    if (dst_seconds) *dst_seconds=dst ? 3600 : 0;
    return 0;
}
static ABI int32_t local_to_utc(int64_t local,int64_t unused,int64_t *utc,GuestTimezone *tz,int32_t *dst_seconds) {
    (void)unused;
    int dst; int64_t off=local_offset(local,&dst);
    if (utc) *utc=local-off;
    if (tz) { tz->minuteswest=(int32_t)(-(off-(dst?3600:0))/60); tz->dsttime=0; }
    if (dst_seconds) *dst_seconds=dst ? 3600 : 0;
    return 0;
}

/* ---------------------------------------------------------------- modules ---
 * One module (the game); handle 0. Enough for the game's own crash-report setup. */
static ABI int32_t module_list(int32_t *handles,uint64_t count,uint64_t *actual) {
    if (!handles || !actual) return (int32_t)0x8002000e;
    if (count<1) return (int32_t)0x80020016;
    handles[0]=0; *actual=1;
    return 0;
}
static ABI int32_t module_info(int32_t handle,void *info) {
    if (!info) return (int32_t)0x8002000e;
    if (handle!=0) return (int32_t)0x80020003;
    uint64_t size; memcpy(&size,info,8);
    if (size<8+256) return (int32_t)0x80020016;
    memset((char *)info+8,0,size-8);
    strcpy((char *)info+8,"eboot.bin");
    return 0;
}
static ABI int32_t module_info_for_unwind(uint64_t address,int32_t flags,void *info) {
    (void)address; (void)flags; (void)info;
    return (int32_t)0x80020003; /* no unwind data: the game throws no C++ exceptions */
}

/* --------------------------------------------------------------- LoadExec ---
 * The bundle menu (Prototype 1's eboot) starts Prototype 2 with LoadExec. The second game is
 * a separate executable with its own prepared image: run.py starts it (exit code 42 plus the
 * requested path in out/loadexec.txt). */
static ABI int32_t load_exec(const char *path,char *const *argv) {
    (void)argv;
    printf("Runtime: sceSystemServiceLoadExec(\"%s\")\n",path ? path : "(null)");
    FILE *f=fopen("out/loadexec.txt","w");
    if (f) { fputs(path ? path : "",f); fclose(f); }
    fflush(NULL);
    _Exit(42);
}

static const RuntimeExport exports[]={
    {"_sceFiberInitializeImpl",fiber_initialize}, {"sceFiberFinalize",fiber_finalize},
    {"sceFiberRun",fiber_run}, {"sceFiberSwitch",fiber_switch},
    {"sceFiberReturnToThread",fiber_return_to_thread}, {"sceFiberGetSelf",fiber_get_self},
    {"sceFiberOptParamInitialize",fiber_opt_init},
    {"sem_init",posix_sem_init}, {"sem_destroy",posix_sem_destroy}, {"sem_post",posix_sem_post},
    {"sem_wait",posix_sem_wait}, {"sem_trywait",posix_sem_trywait}, {"sem_timedwait",posix_sem_timedwait},
    {"sem_getvalue",posix_sem_getvalue},
    {"scePlayGoSetLanguageMask",playgo_set_language_mask}, {"scePlayGoGetToDoList",playgo_get_todo_list},
    {"sceNpTrophyGetGameIcon",trophy_icon}, {"sceNpTrophyGetTrophyIcon",trophy_icon},
    {"sceKernelConvertUtcToLocaltime",utc_to_local}, {"sceKernelConvertLocaltimeToUtc",local_to_utc},
    {"sceKernelGetModuleList",module_list}, {"sceKernelGetModuleInfo",module_info},
    {"sceKernelGetModuleInfoForUnwind",module_info_for_unwind},
    {"sceSystemServiceLoadExec",load_exec},
};
uintptr_t runtime_proto_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_proto_report(void) {
    printf("Runtime: fibers created=%zu, fiber switches=%zu, fiber call errors=%zu\n",fibers_created,
           fiber_switches,fiber_errors);
}
/* The F10 hang report: every live fiber, whether it runs and since when. */
void runtime_fiber_report_state(void) {
    const uint64_t now=fiber_now_ns();
    pthread_mutex_lock(&fiber_registry_lock);
    for (Fiber *f=fiber_registry;f;f=f->registered_next)
        fprintf(stderr,"  fiber %p '%s': %s, last run on thread %u %.1f s ago\n",(void *)f,f->name,
                __atomic_load_n(&f->state,__ATOMIC_ACQUIRE)==FIBER_RUNNING ? "running" : "idle",
                f->last_thread,f->last_switch_ns ? (double)(now-f->last_switch_ns)/1e9 : -1.0);
    pthread_mutex_unlock(&fiber_registry_lock);
    fprintf(stderr,"  fibers created=%zu, switches=%zu, call errors=%zu\n",fibers_created,fiber_switches,fiber_errors);
}
