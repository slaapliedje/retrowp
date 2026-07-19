#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "types.h"
#include "http_client.h"

#define HTTP_RESPONSE_BUF_CAP 65536u

static wp_status set_socket_timeout(int fd, u32 timeout_ms)
{
    struct timeval tv;

    tv.tv_sec = (long)(timeout_ms / 1000u);
    tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) return WP_ERR;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) return WP_ERR;
    return WP_OK;
}

static wp_status send_all(int fd, const u8 *data, u32 len)
{
    u32 sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, data + sent, (size_t)(len - sent), MSG_NOSIGNAL);
        if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? WP_TIMEOUT : WP_ERR;
        if (n == 0) return WP_ERR;
        sent += (u32)n;
    }
    return WP_OK;
}

wp_status http_post_json(const char *host, u16 port, const char *path,
                          const u8 *body, u32 body_len, u32 timeout_ms,
                          u8 *out, u32 out_cap, u32 *out_len)
{
    int fd;
    struct sockaddr_in addr;
    char req_header[512];
    int header_len;
    /* static, not a stack array: this single-request, single-threaded
       client only ever has one call in flight, and 64KB on the stack is
       needlessly large for that. */
    static u8 resp_buf[HTTP_RESPONSE_BUF_CAP];
    u32 resp_len = 0;
    u32 header_end = 0;
    wp_bool found_header_end = WP_FALSE;
    u32 i;

    if (out_len) *out_len = 0;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return WP_ERR;

    if (set_socket_timeout(fd, timeout_ms) != WP_OK) {
        close(fd);
        return WP_ERR;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(fd);
        return WP_ERR;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        wp_status st = (errno == EAGAIN || errno == EINPROGRESS) ? WP_TIMEOUT : WP_ERR;
        close(fd);
        return st;
    }

    header_len = snprintf(req_header, sizeof(req_header),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %lu\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host, (unsigned)port, (unsigned long)body_len);
    if (header_len <= 0 || (size_t)header_len >= sizeof(req_header)) {
        close(fd);
        return WP_ERR;
    }

    if (send_all(fd, (const u8 *)req_header, (u32)header_len) != WP_OK ||
        (body_len > 0 && send_all(fd, body, body_len) != WP_OK)) {
        close(fd);
        return WP_ERR;
    }

    for (;;) {
        ssize_t n;

        if (resp_len >= HTTP_RESPONSE_BUF_CAP) {
            close(fd);
            return WP_ERR; /* response larger than this client will ever expect */
        }
        n = recv(fd, resp_buf + resp_len, (size_t)(HTTP_RESPONSE_BUF_CAP - resp_len), 0);
        if (n < 0) {
            wp_status st = (errno == EAGAIN || errno == EWOULDBLOCK) ? WP_TIMEOUT : WP_ERR;
            close(fd);
            return st;
        }
        if (n == 0) break; /* peer closed -- Connection: close guarantees this means "done" */
        resp_len += (u32)n;
    }
    close(fd);

    for (i = 0; i + 4 <= resp_len; i++) {
        if (resp_buf[i] == '\r' && resp_buf[i + 1] == '\n' &&
            resp_buf[i + 2] == '\r' && resp_buf[i + 3] == '\n') {
            header_end = i + 4;
            found_header_end = WP_TRUE;
            break;
        }
    }
    if (!found_header_end) return WP_ERR;

    {
        u32 body_bytes = resp_len - header_end;
        if (body_bytes > out_cap) return WP_ERR;
        memcpy(out, resp_buf + header_end, body_bytes);
        if (out_len) *out_len = body_bytes;
    }
    return WP_OK;
}
