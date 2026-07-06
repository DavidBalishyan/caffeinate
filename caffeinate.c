/*
 * caffeinate - prevent the system from sleeping, a Linux clone of macOS'
 *              caffeinate(8).
 *
 * Init-system agnostic: it talks to D-Bus directly via libdbus-1 and never
 * links against or spawns systemd. The primary backend is the freedesktop
 * "login1" manager interface, which is implemented by systemd-logind on
 * systemd distros and by elogind on non-systemd distros (Void, Devuan,
 * Artix, Gentoo/OpenRC, ...).  When no login1 provider is present it falls
 * back to the session-bus org.freedesktop.ScreenSaver and
 * org.freedesktop.PowerManagement.Inhibit interfaces.
 *
 * Copyright (c) 2026 David Balishyan <davidbalishyan12@gmail.com. Released under the MIT License.
 */

#include <dbus/dbus.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// Globals & small helpers                                          
static const char *prog = "caffeinate";
static bool verbose = false;

static void vlog(const char *fmt, ...) {
    if (!verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s: ", prog);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void warn_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s: ", prog);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

// pidfd_open may not be wrapped by libc; use the syscall directly.
static int sys_pidfd_open(pid_t pid, unsigned int flags) {
#ifdef SYS_pidfd_open
    return (int)syscall(SYS_pidfd_open, pid, flags);
#else
    errno = ENOSYS;
    return -1;
#endif
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Inhibitor bookkeeping

#define MAX_FDS 4

struct inhibitors {
    // login1 (systemd-logind / elogind) block-mode lock fds.
    int fds[MAX_FDS];
    int nfds;

    // Session-bus fallbacks: cookies + the connection that owns them
    DBusConnection *session; // kept open so cookies stay valid 
    dbus_uint32_t ss_cookie; // org.freedesktop.ScreenSaver
    bool ss_held;
    dbus_uint32_t pm_cookie; // org.freedesktop.PowerManagement
    bool pm_held;
};

// login1 backend (init-agnostic: systemd-logind OR elogind)

/*
 * Call login1.Manager.Inhibit(what, who, why, mode) and hold the returned
 * fd. "what" is a colon-separated list of "idle", "sleep", "shutdown", ...
 * Holding the fd keeps the lock; closing it releases the lock.  The lock is
 * bound to the fd, not the bus connection, so the connection may be dropped.
 * Returns the fd on success, -1 on failure.
 */
static int login1_inhibit(DBusConnection *sys, const char *what, const char *why) {
    const char *who = "caffeinate";
    const char *mode = "block";
    DBusError err;
    dbus_error_init(&err);

    DBusMessage *msg = dbus_message_new_method_call(
        "org.freedesktop.login1", "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager", "Inhibit");
    if (!msg) {
        warn_msg("out of memory building login1 request");
        return -1;
    }

    dbus_message_append_args(msg, DBUS_TYPE_STRING, &what,
                 DBUS_TYPE_STRING, &who, DBUS_TYPE_STRING, &why,
                 DBUS_TYPE_STRING, &mode, DBUS_TYPE_INVALID);

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        sys, msg, 5000, &err);
    dbus_message_unref(msg);

    if (dbus_error_is_set(&err)) {
        vlog("login1 Inhibit(%s) failed: %s", what, err.message);
        dbus_error_free(&err);
        return -1;
    }
    if (!reply)
        return -1;

    int fd = -1;
    if (!dbus_message_get_args(reply, &err, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID)) {
        vlog("login1 Inhibit reply had no fd: %s",
             err.message ? err.message : "?");
        dbus_error_free(&err);
        dbus_message_unref(reply);
        return -1;
    }
    dbus_message_unref(reply);
    vlog("login1 lock acquired: what=%s fd=%d", what, fd);
    return fd;
}

// Session-bus fallbacks

// Generic "Inhibit(app, reason) -> uint32 cookie" call.
static bool session_inhibit(DBusConnection *conn, const char *dest,
                const char *path, const char *iface,
                const char *app, const char *reason,
                dbus_uint32_t *cookie_out) {
    DBusError err;
    dbus_error_init(&err);

    DBusMessage *msg = dbus_message_new_method_call(dest, path, iface,
                            "Inhibit");
    if (!msg)
        return false;

    dbus_message_append_args(msg, DBUS_TYPE_STRING, &app, DBUS_TYPE_STRING,
                 &reason, DBUS_TYPE_INVALID);

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        conn, msg, 5000, &err);
    dbus_message_unref(msg);

    if (dbus_error_is_set(&err)) {
        vlog("%s Inhibit failed: %s", iface, err.message);
        dbus_error_free(&err);
        return false;
    }
    if (!reply)
        return false;

    bool ok = dbus_message_get_args(reply, &err, DBUS_TYPE_UINT32,
                    cookie_out, DBUS_TYPE_INVALID);
    if (!ok) {
        dbus_error_free(&err);
    }
    dbus_message_unref(reply);
    if (ok)
        vlog("%s cookie acquired: %u", iface, *cookie_out);
    return ok;
}

