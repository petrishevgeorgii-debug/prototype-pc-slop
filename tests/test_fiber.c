/* Fibers (runtime_proto.c): run, return to the thread, switch, and run again from another
 * thread; a fiber is never run while it still runs elsewhere. */
#define _GNU_SOURCE
#include <assert.h>
#include "../src/runtime_proto.c"
static int32_t guest_errno;
int32_t *runtime_errno(void) { return &guest_errno; }
int32_t runtime_guest_errno(int e) { return e; }
void runtime_report(void) {}
uintptr_t runtime_lookup(const RuntimeExport *table,size_t n,const char *name) { (void)table; (void)n; (void)name; return 0; }

static GuestFiber a, b;
static int steps;
static void ABI entry_a(uint64_t init, uint64_t run) {
    assert(init==7 && run==1);
    for (;;) {
        ++steps;
        uint64_t next=0;
        assert(fiber_return_to_thread(100+steps,&next)==0);
    }
}
static void ABI entry_b(uint64_t init, uint64_t run) {
    (void)init; (void)run;
    for (;;) {
        uint64_t back=0;
        assert(fiber_switch(&a,2,&back)==0); /* a then returns to the thread */
    }
}
/* Two threads keep resuming the same fiber: only one may run it at a time. */
static volatile int inside;
static long c_steps;
static GuestFiber c;
static void ABI entry_c(uint64_t init, uint64_t run) {
    (void)init; (void)run;
    for (;;) {
        assert(__atomic_add_fetch(&inside,1,__ATOMIC_SEQ_CST)==1);
        for (volatile int i=0;i<200;++i) {}
        __atomic_sub_fetch(&inside,1,__ATOMIC_SEQ_CST);
        long before=__atomic_add_fetch(&c_steps,1,__ATOMIC_SEQ_CST);
        assert(fiber_return_to_thread(0,NULL)==0);
        /* resumed exactly where it left: the next run is the one after this step */
        assert(__atomic_load_n(&c_steps,__ATOMIC_SEQ_CST)==before);
    }
}
static void *runner(void *arg) {
    int ran=0;
    for (int i=0;i<200000 && ran<20000;++i) if (fiber_run(&c,0,NULL)==0) ++ran;
    *(int *)arg=ran;
    return NULL;
}
int main(void) {
    char ctx_a[256] __attribute__((aligned(16))), ctx_b[256] __attribute__((aligned(16))), ctx_c[256] __attribute__((aligned(16)));
    assert(fiber_initialize(&a,"a",entry_a,7,ctx_a,sizeof ctx_a,NULL,0)==0);
    assert(fiber_initialize(&b,"b",entry_b,8,ctx_b,sizeof ctx_b,NULL,0)==0);
    uint64_t back=0;
    assert(fiber_run(&a,1,&back)==0 && back==101 && a.state==FIBER_IDLE);
    assert(fiber_run(&a,1,&back)==0 && back==102);
    assert(fiber_run(&b,0,&back)==0 && back==103); /* b switched to a, a returned */
    assert(a.state==FIBER_IDLE && b.state==FIBER_IDLE);
    assert(fiber_initialize(&c,"c",entry_c,0,ctx_c,sizeof ctx_c,NULL,0)==0);
    int r1=0, r2=0;
    pthread_t t1, t2;
    assert(!pthread_create(&t1,NULL,runner,&r1) && !pthread_create(&t2,NULL,runner,&r2));
    assert(!pthread_join(t1,NULL) && !pthread_join(t2,NULL));
    assert(c_steps==r1+r2);
    printf("PASS: fibers (run, switch, return; %d + %d runs of one fiber from two threads)\n",r1,r2);
    return 0;
}
