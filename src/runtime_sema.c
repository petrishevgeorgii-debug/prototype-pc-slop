/* Kernel semaphores: 32-bit IDs, counted tokens and FIFO waiters.
 * Priority-ordered semaphores are approximated by FIFO order. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <inttypes.h>
#ifdef _WIN32
#include <windows.h>
static unsigned long host_tid(void) { return GetCurrentThreadId(); }
#else
#include <unistd.h>
#include <sys/syscall.h>
static unsigned long host_tid(void) { return (unsigned long)syscall(SYS_gettid); }
#endif
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
/* Each semaphore has its own lock; the registry (id -> semaphore) is a hash table under striped
 * locks held only for the lookup. One lock for everything made threads queue behind each other
 * on every signal and wait (PROTOTYPE creates and uses thousands per minute), which widened the
 * game's own timing windows (see the engine event below). Lock order: a registry stripe, then a
 * semaphore's lock; never the other way round. */
typedef struct Waiter {
    int32_t need, result;
    int done;
    unsigned long tid;  /* host thread, for the F10 hang report */
    uintptr_t caller;   /* return address of the wait call */
    pthread_cond_t event;
    struct Waiter *next;
} Waiter;
typedef struct Sema {
    uint32_t id, attr;
    int32_t count, initial, maximum;
    unsigned refs;      /* under the registry stripe: operations in progress (waits included) */
    int deleted;        /* under the registry stripe */
    pthread_mutex_t m;  /* count and waiters */
    Waiter *first;
    struct Sema *next;  /* bucket chain */
} Sema;
uintptr_t runtime_guest_image;
enum { BUCKETS=4096, STRIPES=64 };
static Sema *buckets[BUCKETS];
static pthread_mutex_t stripes[STRIPES]={[0 ... STRIPES-1]=PTHREAD_MUTEX_INITIALIZER};
static uint32_t next_id=1;
static size_t created, deleted, acquired, signaled, timed_out; /* atomics */
#define COUNT(v) __atomic_add_fetch(&(v),1,__ATOMIC_RELAXED)
static pthread_mutex_t *stripe_of(uint32_t id) { return &stripes[(id%BUCKETS)%STRIPES]; }
static void host_check(int e) {
    if (e) { fprintf(stderr,"STOP: semaphore host error %d\n",e); exit(21); }
}
/* The semaphore with this id, referenced (put() releases it), or NULL. */
static Sema *get(uint32_t id) {
    pthread_mutex_t *l=stripe_of(id);
    pthread_mutex_lock(l);
    Sema *s=buckets[id%BUCKETS];
    while (s && s->id!=id) s=s->next;
    if (s) ++s->refs;
    pthread_mutex_unlock(l);
    return s;
}
static void put(Sema *s) {
    pthread_mutex_t *l=stripe_of(s->id);
    pthread_mutex_lock(l);
    const int last=!--s->refs && s->deleted;
    pthread_mutex_unlock(l);
    if (last) { pthread_mutex_destroy(&s->m); free(s); }
}
/* Waits use this clock for their deadlines. winpthreads condition variables accept only
 * CLOCK_REALTIME (pthread_condattr_setclock(CLOCK_MONOTONIC) fails with EINVAL). */
