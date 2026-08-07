#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "web.h"

int ws_connect(const char *host, int port, const char *path) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) { close(fd); return -1; }
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { close(fd); return -1; }

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    char req[1024];
    int n = snprintf(req, sizeof req,
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n",
        path, host, port);
    if (n < 0 || writeall(fd, req, (size_t)n) < 0) { close(fd); return -1; }

    char resp[2048];
    size_t got = 0;
    while (got < sizeof resp - 1) {
        ssize_t r = read(fd, resp + got, sizeof resp - 1 - got);
        if (r <= 0) { close(fd); return -1; }
        got += (size_t)r;
        resp[got] = 0;
        if (strstr(resp, "\r\n\r\n")) break;
    }
    if (!strstr(resp, " 101 ")) { close(fd); return -1; }

    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    return fd;
}

static int ws_send_frame(WS *ws, int opcode, const char *p, size_t n) {
    unsigned char hdr[14];
    size_t h = 0;
    hdr[h++] = (unsigned char)(0x80 | opcode);

    if (n < 126) {
        hdr[h++] = (unsigned char)(0x80 | n);
    } else if (n <= 0xffff) {
        hdr[h++] = 0x80 | 126;
        hdr[h++] = (unsigned char)(n >> 8);
        hdr[h++] = (unsigned char)n;
    } else {
        hdr[h++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) hdr[h++] = (unsigned char)(n >> (i * 8));
    }

    unsigned char key[4] = {0x37, 0xfa, 0x21, 0x3d};
    memcpy(hdr + h, key, 4);
    h += 4;

    char *masked = malloc(n ? n : 1);
    if (!masked) return -1;
    for (size_t i = 0; i < n; i++) masked[i] = (char)(p[i] ^ key[i & 3]);

    int rc = 0;
    size_t off = 0;
    const char *parts[2] = {(const char *)hdr, masked};
    size_t lens[2] = {h, n};
    for (int i = 0; i < 2 && rc == 0; i++) {
        off = 0;
        while (off < lens[i]) {
            ssize_t w = write(ws->fd, parts[i] + off, lens[i] - off);
            if (w < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    struct timespec ts = {0, 1000000};
                    nanosleep(&ts, NULL);
                    continue;
                }
                rc = -1;
                break;
            }
            off += (size_t)w;
        }
    }
    free(masked);
    return rc;
}

int ws_send_text(WS *ws, const char *p, size_t n) {
    return ws_send_frame(ws, 0x1, p, n);
}

int ws_fill(WS *ws) {
    char tmp[65536];
    int total = 0;
    for (;;) {
        ssize_t r = read(ws->fd, tmp, sizeof tmp);
        if (r > 0) {
            if (buf_add(&ws->in, tmp, (size_t)r) < 0) return -1;
            total += (int)r;
            continue;
        }
        if (r == 0) { ws->closed = true; return total; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return total;
        ws->closed = true;
        return -1;
    }
}

int ws_next(WS *ws, char **msg, size_t *len) {
    for (;;) {
        const unsigned char *b = (const unsigned char *)ws->in.p;
        size_t have = ws->in.len;
        if (have < 2) return 0;

        int fin = b[0] & 0x80;
        int opcode = b[0] & 0x0f;
        int masked = b[1] & 0x80;
        uint64_t plen = b[1] & 0x7f;
        size_t h = 2;

        if (plen == 126) {
            if (have < 4) return 0;
            plen = ((uint64_t)b[2] << 8) | b[3];
            h = 4;
        } else if (plen == 127) {
            if (have < 10) return 0;
            plen = 0;
            for (int i = 0; i < 8; i++) plen = (plen << 8) | b[2 + i];
            h = 10;
        }
        if (masked) h += 4;
        if (have < h + plen) return 0;

        const char *payload = ws->in.p + h;

        if (opcode == 0x9) {                       // ping
            ws_send_frame(ws, 0xa, payload, (size_t)plen);
        } else if (opcode == 0x8) {                // close
            ws->closed = true;
            buf_consume(&ws->in, h + (size_t)plen);
            return 0;
        } else if (opcode == 0x0 || opcode == 0x1 || opcode == 0x2) {
            if (buf_add(&ws->msg, payload, (size_t)plen) < 0) return -1;
        }

        buf_consume(&ws->in, h + (size_t)plen);

        if (fin && ws->msg.len) {
            *msg = ws->msg.p;
            *len = ws->msg.len;
            return 1;
        }
    }
}

void ws_close(WS *ws) {
    if (ws->fd >= 0) close(ws->fd);
    ws->fd = -1;
    buf_free(&ws->in);
    buf_free(&ws->msg);
}
