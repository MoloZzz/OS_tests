#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <errno.h>
#include <inttypes.h>

static inline double now_sec() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

/* ---------------- seq_read ---------------- */
void run_seq_read(size_t N) {
    // allocate array bigger than L3 to force capacity misses eventually
    uint8_t *a = aligned_alloc(64, N);
    if (!a) { perror("alloc"); exit(1); }
    // init
    for (size_t i = 0; i < N; ++i) a[i] = (uint8_t)(i & 0xFF);

    volatile uint64_t sum = 0;
    double t0 = now_sec();
    for (size_t i = 0; i < N; ++i) {
        sum += a[i];
    }
    double t1 = now_sec();
    printf("seq_read: N=%zu time=%.6f s sum=%" PRIu64 "\n", N, t1 - t0, (uint64_t)sum);
    free(a);
}

/* ---------------- rand_read ---------------- */
void run_rand_read(size_t N, unsigned stride) {
    // We will create a permutation / pointer-chase like access to avoid prefetch benefits
    uint64_t *arr = aligned_alloc(64, N * sizeof(uint64_t));
    if (!arr) { perror("alloc"); exit(1); }
    // fill with next index (mod N), simple stride permutation
    for (size_t i = 0; i < N; ++i) {
        arr[i] = (i + stride) % N;
    }
    // pointer-chase
    size_t idx = 0;
    volatile uint64_t steps = 0;
    double t0 = now_sec();
    for (size_t i = 0; i < N; ++i) {
        idx = arr[idx];
        steps += idx;
    }
    double t1 = now_sec();
    printf("rand_read: N=%zu stride=%u time=%.6f s steps=%" PRIu64 "\n", N, stride, t1 - t0, (uint64_t)steps);
    free(arr);
}

/* ---------------- matmul ----------------
   We'll implement three orders: ijk (cache-friendly), ikj (less friendly for typical row-major)
*/
void matmul_ijk(double *A, double *B, double *C, int n) {
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            double sum = 0.0;
            for (int k = 0; k < n; ++k)
                sum += A[i*n + k] * B[k*n + j];
            C[i*n + j] = sum;
        }
}

void matmul_ikj(double *A, double *B, double *C, int n) {
    for (int i = 0; i < n; ++i)
        for (int k = 0; k < n; ++k) {
            double aik = A[i*n + k];
            for (int j = 0; j < n; ++j)
                C[i*n + j] += aik * B[k*n + j];
        }
}

void run_matmul(int n) {
    size_t total = (size_t)n*n;
    double *A = aligned_alloc(64, total * sizeof(double));
    double *B = aligned_alloc(64, total * sizeof(double));
    double *C = aligned_alloc(64, total * sizeof(double));
    if (!A||!B||!C) { perror("alloc"); exit(1); }
    for (size_t i = 0; i < total; ++i) {
        A[i] = 1.0 * (i % 17);
        B[i] = 1.0 * (i % 31);
        C[i] = 0.0;
    }

    double t0 = now_sec();
    matmul_ijk(A,B,C,n);
    double t1 = now_sec();
    printf("matmul ijk: n=%d time=%.6f s sample C[0]=%f\n", n, t1-t0, C[0]);

    // reset C
    for (size_t i = 0; i < total; ++i) C[i] = 0.0;
    t0 = now_sec();
    matmul_ikj(A,B,C,n);
    t1 = now_sec();
    printf("matmul ikj: n=%d time=%.6f s sample C[0]=%f\n", n, t1-t0, C[0]);

    free(A); free(B); free(C);
}

/* ---------------- false_sharing ---------------- */
typedef struct {
    volatile uint64_t val;
    // note: we will demonstrate both with and without padding in separate experiments
} cell_t;

typedef struct {
    cell_t *cells;
    int thread_id;
    size_t iters;
    int pad; // whether cells are padded externally
} fs_arg_t;

void *fs_worker(void *arg) {
    fs_arg_t *a = arg;
    size_t it = a->iters;
    int id = a->thread_id;
    for (size_t i = 0; i < it; ++i) {
        a->cells[id].val++;
    }
    return NULL;
}

void *worker_no_pad(void *arg) {
    size_t iters = ((size_t *)arg)[0];
    volatile uint64_t *cell = (volatile uint64_t *)((size_t *)arg)[1];
    for (size_t i = 0; i < iters; ++i)
        (*cell)++;
    return NULL;
}

void *worker_padded(void *arg) {
    struct {
        size_t iters;
        volatile uint64_t *cell;
    } *a = arg;
    for (size_t i = 0; i < a->iters; ++i)
        (*a->cell)++;
    return NULL;
}

