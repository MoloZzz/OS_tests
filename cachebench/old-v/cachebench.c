// Compile: gcc -O3 -march=native -pthread -std=c11 -o cachebench cachebench.c
// Example: ./cachebench seq_random 256 100   (array MB=256, passes=100)

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#include <unistd.h>
#include <inttypes.h>

static double now_sec(){
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec/1e9;
}

/* ---------- Utilities ---------- */
static void die(const char *s){ perror(s); exit(1); }

/* ---------- Test A: Sequential vs Random read ---------- */
/* params: array_mb, passes */
void test_seq_random(size_t array_mb, int passes){
    size_t bytes = array_mb * 1024UL * 1024UL;
    size_t n = bytes / sizeof(uint64_t);
    uint64_t *A = aligned_alloc(64, bytes);
    if(!A) die("alloc");
    // init
    for(size_t i=0;i<n;i++) A[i] = i;

    // Sequential read passes
    volatile uint64_t acc = 0;
    double t0 = now_sec();
    for(int p=0;p<passes;p++){
        for(size_t i=0;i<n;i++) acc += A[i];
    }
    double t1 = now_sec();
    double secs_seq = t1 - t0;
    double gb = (double)bytes * passes / (1024.0*1024.0*1024.0);
    printf("SEQ_READ: array=%zub, passes=%d, time=%.6fs, bw=%.3f GB/s, acc=%" PRIu64 "\n",
           bytes, passes, secs_seq, gb/secs_seq, acc);

    // Random read (pseudo-random permutation via linear congruential)
    // build index sequence
    uint64_t *inds = aligned_alloc(64, n * sizeof(uint64_t));
    if(!inds) die("alloc2");
    uint64_t seed = 1469598103934665603ULL;
    for(size_t i=0;i<n;i++){ seed = seed * 1099511628211ULL; inds[i] = seed % n; }

    acc = 0;
    t0 = now_sec();
    for(int p=0;p<passes;p++){
        for(size_t i=0;i<n;i++) acc += A[inds[i]];
    }
    t1 = now_sec();
    double secs_rand = t1 - t0;
    printf("RAND_READ: array=%zub, passes=%d, time=%.6fs, bw=%.3f GB/s, acc=%" PRIu64 "\n",
           bytes, passes, secs_rand, gb/secs_rand, acc);

    free(inds); free(A);
}

/* ---------- Test B: Stride ---------- */
/* params: array_mb, stride in bytes, passes */
void test_stride(size_t array_mb, size_t stride_bytes, int passes){
    size_t bytes = array_mb * 1024UL * 1024UL;
    size_t n = bytes / sizeof(uint8_t);
    uint8_t *A = aligned_alloc(64, bytes);
    if(!A) die("alloc");
    for(size_t i=0;i<n;i++) A[i] = (uint8_t)i;

    size_t step = stride_bytes;
    if(step == 0) step = 64;
    volatile uint64_t acc = 0;
    double t0 = now_sec();
    for(int p=0;p<passes;p++){
        for(size_t i=0;i<bytes;i+=step) acc += A[i];
    }
    double t1 = now_sec();
    double gb = (double)bytes * passes / (1024.0*1024.0*1024.0);
    printf("STRIDE: array=%zub, stride=%zub, passes=%d, time=%.6fs, bw=%.3f GB/s, acc=%" PRIu64 "\n",
           bytes, step, passes, t1-t0, gb/(t1-t0), acc);
    free(A);
}

