#ifndef FLOP_BENCH_H_
#define FLOP_BENCH_H_

#include <pthread.h>

/* Types */

/* TODO: Create a type here, or reduce the number of arguments */
typedef void * (*bench_ptr_t) (void *);

struct roof_args {
    /* Config */
    float min_runtime;

    /* Fields */
    int n;
    float a;
    float b;
    float *x;
    float *y;

    /* Output */
    double runtime;
    double flops;
    double bw_load;
    double bw_store;
};

typedef void (*roof_ptr_t) (int, float, float, float *, float *,
                            struct roof_args *);

typedef void (*roof_kernel_t)(int, float, float, float *, float *);

struct thread_args {
    /* Input */
    int tid;
    double min_runtime;
    int vlen;
    roof_ptr_t roof;

    /* Output */
    double runtime;
    double flops;
    double bw_load;
    double bw_store;
};

/* Declarations */
extern pthread_barrier_t timer_barrier;
extern pthread_mutex_t runtime_mutex;
extern volatile int runtime_flag;
#endif  // FLOP_BENCH_H_
