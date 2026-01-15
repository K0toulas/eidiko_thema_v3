#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

#define SOCKET_PATH "/tmp/scheduler_socket"

int main() {
    int client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client_fd == -1) {
        fprintf(stderr, "Failed to create socket: %s\n", strerror(errno));
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(client_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        fprintf(stderr, "Failed to connect to scheduler: %s\n", strerror(errno));
        close(client_fd);
        return 1;
    }

    pid_t shutdown_pid = -1;
    ssize_t bytes_written = write(client_fd, &shutdown_pid, sizeof(pid_t));
    if (bytes_written != sizeof(pid_t)) {
        fprintf(stderr, "Failed to send shutdown message: %s\n", strerror(errno));
        close(client_fd);
        return 1;
    }

    printf("Shutdown message sent to scheduler\n");
    close(client_fd);
    return 0;
}