/* ---------- Test C: Matrix multiply naive vs blocked ---------- */
static void matmul_naive(double *A, double *B, double *C, int N){
    for(int i=0;i<N;i++){
        for(int k=0;k<N;k++){
            double aik = A[i*N + k];
            for(int j=0;j<N;j++){
                C[i*N + j] += aik * B[k*N + j];
            }
        }
    }
}
static void matmul_blocked(double *A, double *B, double *C, int N, int Bsize){
    for(int ii=0; ii<N; ii+=Bsize){
        for(int kk=0; kk<N; kk+=Bsize){
            for(int jj=0; jj<N; jj+=Bsize){
                int iimax = (ii+Bsize> N)?N:ii+Bsize;
                int kkmax = (kk+Bsize> N)?N:kk+Bsize;
                int jjmax = (jj+Bsize> N)?N:jj+Bsize;
                for(int i=ii;i<iimax;i++){
                    for(int k=kk;k<kkmax;k++){
                        double aik = A[i*N + k];
                        for(int j=jj;j<jjmax;j++){
                            C[i*N + j] += aik * B[k*N + j];
                        }
                    }
                }
            }
        }
    }
}
void test_matmul(int N, int block){
    size_t bytes = (size_t)N*N*sizeof(double);
    double *A = aligned_alloc(64, bytes);
    double *B = aligned_alloc(64, bytes);
    double *C = aligned_alloc(64, bytes);
    if(!A||!B||!C) die("alloc matrix");
    for(int i=0;i<N*N;i++){ A[i] = 1.0; B[i] = 1.0; C[i]=0.0; }

    double t0 = now_sec();
    matmul_naive(A,B,C,N);
    double t1 = now_sec();
    printf("MATMUL_NAIVE: N=%d, time=%.6fs\n", N, t1-t0);

    // zero C
    memset(C,0,bytes);
    t0 = now_sec();
    matmul_blocked(A,B,C,N, block);
    t1 = now_sec();
    printf("MATMUL_BLOCK: N=%d, block=%d, time=%.6fs\n", N, block, t1-t0);

    free(A); free(B); free(C);
}

/* ---------- Test D/E: False sharing and atomic vs mutex vs volatile ---------- */
typedef struct { char pad[64]; volatile uint64_t v; } padded_t;
typedef struct { uint64_t v; } plain_t;

struct thr_arg { void *ptr; int iterations; };

void* writer_volatile(void *arg){
    uint64_t *p = (uint64_t*)arg;
    for(int i=0;i<1000000;i++) (*p)++;
    return NULL;
}

void* writer_padded(void *arg){
    padded_t *p = (padded_t*)arg;
    for(int i=0;i<1000000;i++) p->v++;
    return NULL;
}

typedef struct { atomic_uint_fast64_t *ap; int it; } atom_arg;
void* writer_atomic(void *arg){
    atom_arg *a = arg;
    for(int i=0;i<a->it;i++) atomic_fetch_add(a->ap, 1);
    return NULL;
}

typedef struct { uint64_t *p; pthread_mutex_t *m; int it; } mtx_arg;
void* writer_mutex(void *arg){
    mtx_arg *a = arg;
    for(int i=0;i<a->it;i++){
        pthread_mutex_lock(a->m);
        (*a->p)++;
        pthread_mutex_unlock(a->m);
    }
    return NULL;
}

void test_false_sharing(int threads, int ops_per_thread){
    // two counters adjacent -> false sharing if in same cache line
    uint64_t *arr = aligned_alloc(64, sizeof(uint64_t)*threads);
    for(int i=0;i<threads;i++) arr[i]=0;
    pthread_t *ths = malloc(sizeof(pthread_t)*threads);
    double t0 = now_sec();
    for(int i=0;i<threads;i++){
        pthread_create(&ths[i], NULL, writer_volatile, &arr[i]);
    }
    for(int i=0;i<threads;i++) pthread_join(ths[i], NULL);
    double t1 = now_sec();
    printf("FALSE_SHARING_VOLATILE: threads=%d time=%.6fs\n", threads, t1-t0);

    // padded version
    padded_t *parr = aligned_alloc(64, sizeof(padded_t)*threads);
    for(int i=0;i<threads;i++) parr[i].v=0;
    t0 = now_sec();
    for(int i=0;i<threads;i++) pthread_create(&ths[i], NULL, writer_padded, &parr[i]);
    for(int i=0;i<threads;i++) pthread_join(ths[i], NULL);
    t1 = now_sec();
    printf("FALSE_SHARING_PADDED: threads=%d time=%.6fs\n", threads, t1-t0);

    free(arr); free(parr); free(ths);
}

