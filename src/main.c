#define _GNU_SOURCE     /* CPU_*, pthread_attr_setaffinity_np declaration */
#include <features.h>   /* Manually set __USE_GNU (some platforms need this) */

#include <math.h>
#include <pthread.h>    /* pthread_*, CPU_* */
#include <sched.h>      /* CPU_* */
#include <stdio.h>      /* printf */
#include <stdlib.h>     /* strtol, malloc */

#include "sse.h"
#include "sse_fma.h"
#include "avx.h"
#include "avx_fma.h"
#include "avx512.h"
#include "roof.h"
#include "bench.h"
#include "input.h"

#include "stopwatch.h"
#include "gpu_roof.h"

int main(int argc, char *argv[])
{
    /* Input variables */
    struct input_config *cfg;

    cfg = malloc(sizeof(struct input_config));
    parse_input(argc, argv, cfg);

    /* CPU set variables */
    cpu_set_t cpuset;
    int ncpus;
    int *cpus;
    int id, c;

    int b, t;
    int vlen, vlen_start, vlen_end;
    double vlen_scale;
    int nthreads;

    /* Thread control variables */
    pthread_mutex_t mutex;
    pthread_attr_t attr;
    pthread_barrier_t barrier;
    pthread_t *threads;
    struct thread_args *t_args;
    void *status;

    volatile int runtime_flag;

    /* Output variables */
    FILE *output = NULL;
    double **results = NULL;

    double total_flops, total_bw_load, total_bw_store;

    /* Ensemble handler */
    int ens;
    double max_total_flops, max_total_bw_load, max_total_bw_store;

    vlen_start = cfg->vlen_start;
    vlen_end = cfg->vlen_end;
    vlen_scale = cfg->vlen_scale;
    nthreads = cfg->nthreads;

    /* Thread setup */

    threads = malloc(nthreads * sizeof(pthread_t));
    t_args = malloc(nthreads * sizeof(struct thread_args));

    pthread_mutex_init(&mutex, NULL);
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    pthread_barrier_init(&barrier, NULL, nthreads);

    /* Generate the CPU set */
    sched_getaffinity(0, sizeof(cpuset), &cpuset);
    ncpus = CPU_COUNT(&cpuset);
    cpus = malloc(ncpus * sizeof(int));

    c = 0;
    for (id = 0; c < ncpus; id++) {
        if (CPU_ISSET(id, &cpuset)) {
            cpus[c] = id;
            c++;
        }
    }

    ///* Testing a struct for SIMD benchmarks */
    //const struct microbench mbenches[] = {
    //    {.thread = &sse_add, .name = "sse_add"},
    //};

    /* General benchmark loop */
    /* TODO: Combine name and bench into a struct, or add to t_args? */
    const bench_ptr_t benchmarks[] = {
        &sse_add,
        &sse_fma,
        &sse_fmac,
        &avx_add,
        &avx_mac,
        &avx_fma,
        &avx_fmac,
        &avx512_add,
        &avx512_fma,
        &roof_thread,
        &roof_thread,
        &roof_thread,
        &roof_thread,
        &roof_thread,
        &roof_thread,
        &roof_thread,
        &roof_thread,
        &roof_thread,
    0};

    const char * benchnames[] = {
        "sse_add",
        "sse_fma",
        "sse_fmac",
        "avx_add",
        "avx_mac",
        "avx_fma",
        "avx_fmac",
        "avx512_add",
        "avx512_fma",
        "y[:] = x[:]",
        "y[:] = a x[:]",
        "y[:] = x[:] + x[:]",
        "y[:] = x[:] + y[:]",
        "y[:] = a x[:] + y[:]",
        "y[:] = a x[:] + b y[:]",
        "y[1:] = x[1:] + x[:-1]",
        "y[8:] = x[8:] + x[:-8]",
        "GPU: y[:] = a * x[:] + y[:]",
    0};

    const roof_ptr_t roof_tests[] = {
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        &roof_copy,
        &roof_ax,
        &roof_xpx,
        &roof_xpy,
        &roof_axpy,
        &roof_axpby,
        &roof_diff,
        &roof_diff8,
        &gpu_axpy,
    0};

    /* IO setup */
    if (cfg->save_output) {
        int nbench;
        for (nbench = 0; benchmarks[nbench]; nbench++) {}

        results = malloc(2 * sizeof(double *));
        results[0] = malloc(nbench * sizeof(double));
        results[1] = malloc(nbench * sizeof(double));

        output = fopen("results.txt", "w");
    }

    /* Timer setup */
    /* TODO: Evaluate in separate file so function can be declared as static */
    /* TODO: Don't do this, Make a "calibrate" func for each Stopwatch type */
    if (cfg->timer_type == TIMER_TSC)
        stopwatch_set_tsc_freq();

    /* Here we "rev up" the cpu frequency.  This has two effects:
     * 1. Removes the need for redundant ensembles.
     * 2. Consistency bewteen POSIX and TSC, since the TSC frequency estimation
     *    revs up the frequency.
     * Rev time needs to be more than 0.01s, but surely this is highly platform
     * dependent.  (Also may be a very x86 issue)
     *
     * TODO: Yes, this needs to be cleaned up.
     * TODO: Don't do this if using TSC, it's already revved.
     * TODO: Create a separate function.
     */
    Stopwatch *timer;
    volatile int a = 0;
    unsigned long iter = 1;
    timer = stopwatch_create(cfg->timer_type);
    do {
        timer->start(timer);
        for (unsigned long i = 0; i < iter; i++)
            a++;
        timer->stop(timer);
        iter *= 2;
    } while (timer->runtime(timer) < 0.1);
    timer->destroy(timer);

    /* TODO: the avx_* tests don't depend on vector length */
    for (vlen = vlen_start; vlen < vlen_end; vlen = ceil(vlen * vlen_scale)) {
        for (b = 0; benchmarks[b]; b++) {
            max_total_flops = 0.;
            max_total_bw_load = 0.;
            max_total_bw_store = 0.;

            for (ens = 0; ens < cfg->ensembles; ens++) {
                for (t = 0; t < nthreads; t++) {
                    /* TODO: Better way to keep processes off busy threads */
                    if (nthreads > 1) {
                        CPU_ZERO(&cpuset);
                        CPU_SET(cpus[t], &cpuset);
                        pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t),
                                                    &cpuset);
                    }

                    /* Thread inputs */
                    t_args[t].tid = t;
                    t_args[t].vlen = vlen;
                    t_args[t].min_runtime = cfg->min_runtime;
                    t_args[t].roof = roof_tests[b];
                    t_args[t].timer_type = cfg->timer_type;
                    t_args[t].mutex = &mutex;
                    t_args[t].barrier = &barrier;
                    t_args[t].runtime_flag = &runtime_flag;

                    pthread_create(&threads[t], &attr, benchmarks[b],
                                   (void *) &t_args[t]);
                }

                for (t = 0; t < nthreads; t++)
                    pthread_join(threads[t], &status);

                total_flops = 0.0;
                total_bw_load = 0.0;
                total_bw_store = 0.0;

                for (t = 0; t < nthreads; t++) {
                    total_flops += t_args[t].flops;
                    total_bw_load += t_args[t].bw_load;
                    total_bw_store += t_args[t].bw_store;
                }

                /* Ensemble maximum */
                if (total_flops > max_total_flops)
                    max_total_flops = total_flops;

                if (total_bw_load > max_total_bw_load)
                    max_total_bw_load = total_bw_load;

                if (total_bw_store > max_total_bw_store)
                    max_total_bw_store = total_bw_store;
            }

            total_flops = max_total_flops;
            total_bw_load = max_total_bw_load;
            total_bw_store = max_total_bw_store;

            if (total_flops > 0.)
                printf("%s GFLOP/s: %.12f (%.12f / thread)\n",
                        benchnames[b], total_flops / 1e9,
                        total_flops / nthreads / 1e9);

            if (total_bw_load > 0. && total_bw_store > 0.)
                printf("%s GB/s: %.12f (%.12f / thread)\n",
                        benchnames[b], (total_bw_load + total_bw_store) / 1e9,
                        (total_bw_load + total_bw_store) / nthreads / 1e9);

            if (cfg->verbose) {
                for (t = 0; t < nthreads; t++) {
                    printf("    - Thread %i %s runtime: %.12f\n",
                           t, benchnames[b], t_args[t].runtime);
                    printf("    - Thread %i %s gflops: %.12f\n",
                           t, benchnames[b], t_args[t].flops /  1e9);
                    printf("    - Thread %i %s load BW: %.12f\n",
                           t, benchnames[b], t_args[t].bw_load /  1e9);
                    printf("    - Thread %i %s store BW: %.12f\n",
                           t, benchnames[b], t_args[t].bw_store /  1e9);
                }
            }

            /* Store results for model output */
            if (cfg->save_output) {
                results[0][b] = total_flops;
                results[1][b] = total_bw_load + total_bw_store;
            }
        }

        if (cfg->save_output)
            fprintf(output, "%i,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n",
                    vlen,
                    results[0][6], results[0][7], results[0][8], results[0][9],
                    results[0][10], results[0][11], results[0][12], results[0][13],
                    results[1][5],
                    results[1][6], results[1][7], results[1][8], results[1][9],
                    results[1][10], results[1][11], results[1][12], results[1][13]
            );
    }

    /* IO cleanup */
    if (cfg->save_output) {
        free(results);
        fclose(output);
    }

    pthread_barrier_destroy(&barrier);
    pthread_attr_destroy(&attr);
    pthread_mutex_destroy(&mutex);
    free(cpus);
    free(t_args);
    free(threads);
    free(cfg);

    return 0;
}
