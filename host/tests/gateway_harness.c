#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "gateway_harness.h"
#include "platform.h"

#define GATEWAY_BINARY "bin/mock_gateway"
#define GATEWAY_PORT_WAIT_MS 3000u

wp_status gateway_start(const char *const *extra_args, gateway_handle *handle)
{
    int pipefd[2];
    pid_t pid;

    handle->pid = -1;
    handle->port = 0;
    handle->endpoint[0] = '\0';

    if (pipe(pipefd) != 0) return WP_ERR;

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return WP_ERR;
    }

    if (pid == 0) {
        char *argv[16];
        int argc = 0;

        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        argv[argc++] = (char *)GATEWAY_BINARY;
        if (extra_args) {
            while (*extra_args && argc < 15) {
                argv[argc++] = (char *)*extra_args;
                extra_args++;
            }
        }
        argv[argc] = NULL;

        execv(GATEWAY_BINARY, argv);
        _exit(127); /* exec failed */
    }

    close(pipefd[1]);
    handle->pid = (int)pid;

    {
        char line[32];
        u32 n = 0;
        u32 start = plat_ticks_ms();
        wp_bool got_newline = WP_FALSE;

        while (!got_newline && n < sizeof(line) - 1) {
            u32 elapsed = plat_ticks_ms() - start;
            struct pollfd pfd;
            int pr;
            ssize_t r;

            if (elapsed >= GATEWAY_PORT_WAIT_MS) break;

            pfd.fd = pipefd[0];
            pfd.events = POLLIN;
            pr = poll(&pfd, 1, (int)(GATEWAY_PORT_WAIT_MS - elapsed));
            if (pr <= 0) break;

            r = read(pipefd[0], line + n, 1);
            if (r <= 0) break;

            if (line[n] == '\n') {
                got_newline = WP_TRUE;
            } else {
                n++;
            }
        }
        line[n] = '\0';
        close(pipefd[0]);

        if (!got_newline || n == 0) {
            gateway_kill(handle);
            return WP_ERR;
        }

        handle->port = atoi(line);
        if (handle->port <= 0) {
            gateway_kill(handle);
            return WP_ERR;
        }
    }

    snprintf(handle->endpoint, sizeof(handle->endpoint), "tcp:127.0.0.1:%d", handle->port);
    return WP_OK;
}

void gateway_kill(gateway_handle *handle)
{
    if (handle->pid <= 0) return;
    kill((pid_t)handle->pid, SIGKILL);
    waitpid((pid_t)handle->pid, NULL, 0);
    handle->pid = -1;
}

void gateway_stop(gateway_handle *handle)
{
    int status;

    if (handle->pid <= 0) return;
    kill((pid_t)handle->pid, SIGTERM);
    waitpid((pid_t)handle->pid, &status, 0);
    handle->pid = -1;
}