void test_atomic_mutex(int threads, int ops_per_thread){
    atomic_uint_fast64_t *ac = aligned_alloc(64, sizeof(atomic_uint_fast64_t));
    *ac = 0;
    pthread_t *ths = malloc(sizeof(pthread_t)*threads);
    atom_arg *aargs = malloc(sizeof(atom_arg)*threads);

    double t0 = now_sec();
    for(int i=0;i<threads;i++){
        aargs[i].ap = ac; aargs[i].it = ops_per_thread;
        pthread_create(&ths[i], NULL, writer_atomic, &aargs[i]);
    }
    for(int i=0;i<threads;i++) pthread_join(ths[i], NULL);
    double t1 = now_sec();
    printf("ATOMIC: threads=%d ops=%d total=%" PRIu64 " time=%.6fs\n", threads, ops_per_thread, (uint64_t)*ac, t1-t0);

    // mutex version
    uint64_t *mc = aligned_alloc(64, sizeof(uint64_t));
    *mc = 0;
    pthread_t *mt = malloc(sizeof(pthread_t)*threads);
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    mtx_arg *margs = malloc(sizeof(mtx_arg)*threads);
    t0 = now_sec();
    for(int i=0;i<threads;i++){
        margs[i].p = mc; margs[i].m = &m; margs[i].it = ops_per_thread;
        pthread_create(&mt[i], NULL, writer_mutex, &margs[i]);
    }
    for(int i=0;i<threads;i++) pthread_join(mt[i], NULL);
    t1 = now_sec();
    printf("MUTEX: threads=%d ops=%d total=%" PRIu64 " time=%.6fs\n", threads, ops_per_thread, (uint64_t)(*mc), t1-t0);


    free(ac); free(ths); free(aargs); free(mc); free(mt); free(margs);
}

/* ---------- Main and CLI ---------- */
void usage(char *p){
    fprintf(stderr,
        "Usage: %s <test> [args]\n"
        "Tests:\n"
        "  seq_random <array_mb> <passes>\n"
        "  stride <array_mb> <stride_bytes> <passes>\n"
        "  matmul <N> <block>\n"
        "  false_sharing <threads>\n"
        "  atomic <threads> <ops_per_thread>\n"
        "\n", p);
    exit(1);
}

int main(int argc, char **argv){
    if(argc<2) usage(argv[0]);
    if(strcmp(argv[1],"seq_random")==0){
        size_t mb = (argc>2)?atoi(argv[2]):256;
        int passes = (argc>3)?atoi(argv[3]):10;
        test_seq_random(mb, passes);
    } else if(strcmp(argv[1],"stride")==0){
        size_t mb = (argc>2)?atoi(argv[2]):256;
        size_t stride = (argc>3)?atoi(argv[3]):64;
        int passes = (argc>4)?atoi(argv[4]):10;
        test_stride(mb, stride, passes);
    } else if(strcmp(argv[1],"matmul")==0){
        int N = (argc>2)?atoi(argv[2]):512;
        int block = (argc>3)?atoi(argv[3]):64;
        test_matmul(N, block);
    } else if(strcmp(argv[1],"false_sharing")==0){
        int threads = (argc>2)?atoi(argv[2]):4;
        test_false_sharing(threads, 1000000);
    } else if(strcmp(argv[1],"atomic")==0){
        int threads = (argc>2)?atoi(argv[2]):4;
        int ops = (argc>3)?atoi(argv[3]):1000000;
        test_atomic_mutex(threads, ops);
    } else {
        usage(argv[0]);
    }
    return 0;
}