void run_false_sharing(int nthreads, size_t iters) {
    typedef struct { volatile uint64_t val; } cell_t;
    cell_t *cells = aligned_alloc(64, sizeof(cell_t) * nthreads);
    for (int i = 0; i < nthreads; ++i) cells[i].val = 0;

    pthread_t *ths = malloc(sizeof(pthread_t) * nthreads);
    struct { size_t iters; volatile uint64_t *cell; } *args = malloc(sizeof(*args) * nthreads);

    double t0 = now_sec();
    for (int i = 0; i < nthreads; ++i) {
        args[i].iters = iters;
        args[i].cell = &cells[i].val;
        pthread_create(&ths[i], NULL, worker_no_pad, &args[i]);
    }
    for (int i = 0; i < nthreads; ++i) pthread_join(ths[i], NULL);
    double t1 = now_sec();

    uint64_t sum = 0;
    for (int i = 0; i < nthreads; ++i) sum += cells[i].val;
    printf("false_sharing (no padding): threads=%d iters=%zu time=%.6f s sum=%" PRIu64 "\n",
           nthreads, iters, t1 - t0, sum);

    struct padded {
        volatile uint64_t v;
        char pad[64 - sizeof(uint64_t)];
    } *pcells = aligned_alloc(64, sizeof(*pcells) * nthreads);
    for (int i = 0; i < nthreads; ++i) pcells[i].v = 0;

    pthread_t *ths2 = malloc(sizeof(pthread_t) * nthreads);
    struct { size_t iters; volatile uint64_t *cell; } *args2 = malloc(sizeof(*args2) * nthreads);

    double t2 = now_sec();
    for (int i = 0; i < nthreads; ++i) {
        args2[i].iters = iters;
        args2[i].cell = &pcells[i].v;
        pthread_create(&ths2[i], NULL, worker_padded, &args2[i]);
    }
    for (int i = 0; i < nthreads; ++i) pthread_join(ths2[i], NULL);
    double t3 = now_sec();

    uint64_t sum2 = 0;
    for (int i = 0; i < nthreads; ++i) sum2 += pcells[i].v;
    printf("false_sharing (padded): threads=%d iters=%zu time=%.6f s sum=%" PRIu64 "\n",
           nthreads, iters, t3 - t2, sum2);

    free(cells);
    free(args);
    free(ths);
    free(pcells);
    free(args2);
    free(ths2);
}



/* ---------------- inc_race ---------------- */
typedef struct {
    int threads;
    size_t iters;
} incr_arg_t;

void *incr_race_worker(void *arg) {
    incr_arg_t *a = arg;
    for (size_t i = 0; i < a->iters; ++i) {
        // non-atomic increment
        extern uint64_t race_counter;
        race_counter++;
    }
    return NULL;
}

void *incr_mutex_worker(void *arg) {
    incr_arg_t *a = arg;
    extern pthread_mutex_t mtx;
    extern uint64_t mutex_counter;
    for (size_t i = 0; i < a->iters; ++i) {
        pthread_mutex_lock(&mtx);
        mutex_counter++;
        pthread_mutex_unlock(&mtx);
    }
    return NULL;
}

void *incr_atomic_worker(void *arg) {
    incr_arg_t *a = arg;
    extern atomic_uint_fast64_t atomic_counter;
    for (size_t i = 0; i < a->iters; ++i) {
        atomic_fetch_add_explicit(&atomic_counter, 1, memory_order_relaxed);
    }
    return NULL;
}

uint64_t race_counter = 0;
pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
uint64_t mutex_counter = 0;
atomic_uint_fast64_t atomic_counter = 0;

void run_inc_race(int nthreads, size_t iters) {
    pthread_t *ths = malloc(sizeof(pthread_t) * nthreads);
    incr_arg_t arg = { .threads = nthreads, .iters = iters };

    // 1) race (no sync)
    race_counter = 0;
    double t0 = now_sec();
    for (int i = 0; i < nthreads; ++i) pthread_create(&ths[i], NULL, incr_race_worker, &arg);
    for (int i = 0; i < nthreads; ++i) pthread_join(ths[i], NULL);
    double t1 = now_sec();
    printf("inc_race (no sync): expected=%" PRIu64 " got=%" PRIu64 " time=%.6f s\n",
           (uint64_t)nthreads*(uint64_t)iters, (uint64_t)race_counter, t1 - t0);

    // 2) mutex
    mutex_counter = 0;
    t0 = now_sec();
    for (int i = 0; i < nthreads; ++i) pthread_create(&ths[i], NULL, incr_mutex_worker, &arg);
    for (int i = 0; i < nthreads; ++i) pthread_join(ths[i], NULL);
    t1 = now_sec();
    printf("inc_mutex: expected=%" PRIu64 " got=%" PRIu64 " time=%.6f s\n",
           (uint64_t)nthreads*(uint64_t)iters, (uint64_t)mutex_counter, t1 - t0);

    // 3) atomic
    atomic_store(&atomic_counter, 0);
    t0 = now_sec();
    for (int i = 0; i < nthreads; ++i) pthread_create(&ths[i], NULL, incr_atomic_worker, &arg);
    for (int i = 0; i < nthreads; ++i) pthread_join(ths[i], NULL);
    t1 = now_sec();
    printf("inc_atomic: expected=%" PRIu64 " got=%" PRIu64 " time=%.6f s\n",
           (uint64_t)nthreads*(uint64_t)iters, (uint64_t)atomic_load(&atomic_counter), t1 - t0);

    free(ths);
}

/* ---------------- main & dispatch ---------------- */
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <mode> [args...]\n", argv[0]);
        return 1;
    }
    char *mode = argv[1];
    if (strcmp(mode,"seq_read")==0) {
        size_t N = argc>2? strtoull(argv[2],NULL,10) : 100000000;
        run_seq_read(N);
    } else if (strcmp(mode,"rand_read")==0) {
        size_t N = argc>2? strtoull(argv[2],NULL,10) : 10000000;
        unsigned stride = argc>3? atoi(argv[3]) : 16;
        run_rand_read(N, stride);
    } else if (strcmp(mode,"matmul")==0) {
        int n = argc>2? atoi(argv[2]) : 512;
        run_matmul(n);
    } else if (strcmp(mode,"false_sharing")==0) {
        int nth = argc>2? atoi(argv[2]) : 4;
        size_t it = argc>3? strtoull(argv[3],NULL,10) : 10000000;
        run_false_sharing(nth, it);
    } else if (strcmp(mode,"inc_race")==0) {
        int nth = argc>2? atoi(argv[2]) : 4;
        size_t it = argc>3? strtoull(argv[3],NULL,10) : 10000000;
        run_inc_race(nth, it);
    } else {
        fprintf(stderr, "Unknown mode %s\n", mode);
        return 1;
    }
    return 0;
}
