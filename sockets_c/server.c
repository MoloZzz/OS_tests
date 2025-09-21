#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <time.h>

#define SERVER_PORT 12345
#define UNIX_PATH "/tmp/demo_socket"

// ---- простий таймер ----
double now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char *argv[]) {
    int use_unix = 0;
    int blocking = 1;
    int sockfd, connfd;
    struct sockaddr_un addr_un;
    struct sockaddr_in addr_in;

    // ---- парсимо аргументи ----
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--unix")) use_unix = 1;
        if (!strcmp(argv[i], "--nonblocking")) blocking = 0;
    }

    double t_start = now_ms();
    if (use_unix) {
        sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
        memset(&addr_un, 0, sizeof(addr_un));
        addr_un.sun_family = AF_UNIX;
        strcpy(addr_un.sun_path, UNIX_PATH);
        unlink(UNIX_PATH);

        bind(sockfd, (struct sockaddr*)&addr_un, sizeof(addr_un));
    } else {
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        memset(&addr_in, 0, sizeof(addr_in));
        addr_in.sin_family = AF_INET;
        addr_in.sin_addr.s_addr = htonl(INADDR_ANY);
        addr_in.sin_port = htons(SERVER_PORT);

        bind(sockfd, (struct sockaddr*)&addr_in, sizeof(addr_in));
    }
    double t_socket = now_ms() - t_start;

    listen(sockfd, 5);

    t_start = now_ms();
    connfd = accept(sockfd, NULL, NULL);
    double t_accept = now_ms() - t_start;

    if (!blocking) {
        int flags = fcntl(connfd, F_GETFL, 0);
        fcntl(connfd, F_SETFL, flags | O_NONBLOCK);
    }

    char buffer[1024*1024];
    size_t total_bytes = 0;
    size_t packets = 0;

    double t0 = now_ms();
    while (1) {
        ssize_t n = recv(connfd, buffer, sizeof(buffer), 0);
        if (n > 0) {
            total_bytes += n;
            packets++;
            if (n == 3 && !memcmp(buffer, "END", 3)) break;
        }
    }
    double t_elapsed = now_ms() - t0;

    printf("Server results:\n");
    printf(" Socket creation: %.3f ms\n", t_socket);
    printf(" Accept time: %.3f ms\n", t_accept);
    printf(" Packets: %zu\n", packets);
    printf(" Bytes: %zu\n", total_bytes);
    printf(" Duration: %.3f ms\n", t_elapsed);
    printf(" Throughput: %.2f MB/s, %.2f packets/s\n",
           (total_bytes/1e6)/(t_elapsed/1000.0),
           packets/(t_elapsed/1000.0));

    close(connfd);
    close(sockfd);
    if (use_unix) unlink(UNIX_PATH);
    return 0;
}