#ifdef _WIN32
#define WAIT_CLOCK CLOCK_REALTIME
#else
#define WAIT_CLOCK CLOCK_MONOTONIC
#endif
static uint64_t now_ns(void) {
    struct timespec t;
    if (clock_gettime(WAIT_CLOCK,&t)) { perror("clock_gettime"); exit(1); }
    return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
}
static ABI int32_t sem_create(uint32_t *out,const char *name,uint32_t attr,
                              int32_t initial,int32_t maximum,const void *options) {
    if (!out || !name || attr>2 || initial<0 || maximum<=0 || initial>maximum) return ERR(22);
    if (options) { fputs("STOP: semaphore options are not implemented\n",stderr); exit(21); }
    Sema *s=calloc(1,sizeof(*s));
    if (!s) return ERR(12);
    const uint32_t id=__atomic_fetch_add(&next_id,1,__ATOMIC_RELAXED);
    if (id==UINT32_MAX) { free(s); return ERR(28); }
    s->id=id; s->attr=attr; s->count=s->initial=initial; s->maximum=maximum;
    host_check(pthread_mutex_init(&s->m,NULL));
    pthread_mutex_t *l=stripe_of(id);
    pthread_mutex_lock(l);
    s->next=buckets[id%BUCKETS]; buckets[id%BUCKETS]=s;
    pthread_mutex_unlock(l);
    *out=id; COUNT(created);
    if (getenv("THEORDER_TRACE_SEMA")) /* PROTOTYPE creates thousands per minute */
        printf("Runtime: semaphore id=%u, attr=%u, initial=%d, max=%d\n",id,attr,initial,maximum);
    return 0;
}
static int32_t wait_count(uint32_t id,int32_t need,uint32_t *timeout,int block,uintptr_t caller) {
    Sema *s=get(id);
    if (!s) return ERR(3);
    int32_t result=0;
    pthread_mutex_lock(&s->m);
    if (need<=0 || need>s->maximum) result=ERR(22);
    else if (s->count>=need) { s->count-=need; COUNT(acquired); }
    else if (!block) result=ERR(16);
    else if (timeout && !*timeout) { result=ERR(60); COUNT(timed_out); }
    else {
        /* Priority-ordered semaphores (attr 2) are served FIFO: there is no
           PS4 priority scheduler, host threads run at equal priority. */
        if (getenv("THEORDER_TRACE_SEMA")) fprintf(stderr,"Runtime: blocking wait on semaphore %u (need %d, count %d)\n",id,need,s->count); /* deadlock diagnosis */
        Waiter w={.need=need,.tid=host_tid(),.caller=caller};
        pthread_condattr_t attr;
        host_check(pthread_condattr_init(&attr));
        host_check(pthread_condattr_setclock(&attr,WAIT_CLOCK));
        host_check(pthread_cond_init(&w.event,&attr));
        host_check(pthread_condattr_destroy(&attr));
        Waiter **tail=&s->first;
        while (*tail) tail=&(*tail)->next;
        *tail=&w;
        uint64_t deadline=timeout ? now_ns()+(uint64_t)*timeout*1000 : 0;
        struct timespec end={.tv_sec=(time_t)(deadline/1000000000),.tv_nsec=(long)(deadline%1000000000)};
        while (!w.done) {
            int e=timeout ? pthread_cond_timedwait(&w.event,&s->m,&end) : pthread_cond_wait(&w.event,&s->m);
            if (e==ETIMEDOUT && !w.done) {
                Waiter **p=&s->first;
                while (*p!=&w) p=&(*p)->next;
                *p=w.next; w.result=ERR(60); w.done=1; COUNT(timed_out);
            } else if (e!=ETIMEDOUT) host_check(e);
        }
        result=w.result;
        if (!result) COUNT(acquired);
        if (timeout) {
            uint64_t now=now_ns();
            *timeout=result ? 0 : (uint32_t)(now>=deadline ? 0 : (deadline-now)/1000);
        }
        host_check(pthread_cond_destroy(&w.event));
    }
    pthread_mutex_unlock(&s->m);
    put(s);
    return result;
}
static ABI int32_t sem_wait(uint32_t id,int32_t need,uint32_t *timeout) {
    return wait_count(id,need,timeout,1,(uintptr_t)__builtin_return_address(0));
}
static ABI int32_t sem_poll(uint32_t id,int32_t need) { return wait_count(id,need,NULL,0,0); }
static ABI int32_t sem_signal(uint32_t id,int32_t count) {
    Sema *s=get(id);
    if (!s) return ERR(3);
    int32_t result=0;
    pthread_mutex_lock(&s->m);
    if (count<=0 || count>s->maximum-s->count) result=ERR(22);
    else {
        s->count+=count; COUNT(signaled);
        Waiter **p=&s->first;
        while (*p) {
            Waiter *w=*p;
            if (w->need>s->count) { p=&w->next; continue; }
            s->count-=w->need; *p=w->next; w->done=1;
            host_check(pthread_cond_signal(&w->event));
        }
    }
    pthread_mutex_unlock(&s->m);
    put(s);
    return result;
}
static int32_t wake_all(Sema *s,int32_t result) {
    int32_t count=0;
    while (s->first) {
        Waiter *w=s->first; s->first=w->next;
        w->result=result; w->done=1; ++count;
        host_check(pthread_cond_signal(&w->event));
    }
    return count;
}
static ABI int32_t sem_cancel(uint32_t id,int32_t count,int32_t *waiters) {
    Sema *s=get(id);
    if (!s) return ERR(3);
    int32_t result=0;
    pthread_mutex_lock(&s->m);
    if (count>s->maximum) result=ERR(22);
    else {
        int32_t n=wake_all(s,ERR(85));
        if (waiters) *waiters=n;
        s->count=count<0 ? s->initial : count;
    }
    pthread_mutex_unlock(&s->m);
    put(s);
    return result;
}
static ABI int32_t sem_delete(uint32_t id) {
    pthread_mutex_t *l=stripe_of(id);
    pthread_mutex_lock(l);
    Sema **p=&buckets[id%BUCKETS];
    while (*p && (*p)->id!=id) p=&(*p)->next;
    if (!*p) { pthread_mutex_unlock(l); return ERR(3); }
    Sema *s=*p; *p=s->next; s->deleted=1; ++s->refs;
    pthread_mutex_unlock(l);
    COUNT(deleted);
    pthread_mutex_lock(&s->m);
    wake_all(s,ERR(13));
    pthread_mutex_unlock(&s->m);
    put(s);
    return 0;
}
/* PROTOTYPE's engine event: { uint32 counter; uint64 semaphore id at +8 }. Set(n) lowers the
 * counter by n and, when it reaches 0, signals the semaphore with its maximum (every waiter and
 * later waiter passes); Reset(n) re-arms a zero counter with n and cancels the semaphore's
 * tokens; Wait waits on the semaphore while the counter is not 0. In the game's code Set's
 * decrement and its signal are two steps: a Reset and a new job between them (the waiter saw
 * 0 and moved on) leave the late signal's tokens on the re-armed event, and the next Wait
 * returns before its job ran. The decompressor then reads empty results and loops for ever:
 * endless loading screens. On the PS4 the gap is a few instructions; here sem_signal first
 * takes the semaphore lock every thread shares, so it is far wider. probe.c points the game's
 * Set and Reset here, where each runs whole under a lock of its event. */
