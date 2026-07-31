// Test harness: run a terminal program on a pty, capture everything it draws,
// optionally feed it keystrokes, then close it down.
//
//   ptycap out.raw <cols> <rows> <seconds> [-k <hex-keys> <delay>] -- cmd args...

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <util.h>

static double now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static size_t unhex(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (; s[0] && s[1] && o < cap; s += 2) {
        unsigned v;
        if (sscanf(s, "%2x", &v) != 1) break;
        out[o++] = (char)v;
    }
    return o;
}

int main(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: ptycap out.raw cols rows secs [-k hex delay] -- cmd...\n");
        return 2;
    }
    const char *outpath = argv[1];
    int cols = atoi(argv[2]), rows = atoi(argv[3]);
    double secs = atof(argv[4]);

    char keys[512];
    size_t nkeys = 0;
    double keydelay = 0;
    int i = 5;
    if (!strcmp(argv[i], "-k")) {
        nkeys = unhex(argv[i + 1], keys, sizeof keys);
        keydelay = atof(argv[i + 2]);
        i += 3;
    }
    if (strcmp(argv[i], "--") != 0) {
        fprintf(stderr, "ptycap: expected --\n");
        return 2;
    }
    i++;

    struct winsize ws = {0};
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;

    int master;
    pid_t pid = forkpty(&master, NULL, NULL, &ws);
    if (pid < 0) { perror("forkpty"); return 1; }
    if (pid == 0) {
        execvp(argv[i], argv + i);
        _exit(127);
    }

    FILE *out = fopen(outpath, "wb");
    if (!out) { perror("fopen"); return 1; }

    double start = now(), sent = 0;
    char buf[65536];
    int  signalled = 0;
    for (;;) {
        double el = now() - start;
        if (el > secs && !signalled) {
            // Keep draining after the signal: the child may be blocked writing
            // a frame, and it cannot exit until we make room.
            kill(pid, SIGTERM);
            signalled = 1;
        }
        if (signalled && el > secs + 3) break;
        if (signalled && waitpid(pid, NULL, WNOHANG) == pid) { pid = 0; break; }
        if (nkeys && !sent && el > keydelay) {
            (void)!write(master, keys, nkeys);
            sent = 1;
        }
        struct pollfd p = {master, POLLIN, 0};
        if (poll(&p, 1, 50) > 0 && (p.revents & POLLIN)) {
            ssize_t r = read(master, buf, sizeof buf);
            if (r <= 0) { if (signalled) break; }
            else fwrite(buf, 1, (size_t)r, out);
        }
    }
    fclose(out);

    if (pid > 0) {
        kill(pid, SIGKILL);
        for (int t = 0; t < 40 && waitpid(pid, NULL, WNOHANG) != pid; t++)
            usleep(25000);
    }
    close(master);
    return 0;
}