static void session_uninhibit(DBusConnection *conn, const char *dest,
                  const char *path, const char *iface,
                  dbus_uint32_t cookie) {
    DBusMessage *msg = dbus_message_new_method_call(dest, path, iface,
                            "UnInhibit");
    if (!msg)
        return;
    dbus_message_append_args(msg, DBUS_TYPE_UINT32, &cookie,
                 DBUS_TYPE_INVALID);
    // Fire and forget.
    dbus_connection_send(conn, msg, NULL);
    dbus_connection_flush(conn);
    dbus_message_unref(msg);
}

// Acquire / release everything
static void inhibitors_release(struct inhibitors *in) {
    for (int i = 0; i < in->nfds; i++)
        if (in->fds[i] >= 0)
            close(in->fds[i]);
    in->nfds = 0;

    if (in->session) {
        if (in->ss_held)
            session_uninhibit(in->session,
                      "org.freedesktop.ScreenSaver",
                      "/org/freedesktop/ScreenSaver",
                      "org.freedesktop.ScreenSaver",
                      in->ss_cookie);
        if (in->pm_held)
            session_uninhibit(in->session,
                      "org.freedesktop.PowerManagement",
                      "/org/freedesktop/PowerManagement/Inhibit",
                      "org.freedesktop.PowerManagement.Inhibit",
                      in->pm_cookie);
        dbus_connection_unref(in->session);
        in->session = NULL;
    }
}

/*
 * Acquire inhibitors for the requested assertions.  Returns the number of
 * backends that succeeded (0 means nothing is actually holding the system
 * awake).
 */
static int inhibitors_acquire(struct inhibitors *in, bool want_idle,
                  bool want_system, bool want_display,
                  const char *why) {
    int held = 0;
    memset(in, 0, sizeof(*in));
    for (int i = 0; i < MAX_FDS; i++)
        in->fds[i] = -1;

    // Primary: login1 (systemd-logind / elogind) on system bus
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *sys = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (dbus_error_is_set(&err)) {
        vlog("system bus unavailable: %s", err.message);
        dbus_error_free(&err);
        sys = NULL;
    }

    if (sys) {
        // Do not exit the process if the bus disconnects
        dbus_connection_set_exit_on_disconnect(sys, FALSE);

        // login1's "idle" lock covers -i and -d (display-idle);
        // its "sleep" lock covers -s
        char what[64];
        what[0] = '\0';
        if (want_idle || want_display)
            strcpy(what, "idle");
        if (want_system) {
            if (what[0])
                strcat(what, ":");
            strcat(what, "sleep");
        }

        if (what[0]) {
            int fd = login1_inhibit(sys, what, why);
            if (fd >= 0) {
                in->fds[in->nfds++] = fd;
                held++;
            }
        }
        // login1 fd is self-contained; we don't need to keep sys.
        dbus_connection_unref(sys);
    }

    // Session bus fallbacks for the desktop (display / idle)
    if (want_display || want_idle) {
        dbus_error_init(&err);
        DBusConnection *ses = dbus_bus_get(DBUS_BUS_SESSION, &err);
        if (dbus_error_is_set(&err)) {
            vlog("session bus unavailable: %s", err.message);
            dbus_error_free(&err);
            ses = NULL;
        }
        if (ses) {
            dbus_connection_set_exit_on_disconnect(ses, FALSE);
            in->session = ses;

            if (session_inhibit(
                    ses, "org.freedesktop.ScreenSaver",
                    "/org/freedesktop/ScreenSaver",
                    "org.freedesktop.ScreenSaver", prog, why,
                    &in->ss_cookie)) {
                in->ss_held = true;
                held++;
            }
            if (session_inhibit(
                    ses, "org.freedesktop.PowerManagement",
                    "/org/freedesktop/PowerManagement/Inhibit",
                    "org.freedesktop.PowerManagement.Inhibit",
                    prog, why, &in->pm_cookie)) {
                in->pm_held = true;
                held++;
            }
            // Keep the connection only if a cookie is held
            if (!in->ss_held && !in->pm_held) {
                dbus_connection_unref(ses);
                in->session = NULL;
            }
        }
    }

    return held;
}

