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

// -------------------------------------------------------------------- text

// cells per codepoint: 0 for combining marks, 2 for CJK and emoji, 1 otherwise
int cp_width(unsigned c) {
    if (c < 0x300) return 1;
    if ((c >= 0x300 && c <= 0x36F) || (c >= 0xFE00 && c <= 0xFE0F) ||
        c == 0x200D || (c >= 0x20D0 && c <= 0x20F0)) return 0;
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0x303E) ||
        (c >= 0x3041 && c <= 0x33FF) || (c >= 0x3400 && c <= 0x4DBF) ||
        (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xA000 && c <= 0xA4CF) ||
        (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) ||
        (c >= 0xFE30 && c <= 0xFE6F) || (c >= 0xFF00 && c <= 0xFF60) ||
        (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1F9FF) ||
        (c >= 0x20000 && c <= 0x3FFFD)) return 2;
    return 1;
}

// bytes in the utf-8 character at s, at most `left`
static size_t u8_step(const char *s, size_t left) {
    unsigned char c = (unsigned char)*s;
    size_t len = (c & 0xF8) == 0xF0 ? 4 : (c & 0xF0) == 0xE0 ? 3 :
                 (c & 0xE0) == 0xC0 ? 2 : 1;
    return len > left ? 1 : len;
}

static unsigned u8_cp(const char *s, size_t len) {
    unsigned char c = (unsigned char)*s;
    if (len == 1) return c;
    unsigned cp = (unsigned)(c & (0xFF >> (len + 1)));
    for (size_t i = 1; i < len; i++)
        cp = (cp << 6) | (unsigned)(s[i] & 0x3F);
    return cp;
}

size_t utf8_tail(const char *s, size_t len, int cols, int *used) {
    size_t start = 0;
    int w = 0;
    for (size_t i = 0; i < len;) {
        size_t k = u8_step(s + i, len - i);
        w += cp_width(u8_cp(s + i, k));
        i += k;
        while (w > cols && start < i) {
            size_t j = u8_step(s + start, len - start);
            w -= cp_width(u8_cp(s + start, j));
            start += j;
        }
    }
    if (used) *used = w;
    return start;
}

// ------------------------------------------------------------------- base64

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                          "abcdefghijklmnopqrstuvwxyz0123456789+/";

size_t base64_decode(const char *src, size_t n, char *dst) {
    static signed char rev[256];
    static bool ready;
    if (!ready) {
        memset(rev, -1, sizeof rev);
        for (int i = 0; i < 64; i++) rev[(unsigned char)B64[i]] = (signed char)i;
        ready = true;
    }
    unsigned acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        int v = rev[(unsigned char)src[i]];
        if (v < 0) continue;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            dst[o++] = (char)((acc >> bits) & 0xff);
        }
    }
    return o;
}

size_t base64_encode(const void *src, size_t n, char *dst) {
    const unsigned char *p = src;
    size_t i = 0, o = 0;
    for (; i + 2 < n; i += 3) {
        unsigned v = ((unsigned)p[i] << 16) | ((unsigned)p[i + 1] << 8) | p[i + 2];
        dst[o++] = B64[(v >> 18) & 63];
        dst[o++] = B64[(v >> 12) & 63];
        dst[o++] = B64[(v >> 6) & 63];
        dst[o++] = B64[v & 63];
    }
    if (i < n) {
        int rem = (int)(n - i);
        unsigned v = (unsigned)p[i] << 16;
        if (rem == 2) v |= (unsigned)p[i + 1] << 8;
        dst[o++] = B64[(v >> 18) & 63];
        dst[o++] = B64[(v >> 12) & 63];
        dst[o++] = rem == 2 ? B64[(v >> 6) & 63] : '=';
        dst[o++] = '=';
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