typedef struct { volatile uint32_t counter; uint32_t pad; uint64_t sema; } GuestEvent;
static pthread_mutex_t event_locks[64]={[0 ... 63]=PTHREAD_MUTEX_INITIALIZER};
static size_t events_set, events_reset;
static pthread_mutex_t *event_lock(const void *e) { return &event_locks[((uintptr_t)e>>4)%64]; }
ABI int32_t runtime_event_set(void *event,uint32_t n) {
    GuestEvent *e=event;
    if (!n) return 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    pthread_mutex_t *l=event_lock(e);
    pthread_mutex_lock(l);
    uint32_t old=__atomic_load_n(&e->counter,__ATOMIC_SEQ_CST), next;
    do next=old>n ? old-n : 0;
    while (!__atomic_compare_exchange_n(&e->counter,&old,next,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST));
    int32_t r=0;
    if (old-1<n) r=sem_signal((uint32_t)e->sema,0x7fffffff);
    COUNT(events_set);
    pthread_mutex_unlock(l);
    return r;
}
ABI int32_t runtime_event_reset(void *event,uint32_t n) {
    GuestEvent *e=event;
    pthread_mutex_t *l=event_lock(e);
    pthread_mutex_lock(l);
    uint32_t zero=0;
    __atomic_compare_exchange_n(&e->counter,&zero,n,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
    int32_t r=0;
    if (n) r=sem_cancel((uint32_t)e->sema,0,NULL);
    COUNT(events_reset);
    pthread_mutex_unlock(l);
    return r;
}
/* The F10 hang report: semaphores someone waits on, with their counts. */
void runtime_sema_report_waiting(void) {
    unsigned shown=0;
    for (unsigned b=0;b<BUCKETS && shown<40;++b) {
        pthread_mutex_t *l=&stripes[b%STRIPES];
        pthread_mutex_lock(l);
        for (Sema *s=buckets[b];s && shown<40;s=s->next) {
            pthread_mutex_lock(&s->m);
            unsigned n=0; int32_t need=0;
            for (Waiter *w=s->first;w;w=w->next) { ++n; if (!need) need=w->need; }
            if (n) {
                fprintf(stderr,"  semaphore %u: %u waiting (first needs %d), count %d of %d; waiters:",s->id,n,need,s->count,s->maximum);
                for (Waiter *w=s->first;w;w=w->next)
                    fprintf(stderr," tid %lu (call returns to guest offset 0x%" PRIxPTR ")",w->tid,
                            w->caller-runtime_guest_image);
                fputc('\n',stderr);
                ++shown;
            }
            pthread_mutex_unlock(&s->m);
        }
        pthread_mutex_unlock(l);
    }
}
/* Read-only test/diagnostic snapshot, not exposed to guest code. */
unsigned runtime_sema_waiters(uint32_t id) {
    Sema *s=get(id); unsigned n=0;
    if (!s) return 0;
    pthread_mutex_lock(&s->m);
    for (Waiter *w=s->first;w;w=w->next) ++n;
    pthread_mutex_unlock(&s->m);
    put(s);
    return n;
}
uintptr_t runtime_sema_resolve(const char *name) {
    if (!strcmp(name,"188x57JYp0g#libkernel")) return (uintptr_t)sem_create;
    if (!strcmp(name,"Zxa0VhQVTsk#libkernel")) return (uintptr_t)sem_wait;
    if (!strcmp(name,"4czppHBiriw#libkernel")) return (uintptr_t)sem_signal;
    if (!strcmp(name,"12wOHk8ywb0#libkernel")) return (uintptr_t)sem_poll;
    if (!strcmp(name,"4DM06U2BNEY#libkernel")) return (uintptr_t)sem_cancel;
    if (!strcmp(name,"R1Jvn8bSCW8#libkernel")) return (uintptr_t)sem_delete;
    return 0;
}
void runtime_sema_report(void) {
    printf("Runtime: semaphores created=%zu, deleted=%zu, acquired=%zu, signals=%zu, timeouts=%zu\n",
           created,deleted,acquired,signaled,timed_out);
    if (events_set || events_reset)
        printf("Runtime: engine events set=%zu, reset=%zu (made atomic)\n",events_set,events_reset);
}

