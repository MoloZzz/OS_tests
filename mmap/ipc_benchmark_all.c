// ipc_benchmark_all.c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <mqueue.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <limits.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <limits.h>
#include <assert.h>

#ifndef MSG_SIZE
#define MSG_SIZE (4 * 1024)   // 4 KB per message
#endif
#ifndef ITERATIONS
#define ITERATIONS 10000      // number of messages
#endif

static inline double diff_sec(struct timespec a, struct timespec b) {
    return (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec)/1e9;
}

static void print_header(const char *title) {
    printf("------------------------------------------------------------\n");
    printf("%s\n", title);
    printf("msg_size=%d bytes, iterations=%d\n", MSG_SIZE, ITERATIONS);
    printf("------------------------------------------------------------\n");
}

/* Utility: wait for child to exit, avoid zombie */
static void wait_child(pid_t pid) {
    int status;
    waitpid(pid, &status, 0);
}

/* --------------- MMAP (file-backed MAP_SHARED) --------------- */
void test_mmap_file() {
    print_header("MMAP (file-backed, MAP_SHARED)");

    const char *path = "/tmp/ipc_bench_mmap_file.dat";
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { perror("open"); return; }
    if (ftruncate(fd, MSG_SIZE) == -1) { perror("ftruncate"); close(fd); return; }

    void *addr = mmap(NULL, MSG_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) { perror("mmap"); close(fd); return; }
    close(fd);

    pid_t pid = fork();
    if (pid == 0) {
        // child: reader
        char *buf = malloc(MSG_SIZE);
        for (int i = 0; i < ITERATIONS; ++i) {
            // busy-wait until first byte becomes non-zero (simple sync)
            while (((volatile char*)addr)[0] == 0) { /* spin */ }
            memcpy(buf, addr, MSG_SIZE);
            // reset flag
            ((volatile char*)addr)[0] = 0;
        }
        free(buf);
        _exit(0);
    } else if (pid > 0) {
        // parent: writer
        char *msg = malloc(MSG_SIZE);
        memset(msg, 'A', MSG_SIZE);
        // ensure flag zero
        ((volatile char*)addr)[0] = 0;

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < ITERATIONS; ++i) {
            memcpy(addr, msg, MSG_SIZE);
            // set flag to notify (non-zero)
            ((volatile char*)addr)[0] = 1;
            // wait for child to reset flag (simple sync)
            while (((volatile char*)addr)[0] != 0) { /* spin */ }
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);

        double sec = diff_sec(t0, t1);
        double mb = (double)MSG_SIZE * ITERATIONS / (1024.0*1024.0);
        printf("Transferred %.3f MB in %.6f s\n", mb, sec);
        printf("Throughput: %.2f MB/s\n", mb / sec);
        printf("Latency: %.3f us/msg\n", (sec / ITERATIONS) * 1e6);

        wait_child(pid);
        munmap(addr, MSG_SIZE);
        unlink(path);
        free(msg);
    } else { perror("fork"); munmap(addr, MSG_SIZE); unlink(path); }
}

/* --------------- MMAP (anonymous MAP_SHARED) --------------- */
/* Anonymous shared mapping usable between parent and child after fork */
void test_mmap_anon_shared() {
    print_header("MMAP (anonymous, MAP_SHARED)");
    void *addr = mmap(NULL, MSG_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) { perror("mmap anon"); return; }

    pid_t pid = fork();
    if (pid == 0) {
        char *buf = malloc(MSG_SIZE);
        for (int i = 0; i < ITERATIONS; ++i) {
            while (((volatile char*)addr)[0] == 0) {}
            memcpy(buf, addr, MSG_SIZE);
            ((volatile char*)addr)[0] = 0;
        }
        free(buf);
        _exit(0);
    } else if (pid > 0) {
        char *msg = malloc(MSG_SIZE);
        memset(msg, 'B', MSG_SIZE);
        ((volatile char*)addr)[0] = 0;

        struct timespec t0,t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < ITERATIONS; ++i) {
            memcpy(addr, msg, MSG_SIZE);
            ((volatile char*)addr)[0] = 1;
            while (((volatile char*)addr)[0] != 0) {}
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double sec = diff_sec(t0,t1);
        double mb = (double)MSG_SIZE * ITERATIONS / (1024.0*1024.0);
        printf("Transferred %.3f MB in %.6f s\n", mb, sec);
        printf("Throughput: %.2f MB/s\n", mb / sec);
        printf("Latency: %.3f us/msg\n", (sec / ITERATIONS) * 1e6);

        wait_child(pid);
        munmap(addr, MSG_SIZE);
        free(msg);
    } else { perror("fork"); munmap(addr, MSG_SIZE); }
}