static void usage(FILE *f) {
    fprintf(f,
"Usage: %s [-dismu] [-t seconds] [-w pid] [-v] [command [args...]]\n"
"\n"
"Prevent the system from sleeping.  A Linux, init-system-agnostic clone of\n"
"macOS caffeinate(8).\n"
"\n"
"Assertions:\n"
"  -d            Prevent the display from sleeping.\n"
"  -i            Prevent the system from idle sleeping. (default)\n"
"  -m            Prevent the disk from idle sleeping. (accepted; no-op on\n"
"                Linux, folded into -i)\n"
"  -s            Prevent the system from sleeping entirely.\n"
"  -u            Declare that the user is active: assert display wake for\n"
"                the timeout (default 5s if no -t given).\n"
"\n"
"Duration (mutually usable; whichever ends first wins):\n"
"  -t seconds    Hold the assertion for this many seconds, then exit.\n"
"  -w pid        Hold the assertion until the given pid exits.\n"
"  command ...   Run command and hold the assertion until it exits;\n"
"                caffeinate exits with the command's exit status.\n"
"\n"
"With none of -t/-w/command, caffeinate holds until interrupted (Ctrl-C).\n"
"If no assertion flag is given, -i is assumed.\n"
"\n"
"Other:\n"
"  -v            Verbose: report which backends were engaged.\n"
"  -h, --help    Show this help.\n"
"  --version     Show version.\n",
        prog);
}

