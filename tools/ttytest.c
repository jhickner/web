// Does poll() report readable input on /dev/tty the way it does on stdin?
// Run under ptycap, which injects a keystroke partway through.

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    int devtty = open("/dev/tty", O_RDWR);
    printf("opened /dev/tty as fd %d, stdin is fd 0\r\n", devtty);
    fflush(stdout);

    int tty_signalled = 0, stdin_signalled = 0, got_byte = 0;

    for (int i = 0; i < 300; i++) {                 // ~3 seconds
        struct pollfd p[2];
        p[0].fd = devtty; p[0].events = POLLIN; p[0].revents = 0;
        p[1].fd = STDIN_FILENO; p[1].events = POLLIN; p[1].revents = 0;

        int rc = poll(p, 2, 10);
        if (rc > 0) {
            if (p[0].revents) {
                printf("poll: /dev/tty revents=0x%x\r\n", p[0].revents);
                tty_signalled = 1;
            }
            if (p[1].revents) {
                printf("poll: stdin revents=0x%x\r\n", p[1].revents);
                stdin_signalled = 1;
            }
            char buf[64];
            int src = p[0].revents ? devtty : STDIN_FILENO;
            ssize_t r = read(src, buf, sizeof buf);
            if (r > 0) {
                printf("read %zd bytes from fd %d:", r, src);
                for (ssize_t j = 0; j < r; j++) printf(" %02x", (unsigned char)buf[j]);
                printf("\r\n");
                got_byte = 1;
                break;
            }
        }
    }
    printf("RESULT devtty_signalled=%d stdin_signalled=%d got_byte=%d\r\n",
           tty_signalled, stdin_signalled, got_byte);
    fflush(stdout);
    return 0;
}
