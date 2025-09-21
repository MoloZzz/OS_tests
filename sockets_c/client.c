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
#include <sys/select.h>

#define SERVER_PORT 12345
#define UNIX_PATH "/tmp/demo_socket"

double now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char *argv[]) {
    int use_unix = 0;
    int blocking = 1;
    int async_mode = 0;
    int small_packets = 1;
    int sockfd;
    struct sockaddr_un addr_un;
    struct sockaddr_in addr_in;

    // ---- парсимо аргументи ----
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--unix")) use_unix = 1;
        if (!strcmp(argv[i], "--nonblocking")) blocking = 0;
        if (!strcmp(argv[i], "--async")) async_mode = 1;
        if (!strcmp(argv[i], "--large")) small_packets = 0;
    }

    // ---- створюємо сокет і підключаємось ----
    double t_start = now_ms();
    if (use_unix) {
        sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
        memset(&addr_un, 0, sizeof(addr_un));
        addr_un.sun_family = AF_UNIX;
        strcpy(addr_un.sun_path, UNIX_PATH);

        connect(sockfd, (struct sockaddr*)&addr_un, sizeof(addr_un));
    } else {
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        memset(&addr_in, 0, sizeof(addr_in));
        addr_in.sin_family = AF_INET;
        addr_in.sin_port = htons(SERVER_PORT);
        addr_in.sin_addr.s_addr = inet_addr("127.0.0.1");

        connect(sockfd, (struct sockaddr*)&addr_in, sizeof(addr_in));
    }
    double t_connect = now_ms() - t_start;

    // ---- non-blocking ----
    if (!blocking || async_mode) {
        int flags = fcntl(sockfd, F_GETFL, 0);
        fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    }

    char small_buf[1024];       // 1 KB
    char large_buf[1024*1024];  // 1 MB
    memset(small_buf, 'A', sizeof(small_buf));
    memset(large_buf, 'B', sizeof(large_buf));

    size_t packets = small_packets ? 100000 : 100;
    size_t size = small_packets ? sizeof(small_buf) : sizeof(large_buf);

    double t0 = now_ms();

    if (async_mode) {
        // ---- async send через select() ----
        fd_set write_fds;
        struct timeval tv = {0, 1000}; // 1 ms
        size_t sent_packets = 0;
        while (sent_packets < packets) {
            FD_ZERO(&write_fds);
            FD_SET(sockfd, &write_fds);
            int ready = select(sockfd+1, NULL, &write_fds, NULL, &tv);
            if (ready > 0 && FD_ISSET(sockfd, &write_fds)) {
                send(sockfd, small_packets ? small_buf : large_buf, size, 0);
                sent_packets++;
            }
        }
    } else {
        // ---- blocking / non-blocking send ----
        for (size_t i = 0; i < packets; i++) {
            send(sockfd, small_packets ? small_buf : large_buf, size, 0);
        }
    }

    // ---- маркер кінця передачі ----
    send(sockfd, "END", 3, 0);
    double t_elapsed = now_ms() - t0;

    printf("Client results:\n");
    printf(" Connect time: %.3f ms\n", t_connect);
    printf(" Packets sent: %zu\n", packets);
    printf(" Bytes sent: %zu\n", packets * size);
    printf(" Duration: %.3f ms\n", t_elapsed);
    printf(" Send rate: %.2f MB/s, %.2f packets/s\n",
           (packets*size/1e6)/(t_elapsed/1000.0),
           packets/(t_elapsed/1000.0));

    close(sockfd);
    return 0;
}
