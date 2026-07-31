// Ask the terminal what it thinks, instead of guessing from pixels.
//
// Sends several kitty-graphics transmissions with responses ENABLED (no q=2)
// and records each reply. Run it in a real terminal window; results go to a
// log file, so no screenshot is needed.
//
//   gquery <logfile> <frame.png>

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static int use_tmux;
static FILE *log_f;

static void esc(const char *seq, size_t n) {
    if (!use_tmux) { (void)!write(STDOUT_FILENO, seq, n); return; }
    char *w = malloc(n * 2 + 32);
    size_t o = 0;
    memcpy(w, "\x1bPtmux;", 7);
    o = 7;
    for (size_t i = 0; i < n; i++) {
        if (seq[i] == 0x1b) w[o++] = 0x1b;
        w[o++] = seq[i];
    }
    w[o++] = 0x1b;
    w[o++] = '\\';
    (void)!write(STDOUT_FILENO, w, o);
    free(w);
}

// Drain replies for a moment and log whatever came back.
static void collect(const char *label) {
    char buf[4096];
    size_t got = 0;
    for (int i = 0; i < 40 && got < sizeof buf - 1; i++) {
        struct pollfd p = {STDIN_FILENO, POLLIN, 0};
        if (poll(&p, 1, 50) > 0 && (p.revents & POLLIN)) {
            ssize_t r = read(STDIN_FILENO, buf + got, sizeof buf - 1 - got);
            if (r > 0) got += (size_t)r;
        }
    }
    buf[got] = 0;
    fprintf(log_f, "%-34s -> ", label);
    if (!got) {
        fprintf(log_f, "(no response)\n");
    } else {
        for (size_t i = 0; i < got; i++) {
            unsigned char c = (unsigned char)buf[i];
            if (c == 0x1b) fprintf(log_f, "<ESC>");
            else if (c >= 32 && c < 127) fputc(c, log_f);
            else fprintf(log_f, "\\x%02x", c);
        }
        fputc('\n', log_f);
    }
    fflush(log_f);
}

