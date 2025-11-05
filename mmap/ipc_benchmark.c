#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

#define FILE_PATH "/tmp/ipc_test_mmap.dat"
#define DATA_SIZE 4096          // 4 KB
#define ITERATIONS 100000       // 100k повідомлень

double timespec_diff_sec(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
}

int main() {
    printf("=== IPC PERFORMANCE TEST: MMAP ===\n");

    int fd = open(FILE_PATH, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    // Виділяємо розмір файлу
    if (ftruncate(fd, DATA_SIZE) == -1) {
        perror("ftruncate");
        return 1;
    }

    // Відображаємо файл у пам'ять
    void *addr = mmap(NULL, DATA_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    close(fd);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    struct timespec start, end;

    if (pid == 0) {
        // Дочірній процес: отримувач
        char buffer[DATA_SIZE];
        for (int i = 0; i < ITERATIONS; i++) {
            memcpy(buffer, addr, DATA_SIZE);
        }
        return 0;
    } else {
        // Батьківський процес: відправник
        char message[DATA_SIZE];
        memset(message, 'A', DATA_SIZE);

        clock_gettime(CLOCK_MONOTONIC, &start);
        for (int i = 0; i < ITERATIONS; i++) {
            memcpy(addr, message, DATA_SIZE);
        }
        clock_gettime(CLOCK_MONOTONIC, &end);

        double seconds = timespec_diff_sec(start, end);
        double mb_transferred = (DATA_SIZE * (double)ITERATIONS) / (1024.0 * 1024.0);
        double throughput = mb_transferred / seconds;
        double latency_us = (seconds / ITERATIONS) * 1e6;

        printf("Transferred: %.2f MB\n", mb_transferred);
        printf("Time: %.6f sec\n", seconds);
        printf("Throughput: %.2f MB/s\n", throughput);
        printf("Latency: %.3f µs/message\n", latency_us);
    }

    munmap(addr, DATA_SIZE);
    unlink(FILE_PATH);
    return 0;
}
