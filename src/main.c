#include "backmux/config.h"

int hold_cli_main(int argc, char **argv);

int main(int argc, char **argv) {
    /* Started with stdin, stdout, or stderr closed (`backmux ... <&-`), the
     * next file backmux opened would land on that number - a console socket
     * read as its own input, a log written as its output. Hold each closed
     * one open on /dev/null first, as a well-behaved program does. */
    for (int fd = 0; fd <= 2; fd++) {
        if (fcntl(fd, F_GETFD) == -1 && errno == EBADF) {
            int nul = open("/dev/null", fd == 0 ? O_RDONLY : O_WRONLY);
            if (nul >= 0 && nul != fd) close(nul); /* fd was taken meanwhile: nothing to hold */
        }
    }
    return hold_cli_main(argc, argv);
}