static const char B64T[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64(const unsigned char *in, size_t n, char *out) {
    size_t o = 0, i = 0;
    for (; i + 2 < n; i += 3) {
        unsigned v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out[o++] = B64T[(v >> 18) & 63]; out[o++] = B64T[(v >> 12) & 63];
        out[o++] = B64T[(v >> 6) & 63];  out[o++] = B64T[v & 63];
    }
    if (i < n) {
        unsigned v = in[i] << 16;
        int rem = (int)(n - i);
        if (rem == 2) v |= in[i + 1] << 8;
        out[o++] = B64T[(v >> 18) & 63]; out[o++] = B64T[(v >> 12) & 63];
        out[o++] = rem == 2 ? B64T[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = 0;
    return o;
}

// Transmit with responses on. chunk == 0 means a single escape.
static void transmit(const char *data, size_t len, unsigned id, size_t chunk,
                     const char *label) {
    char *seq = malloc(len + 65536);
    if (chunk == 0) {
        int hl = sprintf(seq, "\x1b_Ga=t,f=100,t=d,i=%u;", id);
        memcpy(seq + hl, data, len);
        memcpy(seq + hl + len, "\x1b\\", 2);
        esc(seq, (size_t)hl + len + 2);
    } else {
        size_t off = 0;
        int first = 1;
        while (off < len) {
            size_t n = len - off;
            if (n > chunk) n = chunk;
            int more = (off + n < len);
            int hl;
            if (first)
                hl = sprintf(seq, "\x1b_Ga=t,f=100,t=d,i=%u,m=%d;", id, more);
            else
                hl = sprintf(seq, "\x1b_Gm=%d;", more);
            memcpy(seq + hl, data + off, n);
            memcpy(seq + hl + n, "\x1b\\", 2);
            esc(seq, (size_t)hl + n + 2);
            off += n;
            first = 0;
        }
    }
    free(seq);
    collect(label);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: gquery log frame.png\n"); return 2; }
    log_f = fopen(argv[1], "w");
    if (!log_f) return 1;

    FILE *f = fopen(argv[2], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *png = malloc((size_t)fsz);
    if (fread(png, 1, (size_t)fsz, f) != (size_t)fsz) return 1;
    fclose(f);

    char *data = malloc((size_t)fsz * 2 + 64);
    size_t dlen = b64(png, (size_t)fsz, data);

    use_tmux = getenv("TMUX") != NULL;
    fprintf(log_f, "TERM=%s tmux=%s png=%ld base64=%zu\n",
            getenv("TERM") ? getenv("TERM") : "?", use_tmux ? "yes" : "no",
            fsz, dlen);

    struct termios saved, raw;
    tcgetattr(STDIN_FILENO, &saved);
    raw = saved;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    // A tiny payload first, to prove replies reach us at all.
    static const char TINY[] =
        "iVBORw0KGgoAAAANSUhEUgAAAEAAAABACAAAAACPAi4CAAAASUlEQVR4nO3MsREAMAzCQE+S"
        "/bdMGjNAKHWiU8HP2d3db48AAmiPaQEG0B7TAgygPaYFGEB7TAswgPaYFmAA7TEtwADaY1oA"
        "ATxN7+h58sjORwAAAABJRU5ErkJggg==";
    transmit(TINY, strlen(TINY), 0x0A5F01, 0, "tiny png, one escape");

    transmit(data, dlen, 0x0A5F02, 0,     "real png, one escape");
    transmit(data, dlen, 0x0A5F03, 4096,  "real png, chunked 4096");
    transmit(data, dlen, 0x0A5F04, 65536, "real png, chunked 65536");

    // Halves of the real payload, to find where it stops being accepted.
    transmit(data, dlen / 2, 0x0A5F05, 0, "half png (invalid data), 1 escape");

    // And the emulator's format, as a control.
    {
        size_t n = 64 * 64 * 3;
        unsigned char *rgb = malloc(n);
        for (size_t i = 0; i < n; i++) rgb[i] = (unsigned char)(i * 7);
        char *rb = malloc(n * 2 + 16);
        size_t rl = b64(rgb, n, rb);
        char *seq = malloc(rl + 4096);
        int hl = sprintf(seq, "\x1b_Ga=t,f=24,s=64,v=64,i=%u;", 0x0A5F06);
        memcpy(seq + hl, rb, rl);
        memcpy(seq + hl + rl, "\x1b\\", 2);
        esc(seq, (size_t)hl + rl + 2);
        collect("raw rgb f=24, one escape");
        free(seq); free(rb); free(rgb);
    }

    // The transmissions above all succeed, so the question is what happens when
    // an image is given a *virtual* placement - the part placeholders rely on.
    {
        char seq[512];
        int n;

        n = sprintf(seq, "\x1b_Ga=p,U=1,i=%u,p=1,c=8,r=4\x1b\\", 0x0A5F02);
        esc(seq, (size_t)n);
        collect("a=p,U=1 virtual placement");

        n = sprintf(seq, "\x1b_Ga=p,i=%u,p=2,c=8,r=4\x1b\\", 0x0A5F02);
        esc(seq, (size_t)n);
        collect("a=p classic placement");

        // Transmit and place in one command, the other documented spelling.
        char *seq2 = malloc(dlen + 4096);
        int hl = sprintf(seq2, "\x1b_Ga=T,U=1,f=100,t=d,i=%u,c=8,r=4;", 0x0A5F07);
        memcpy(seq2 + hl, data, dlen);
        memcpy(seq2 + hl + dlen, "\x1b\\", 2);
        esc(seq2, (size_t)hl + dlen + 2);
        collect("a=T,U=1 transmit+virtual place");
        free(seq2);

        // Does this terminal advertise graphics at all?
        n = sprintf(seq, "\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\");
        esc(seq, (size_t)n);
        collect("a=q support query");
    }

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
    fprintf(log_f, "done\n");
    fclose(log_f);
    return 0;
}
