#pragma once
#ifndef HOLD_CONFIG_H
#define HOLD_CONFIG_H

/* Feature-test macros must precede every system header in EVERY translation
 * unit, so this header must be the FIRST include in every .c file. */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <libproc.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ID_DISPLAY_HEX_LEN 12
#define ID_HEX_LEN 64
#define ID_STR_LEN (ID_HEX_LEN + 1)
#define ALIAS_MAX_LEN 64
/* An operator inbox (store/inbox.c) and its messages expire after an hour
 * unused: long enough for a slow agent to answer, short enough that names
 * from finished sessions do not pile up. */
#define HOLD_INBOX_TTL_S 3600
/* One inbox message, at most: the same order as a msg through a broker. */
#define HOLD_INBOX_MAX_TEXT 65536
#define STOP_TIMEOUT_MS 5000
#define POLL_SLEEP_MS 25
#ifndef HOLD_VERSION
#define HOLD_VERSION "dev"
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define HOLD_PATH_MAX PATH_MAX
#define MAX_RECORD_BYTES (1024 * 1024)
/* A saved screen (HSNAP1, about 21 bytes a cell): room for 1000x1000. */
#define MAX_SCREEN_FILE_BYTES (32 * 1024 * 1024)
#ifndef HOLD_BOOT_ID_PATH
#define HOLD_BOOT_ID_PATH "/proc/sys/kernel/random/boot_id"
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#define HOLD_NEED_SOCKET_CLOEXEC 1
#endif
#define JSON_MAX_DEPTH 64
/* A PTY process's terminal size, in character cells. --size, the saved
 * detach-size default and backmux resize all take 1..HOLD_TERM_MAX_DIM columns
 * and rows; with nothing to go on, a process starts at the default below. */
/* Shared exit-code constants. See backmux help scripting for command-specific
 * meanings and foreground launches that return the program's exit status. */
#define HOLD_EXIT_USAGE 1
#define HOLD_EXIT_NO_TARGET 5
/* The target exists but has no console for the requested operation.
 * Both PTY and pipe-input processes can have consoles; exit-only wait does
 * not require one. */
#define HOLD_EXIT_NO_SCREEN 10
/* The name asked for is taken (--name, rename). Its own code, not 5: the
 * process it names does exist - "no such process" would be the opposite. */
#define HOLD_EXIT_NAME_TAKEN 11
/* No --restart-delay given: the restart supervisor backs off (1 s, doubling
 * to 30 s; reset after a run of 10 s or more) instead of restarting at once.
 * Why: a program that fails at once would otherwise restart in a tight loop
 * - forever under `always` - burning CPU and flooding its log. An explicit
 * --restart-delay N (0 included) is used exactly as given. */
#define HOLD_RESTART_BACKOFF (-1)
#define HOLD_RESTART_BACKOFF_FIRST_S 1
#define HOLD_RESTART_BACKOFF_MAX_S 30
#define HOLD_RESTART_BACKOFF_RESET_S 10

#define HOLD_TERM_MAX_DIM 1000
#define HOLD_TERM_DEFAULT_COLS 80
#define HOLD_TERM_DEFAULT_ROWS 24
#ifndef HOLD_SYSTEM_STATE_DIR
#if defined(__APPLE__)
#define HOLD_SYSTEM_STATE_DIR "/var/db/backmux"
#else
#define HOLD_SYSTEM_STATE_DIR "/var/lib/backmux"
#endif
#endif

#endif /* HOLD_CONFIG_H */
