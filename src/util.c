#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include "web.h"

int buf_reserve(Buf *b, size_t need) {
    if (b->cap >= need) return 0;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < need) cap *= 2;
    char *np = realloc(b->p, cap);
    if (!np) return -1;
    b->p = np;
    b->cap = cap;
    return 0;
}

int buf_add(Buf *b, const void *src, size_t n) {
    if (buf_reserve(b, b->len + n + 1) < 0) return -1;
    memcpy(b->p + b->len, src, n);
    b->len += n;
    b->p[b->len] = 0;
    return 0;
}

int buf_addf(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if (buf_reserve(b, b->len + (size_t)n + 1) < 0) return -1;
    va_start(ap, fmt);
    vsnprintf(b->p + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
    return 0;
}

void buf_consume(Buf *b, size_t n) {
    if (n >= b->len) { b->len = 0; if (b->p) b->p[0] = 0; return; }
    memmove(b->p, b->p + n, b->len - n);
    b->len -= n;
    b->p[b->len] = 0;
}

void buf_free(Buf *b) {
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

void (*g_input_pump)(void) = NULL;
volatile sig_atomic_t g_write_force = 0;

double now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + tv.tv_usec / 1e6;
}

void mkdirs(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        mkdir(tmp, 0755);
        *p = '/';
    }
    mkdir(tmp, 0755);
}

int writeall(int fd, const char *p, size_t n) {
    double deadline = 0;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (g_input_pump) g_input_pump();
                if (g_quit && !g_write_force) return -1;
                if (g_write_force) {
                    if (deadline == 0) deadline = now_sec() + 2.0;
                    else if (now_sec() > deadline) return -1;
                }
                struct pollfd pf = {fd, POLLOUT, 0};
                poll(&pf, 1, 200);
                continue;
            }
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

// ------------------------------------------------------------------- base64

size_t base64_decode(const char *src, size_t n, char *dst) {
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                            "abcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        const char *p = memchr(A, src[i], 64);
        if (!p) continue;
        acc = (acc << 6) | (unsigned)(p - A);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            dst[o++] = (char)((acc >> bits) & 0xff);
        }
    }
    return o;
}

// --------------------------------------------------------------------- json

static const char *find_key(const char *js, const char *key) {
    char pat[64];
    int n = snprintf(pat, sizeof pat, "\"%s\":", key);
    if (n < 0 || (size_t)n >= sizeof pat) return NULL;
    const char *p = strstr(js, pat);
    return p ? p + n : NULL;
}

const char *json_str(const char *js, const char *key, size_t *len) {
    const char *p = find_key(js, key);
    if (!p) return NULL;
    while (*p == ' ') p++;
    if (*p != '"') return NULL;
    p++;
    const char *e = p;
    while (*e && *e != '"') {
        if (*e == '\\' && e[1]) e++;
        e++;
    }
    *len = (size_t)(e - p);
    return p;
}

double json_num(const char *js, const char *key, double def) {
    const char *p = find_key(js, key);
    if (!p) return def;
    while (*p == ' ' || *p == '"') p++;
    char *end;
    double v = strtod(p, &end);
    return end == p ? def : v;
}

bool json_has(const char *js, const char *key) {
    return find_key(js, key) != NULL;
}

// returns the escape for one byte, or NULL when it passes through; `tmp` backs
// the \u form
static const char *escape_of(unsigned char c, char tmp[8]) {
    switch (c) {
    case '"':  return "\\\"";
    case '\\': return "\\\\";
    case '\n': return "\\n";
    case '\r': return "\\r";
    case '\t': return "\\t";
    default:
        if (c < 0x20) {
            snprintf(tmp, 8, "\\u%04x", c);
            return tmp;
        }
    }
    return NULL;
}

size_t json_escape(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    for (const unsigned char *s = (const unsigned char *)src; *s; s++) {
        char tmp[8];
        const char *rep = escape_of(*s, tmp);
        if (rep) {
            size_t n = strlen(rep);
            if (o + n + 1 >= cap) break;
            memcpy(dst + o, rep, n);
            o += n;
        } else {
            if (o + 2 >= cap) break;
            dst[o++] = (char)*s;
        }
    }
    if (cap) dst[o] = 0;
    return o;
}

// the same escape appended to a growable buffer, without truncating
void json_escape_buf(Buf *out, const char *src) {
    for (const unsigned char *s = (const unsigned char *)src; *s; s++) {
        char tmp[8];
        const char *rep = escape_of(*s, tmp);
        if (rep) buf_add(out, rep, strlen(rep));
        else     buf_add(out, s, 1);
    }
}

// the string a Runtime.evaluate returned by value
const char *json_eval_str(const char *msg, size_t *len) {
    const char *p = strstr(msg, "\"result\":{\"result\":{");
    if (!p || json_has(msg, "exceptionDetails")) return NULL;
    return json_str(p, "value", len);
}

// the four hex digits of a \u escape; caller has checked they are there
static unsigned hex4(const char *s) {
    unsigned cp = 0;
    for (int k = 0; k < 4; k++) {
        char c = s[k];
        cp = cp * 16 + (unsigned)(c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
    }
    return cp;
}

size_t json_unescape(char *dst, size_t cap, const char *src, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 4 < cap; i++) {
        if (src[i] != '\\') { dst[o++] = src[i]; continue; }
        if (++i >= n) break;
        switch (src[i]) {
        case 'n': dst[o++] = '\n'; break;
        case 't': dst[o++] = '\t'; break;
        case 'r': dst[o++] = '\r'; break;
        case 'b': dst[o++] = '\b'; break;
        case 'f': dst[o++] = '\f'; break;
        case 'u': {
            if (i + 4 >= n) { i = n; break; }
            unsigned cp = hex4(src + i + 1);
            i += 4;
            // surrogate pair above the BMP; a half with no partner becomes fffd
            if (cp >= 0xd800 && cp <= 0xdbff) {
                unsigned lo = (i + 6 < n && src[i + 1] == '\\' &&
                               src[i + 2] == 'u') ? hex4(src + i + 3) : 0;
                if (lo >= 0xdc00 && lo <= 0xdfff) {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    i += 6;
                } else {
                    cp = 0xfffd;
                }
            } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                cp = 0xfffd;               // a trailing half
            }
            if (cp < 0x80) {
                dst[o++] = (char)cp;
            } else if (cp < 0x800) {
                dst[o++] = (char)(0xc0 | (cp >> 6));
                dst[o++] = (char)(0x80 | (cp & 0x3f));
            } else if (cp < 0x10000) {
                dst[o++] = (char)(0xe0 | (cp >> 12));
                dst[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
                dst[o++] = (char)(0x80 | (cp & 0x3f));
            } else {
                dst[o++] = (char)(0xf0 | (cp >> 18));
                dst[o++] = (char)(0x80 | ((cp >> 12) & 0x3f));
                dst[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
                dst[o++] = (char)(0x80 | (cp & 0x3f));
            }
            break;
        }
        default: dst[o++] = src[i];
        }
    }
    dst[o] = 0;
    return o;
}
