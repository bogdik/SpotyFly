/*
 * Transparent loopback relay for mediad's apiUrl.
 *
 * mediad is configured to connect to ws://127.0.0.1:9/quasar/websocket.
 * Port 9 used to be deliberately absent, which converts a normal internal
 * Quasar status channel into ECONNREFUSED.  This program forwards bytes
 * unchanged to Quasar on 9875.  It is intentionally TCP-only: the WebSocket
 * handshake and all framing remain end-to-end between mediad and Quasar.
 */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define LISTEN_PORT 9
#define TARGET_PORT 9875
#define BUF_SIZE 16384

static int connect_target(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TARGET_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void relay(int client) {
    int target = connect_target();
    if (target < 0) { close(client); return; }
    struct pollfd fds[2] = {{client, POLLIN, 0}, {target, POLLIN, 0}};
    char buf[BUF_SIZE];
    int client_open = 1, target_open = 1;
    while (client_open || target_open) {
        fds[0].events = client_open ? POLLIN : 0;
        fds[1].events = target_open ? POLLIN : 0;
        int n = poll(fds, 2, -1);
        if (n < 0) { if (errno == EINTR) continue; break; }
        for (int i = 0; i < 2; i++) {
            if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            int from = fds[i].fd, to = fds[1 - i].fd;
            ssize_t got = read(from, buf, sizeof(buf));
            if (got <= 0) {
                if (i == 0) client_open = 0; else target_open = 0;
                shutdown(to, SHUT_WR);
                continue;
            }
            for (ssize_t off = 0; off < got;) {
                ssize_t sent = write(to, buf + off, (size_t)(got - off));
                if (sent > 0) { off += sent; continue; }
                if (sent < 0 && errno == EINTR) continue;
                client_open = target_open = 0;
                break;
            }
        }
    }
    close(target);
    close(client);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) return 1;
    int one = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(LISTEN_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(server, 8) < 0) {
        perror("quasar-ws-relay");
        close(server);
        return 1;
    }
    for (;;) {
        int client = accept(server, NULL, NULL);
        if (client < 0) { if (errno == EINTR) continue; break; }
        pid_t pid = fork();
        if (pid == 0) { close(server); relay(client); _exit(0); }
        close(client);
    }
    close(server);
    return 1;
}