int main(int argc, char **argv) {
    if (argc > 0 && argv[0][0])
        prog = "caffeinate";

    bool a_display = false, a_idle = false, a_disk = false;
    bool a_system = false, a_user = false;
    long timeout_s = -1; // -1 => none
    pid_t wait_pid = 0;
    int cmd_index = -1;

    // long options first 
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            puts("caffeinate 1.0.0");
            return 0;
        }
        if (strcmp(argv[i], "--") == 0) {
            cmd_index = i + 1;
            break;
        }
        if (argv[i][0] != '-' || argv[i][1] == '\0') {
            // First non-option token starts the command.  
            cmd_index = i;
            break;
        }

        // Cluster of short flags, e.g. -dis or -t 10.
        for (int j = 1; argv[i][j]; j++) {
            char c = argv[i][j];
            switch (c) {
            case 'd': a_display = true; break;
            case 'i': a_idle = true; break;
            case 'm': a_disk = true; break;
            case 's': a_system = true; break;
            case 'u': a_user = true; break;
            case 'v': verbose = true; break;
            case 'h': usage(stdout); return 0;
            case 't':
            case 'w': {
                // Value is rest of this arg or next arg.
                const char *val = argv[i] + j + 1;
                if (!*val) {
                    if (i + 1 >= argc) {
                        warn_msg("option -%c requires an argument",
                             c);
                        return 2;
                    }
                    val = argv[++i];
                }
                char *end = NULL;
                errno = 0;
                long n = strtol(val, &end, 10);
                if (errno || !end || *end || n < 0 ||
                    (c == 'w' && n == 0)) {
                    warn_msg("invalid argument for -%c: %s",
                         c, val);
                    return 2;
                }
                if (c == 't')
                    timeout_s = n;
                else
                    wait_pid = (pid_t)n;
                goto next_arg; // value consumed rest of token
            }
            default:
                warn_msg("unknown option -%c", c);
                usage(stderr);
                return 2;
            }
        }
    next_arg:;
    }

    // Defaults matching caffeinate(8).
    if (!a_display && !a_idle && !a_disk && !a_system && !a_user)
        a_idle = true;
    if (a_disk)
        a_idle = true; // no disk-idle concept on Linux
    if (a_user) {
        a_display = true;
        if (timeout_s < 0)
            timeout_s = 5; // -u default assertion window
    }

    // Acquire inhibitors
    struct inhibitors in;
    const char *why = "User requested caffeinate";
    int held = inhibitors_acquire(&in, a_idle, a_system, a_display, why);

    if (held == 0) {
        warn_msg("warning: no inhibition backend available; "
             "system may still sleep");
        warn_msg("  (need logind/elogind on the system bus, or a "
             "session bus with ScreenSaver/PowerManagement)");
    } else {
        vlog("engaged %d inhibition backend(s)", held);
    }

    // Block signals we care about; deliver via signalfd
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    int sfd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (sfd < 0) {
        warn_msg("signalfd: %s", strerror(errno));
        inhibitors_release(&in);
        return 1;
    }

    // Optionally spawn the trailing command
    pid_t child = 0;
    if (cmd_index >= 0 && cmd_index < argc) {
        child = fork();
        if (child < 0) {
            warn_msg("fork: %s", strerror(errno));
            inhibitors_release(&in);
            return 1;
        }
        if (child == 0) {
            // Child: restore default signal handling.
            sigprocmask(SIG_UNBLOCK, &mask, NULL);
            execvp(argv[cmd_index], &argv[cmd_index]);
            // 127 like a shell when exec fails.
            fprintf(stderr, "%s: %s: %s\n", prog,
                argv[cmd_index], strerror(errno));
            _exit(127);
        }
        vlog("spawned command '%s' pid=%d", argv[cmd_index], child);
    }

    // Optional pidfd for -w
    int exit_status = 0;
    int pfd = -1;
    if (wait_pid > 0) {
        pfd = sys_pidfd_open(wait_pid, 0);
        if (pfd < 0) {
            if (errno == ESRCH) {
                vlog("pid %d already gone", wait_pid);
                goto done; // nothing to wait for
            }
            vlog("pidfd_open(%d) failed (%s); polling instead",
                 wait_pid, strerror(errno));
        } else {
            vlog("waiting on pid %d via pidfd", wait_pid);
        }
    }

    // Event loop
    long deadline = (timeout_s >= 0) ? now_ms() + timeout_s * 1000 : -1;

    for (;;) {
        struct pollfd pfds[3];
        int n = 0;
        int idx_sig = -1, idx_pidfd = -1;

        pfds[n].fd = sfd;
        pfds[n].events = POLLIN;
        idx_sig = n++;

        if (pfd >= 0) {
            pfds[n].fd = pfd;
            pfds[n].events = POLLIN;
            idx_pidfd = n++;
        }

        int timeout_ms = -1;
        if (deadline >= 0) {
            long rem = deadline - now_ms();
            if (rem <= 0)
                break; // timed out
            timeout_ms = (rem > 1000000) ? 1000000 : (int)rem;
        } else if (wait_pid > 0 && pfd < 0) {
            // Fallback: poll pid liveness every 250ms.
            timeout_ms = 250;
        }

        int r = poll(pfds, n, timeout_ms);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            warn_msg("poll: %s", strerror(errno));
            break;
        }
        if (r == 0) {
            if (deadline >= 0 && now_ms() >= deadline)
                break;
            if (wait_pid > 0 && pfd < 0 &&
                kill(wait_pid, 0) < 0 && errno == ESRCH) {
                vlog("pid %d exited", wait_pid);
                break;
            }
            continue;
        }

        // Signal?
        if (idx_sig >= 0 && (pfds[idx_sig].revents & POLLIN)) {
            struct signalfd_siginfo si;
            while (read(sfd, &si, sizeof(si)) == sizeof(si)) {
                if (si.ssi_signo == SIGCHLD) {
                    int st;
                    pid_t w;
                    while ((w = waitpid(-1, &st,
                                WNOHANG)) > 0) {
                        if (child > 0 && w == child) {
                            if (WIFEXITED(st))
                                exit_status =
                                    WEXITSTATUS(st);
                            else if (WIFSIGNALED(st))
                                exit_status =
                                    128 + WTERMSIG(st);
                            vlog("command exited (status %d)",
                                 exit_status);
                            goto done;
                        }
                    }
                } else {
                    vlog("received signal %u, releasing",
                         si.ssi_signo);
                    // Terminate cleanly.
                    if (child > 0)
                        kill(child, SIGTERM);
                    goto done;
                }
            }
        }

        // Waited-on pid exited (pidfd path)
        if (idx_pidfd >= 0 && (pfds[idx_pidfd].revents & POLLIN)) {
            vlog("pid %d exited", wait_pid);
            break;
        }
    }

done:
    if (pfd >= 0)
        close(pfd);
    close(sfd);
    inhibitors_release(&in);
    vlog("released inhibitors, exiting (%d)", exit_status);
    return exit_status;
}