/* --------------- MMAP (file-backed MAP_PRIVATE) --------------- */
/* MAP_PRIVATE is NOT suitable for IPC — modifications are private */
void test_mmap_file_private() {
    print_header("MMAP (file-backed, MAP_PRIVATE) — changes are private (no IPC)");
    const char *path = "/tmp/ipc_bench_mmap_private.dat";
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { perror("open"); return; }
    if (ftruncate(fd, MSG_SIZE) == -1) { perror("ftruncate"); close(fd); return; }

    void *addr = mmap(NULL, MSG_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (addr == MAP_FAILED) { perror("mmap private"); close(fd); return; }
    close(fd);

    pid_t pid = fork();
    if (pid == 0) {
        // child reads initial content (zero)
        char *buf = malloc(MSG_SIZE);
        for (int i = 0; i < 10; ++i) { // few iterations to show no IPC effect
            memcpy(buf, addr, MSG_SIZE);
            usleep(1000);
        }
        free(buf);
        _exit(0);
    } else if (pid > 0) {
        // parent writes; changes won't be visible to child
        char *msg = malloc(MSG_SIZE);
        memset(msg, 'C', MSG_SIZE);
        for (int i = 0; i < 10; ++i) {
            memcpy(addr, msg, MSG_SIZE);
            usleep(500);
        }
        wait_child(pid);
        munmap(addr, MSG_SIZE);
        unlink(path);
        free(msg);
        printf("MAP_PRIVATE: parent modifications are NOT visible to child (copy-on-write).\n");
    } else { perror("fork"); munmap(addr, MSG_SIZE); unlink(path); }
}

/* --------------- POSIX SHARED MEMORY (shm_open) --------------- */
void test_posix_shm() {
    print_header("POSIX SHM (shm_open + mmap)");
    const char *name = "/ipc_bench_posix_shm";
    // remove existing
    shm_unlink(name);
    int fd = shm_open(name, O_RDWR | O_CREAT, 0666);
    if (fd < 0) { perror("shm_open"); return; }
    if (ftruncate(fd, MSG_SIZE) == -1) { perror("ftruncate"); close(fd); shm_unlink(name); return; }

    void *addr = mmap(NULL, MSG_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) { perror("mmap shm"); close(fd); shm_unlink(name); return; }
    close(fd);

    pid_t pid = fork();
    if (pid == 0) {
        char *buf = malloc(MSG_SIZE);
        for (int i = 0; i < ITERATIONS; ++i) {
            while (((volatile char*)addr)[0] == 0) {}
            memcpy(buf, addr, MSG_SIZE);
            ((volatile char*)addr)[0] = 0;
        }
        free(buf);
        _exit(0);
    } else if (pid > 0) {
        char *msg = malloc(MSG_SIZE);
        memset(msg, 'S', MSG_SIZE);
        ((volatile char*)addr)[0] = 0;
        struct timespec t0,t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < ITERATIONS; ++i) {
            memcpy(addr, msg, MSG_SIZE);
            ((volatile char*)addr)[0] = 1;
            while (((volatile char*)addr)[0] != 0) {}
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double sec = diff_sec(t0,t1);
        double mb = (double)MSG_SIZE * ITERATIONS / (1024.0*1024.0);
        printf("Transferred %.3f MB in %.6f s\n", mb, sec);
        printf("Throughput: %.2f MB/s\n", mb / sec);
        printf("Latency: %.3f us/msg\n", (sec / ITERATIONS) * 1e6);

        wait_child(pid);
        munmap(addr, MSG_SIZE);
        shm_unlink(name);
        free(msg);
    } else { perror("fork"); munmap(addr, MSG_SIZE); shm_unlink(name); }
}

/* --------------- PIPE (unnamed) --------------- */
void test_pipe() {
    print_header("PIPE (unnamed)");
    int p[2];
    if (pipe(p) == -1) { perror("pipe"); return; }

    pid_t pid = fork();
    if (pid == 0) {
        // child reads
        close(p[1]);
        char *buf = malloc(MSG_SIZE);
        ssize_t got;
        for (int i = 0; i < ITERATIONS; ++i) {
            ssize_t need = MSG_SIZE;
            char *ptr = buf;
            while (need > 0) {
                got = read(p[0], ptr, need);
                if (got <= 0) { perror("read pipe"); _exit(1); }
                need -= got;
                ptr += got;
            }
        }
        free(buf);
        close(p[0]);
        _exit(0);
    } else if (pid > 0) {
        // parent writes
        close(p[0]);
        char *msg = malloc(MSG_SIZE);
        memset(msg, 'P', MSG_SIZE);
        struct timespec t0,t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < ITERATIONS; ++i) {
            ssize_t sent = 0;
            char *ptr = msg;
            ssize_t tosend = MSG_SIZE;
            while (tosend > 0) {
                ssize_t w = write(p[1], ptr, tosend);
                if (w <= 0) { perror("write pipe"); break; }
                tosend -= w;
                ptr += w;
                sent += w;
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double sec = diff_sec(t0,t1);
        double mb = (double)MSG_SIZE * ITERATIONS / (1024.0*1024.0);
        printf("Transferred %.3f MB in %.6f s\n", mb, sec);
        printf("Throughput: %.2f MB/s\n", mb / sec);
        printf("Latency: %.3f us/msg\n", (sec / ITERATIONS) * 1e6);

        close(p[1]);
        wait_child(pid);
        free(msg);

        // try to get pipe capacity if supported (Linux)
#ifdef F_GETPIPE_SZ
        int cap = fcntl(p[0], F_GETPIPE_SZ);
        if (cap != -1) printf("Pipe buffer size (F_GETPIPE_SZ): %d bytes\n", cap);
#endif
    } else { perror("fork"); close(p[0]); close(p[1]); }
}

/* --------------- FIFO (named pipe) --------------- */
void test_fifo() {
    print_header("FIFO (named pipe via mkfifo)");
    const char *path = "/tmp/ipc_bench_fifo";
    unlink(path);
    if (mkfifo(path, 0666) == -1) {
        if (errno != EEXIST) { perror("mkfifo"); return; }
    }

    pid_t pid = fork();
    if (pid == 0) {
        // child: reader (open for read)
        int rfd = open(path, O_RDONLY);
        if (rfd < 0) { perror("open fifo r"); _exit(1); }
        char *buf = malloc(MSG_SIZE);
        for (int i = 0; i < ITERATIONS; ++i) {
            ssize_t need = MSG_SIZE;
            char *ptr = buf;
            while (need > 0) {
                ssize_t got = read(rfd, ptr, need);
                if (got <= 0) { perror("fifo read"); _exit(1); }
                need -= got; ptr += got;
            }
        }
        free(buf);
        close(rfd);
        _exit(0);
    } else if (pid > 0) {
        // parent: writer
        int wfd = open(path, O_WRONLY);
        if (wfd < 0) { perror("open fifo w"); wait_child(pid); return; }
        char *msg = malloc(MSG_SIZE);
        memset(msg, 'F', MSG_SIZE);
        struct timespec t0,t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < ITERATIONS; ++i) {
            ssize_t tosend = MSG_SIZE;
            char *ptr = msg;
            while (tosend > 0) {
                ssize_t w = write(wfd, ptr, tosend);
                if (w <= 0) { perror("fifo write"); break; }
                tosend -= w; ptr += w;
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double sec = diff_sec(t0,t1);
        double mb = (double)MSG_SIZE * ITERATIONS / (1024.0*1024.0);
        printf("Transferred %.3f MB in %.6f s\n", mb, sec);
        printf("Throughput: %.2f MB/s\n", mb / sec);
        printf("Latency: %.3f us/msg\n", (sec / ITERATIONS) * 1e6);

        close(wfd);
        wait_child(pid);
        free(msg);
        unlink(path);
    } else { perror("fork"); unlink(path); }
}

/* --------------- UNIX SOCKETPAIR (AF_UNIX, SOCK_STREAM) --------------- */
void test_socketpair_stream() {
    print_header("socketpair(AF_UNIX, SOCK_STREAM)");
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == -1) { perror("socketpair"); return; }

    pid_t pid = fork();
    if (pid == 0) {
        close(sv[0]);
        char *buf = malloc(MSG_SIZE);
        for (int i = 0; i < ITERATIONS; ++i) {
            ssize_t need = MSG_SIZE;
            char *ptr = buf;
            while (need > 0) {
                ssize_t r = read(sv[1], ptr, need);
                if (r <= 0) { perror("socket read"); _exit(1); }
                need -= r; ptr += r;
            }
        }
        free(buf);
        close(sv[1]);
        _exit(0);
    } else if (pid > 0) {
        close(sv[1]);
        char *msg = malloc(MSG_SIZE);
        memset(msg, 'O', MSG_SIZE);
        struct timespec t0,t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < ITERATIONS; ++i) {
            ssize_t tosend = MSG_SIZE;
            char *ptr = msg;
            while (tosend > 0) {
                ssize_t w = write(sv[0], ptr, tosend);
                if (w <= 0) { perror("socket write"); break; }
                tosend -= w; ptr += w;
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double sec = diff_sec(t0,t1);
        double mb = (double)MSG_SIZE * ITERATIONS / (1024.0*1024.0);
        printf("Transferred %.3f MB in %.6f s\n", mb, sec);
        printf("Throughput: %.2f MB/s\n", mb / sec);
        printf("Latency: %.3f us/msg\n", (sec / ITERATIONS) * 1e6);

        close(sv[0]);
        wait_child(pid);
        free(msg);
    } else { perror("fork"); close(sv[0]); close(sv[1]); }
}

/* --------------- FILE I/O (open/write then read) --------------- */
void test_file_io() {
    print_header("File I/O (open/write then open/read)");
    const char *path = "/tmp/ipc_bench_file.dat";
    unlink(path);

    struct timespec t0,t1;
    char *buf = malloc(MSG_SIZE);

    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < ITERATIONS; ++i) {
        // write
        int wfd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (wfd < 0) { perror("open w"); free(buf); return; }
        ssize_t tosend = MSG_SIZE; char *ptr = buf; ssize_t w;
        while (tosend > 0) {
            w = write(wfd, ptr, tosend);
            if (w <= 0) { perror("file write"); close(wfd); free(buf); return; }
            tosend -= w; ptr += w;
        }
        close(wfd);
        // read
        int rfd = open(path, O_RDONLY);
        if (rfd < 0) { perror("open r"); free(buf); return; }
        ssize_t need = MSG_SIZE; ptr = buf;
        while (need > 0) {
            ssize_t r = read(rfd, ptr, need);
            if (r <= 0) { perror("file read"); close(rfd); free(buf); return; }
            need -= r; ptr += r;
        }
        close(rfd);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double sec = diff_sec(t0,t1);
    double mb = (double)MSG_SIZE * ITERATIONS * 2 / (1024.0*1024.0); // read+write
    printf("Total IO (write+read): %.3f MB in %.6f s\n", mb, sec);
    printf("Throughput: %.2f MB/s\n", mb / sec);
    printf("Latency (per write+read): %.3f us\n", (sec / ITERATIONS) * 1e6);

    free(buf);
    unlink(path);
}

/* --------------- POSIX MESSAGE QUEUE (mq) --------------- */
void test_posix_mq() {
    print_header("POSIX Message Queue (mq_open/mq_send/mq_receive)");
    const char *name = "/ipc_bench_mq";
    mq_unlink(name);

    struct mq_attr attr;
    attr.mq_maxmsg = 10;              // small queue capacity
    attr.mq_msgsize = MSG_SIZE;
    attr.mq_flags = 0;

    mqd_t mq = mq_open(name, O_CREAT | O_RDWR, 0666, &attr);
    if (mq == (mqd_t)-1) { perror("mq_open"); return; }

    // get actual attributes
    struct mq_attr real_attr;
    if (mq_getattr(mq, &real_attr) == 0) {
        printf("mq: maxmsg=%ld, msgsize=%ld\n", real_attr.mq_maxmsg, real_attr.mq_msgsize);
    }

    pid_t pid = fork();
    if (pid == 0) {
        // child: receiver
        mqd_t r = mq_open(name, O_RDONLY);
        if (r == (mqd_t)-1) { perror("mq_open child"); _exit(1); }
        char *buf = malloc(real_attr.mq_msgsize);
        unsigned int prio;
        for (int i = 0; i < ITERATIONS; ++i) {
            ssize_t rcv = mq_receive(r, buf, real_attr.mq_msgsize, &prio);
            if (rcv == -1) { perror("mq_receive"); _exit(1); }
        }
        free(buf);
        mq_close(r);
        _exit(0);
    } else if (pid > 0) {
        // parent: sender
        char *msg = malloc(MSG_SIZE);
        memset(msg, 'M', MSG_SIZE);
        struct timespec t0,t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < ITERATIONS; ++i) {
            if (mq_send(mq, msg, MSG_SIZE, 0) == -1) {
                // if queue full, block or fail depending on flags; we retry
                if (errno == EAGAIN) { usleep(10); --i; continue; }
                perror("mq_send");
                break;
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double sec = diff_sec(t0,t1);
        double mb = (double)MSG_SIZE * ITERATIONS / (1024.0*1024.0);
        printf("Transferred %.3f MB in %.6f s\n", mb, sec);
        printf("Throughput: %.2f MB/s\n", mb / sec);
        printf("Latency: %.3f us/msg\n", (sec / ITERATIONS) * 1e6);

        wait_child(pid);
        free(msg);
        mq_close(mq);
        mq_unlink(name);
    } else { perror("fork"); mq_close(mq); mq_unlink(name); }
}

/* --------------- Runner --------------- */
int main(int argc, char **argv) {
    int run_all = 1;
    // optional args could be added to run specific test
    printf("=== IPC BENCHMARK (simple) ===\n");
    printf("Message size: %d bytes, iterations: %d\n\n", MSG_SIZE, ITERATIONS);

    test_mmap_file();
    test_mmap_anon_shared();
    test_mmap_file_private();   // demonstrates MAP_PRIVATE isn't IPC
    test_posix_shm();
    test_pipe();
    test_fifo();
    test_socketpair_stream();
    test_file_io();
    test_posix_mq();

    printf("\nAll tests finished.\n");
    return 0;
}
