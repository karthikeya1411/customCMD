/**
 * daemon.c
 *
 * Implementation of the daemonize() function.
 */
#include "sh_share.h" // For standard includes
#include "daemon.h"

/**
 * daemonize
 *
 * Performs the standard "double-fork" to detach the process
 * from the controlling terminal and run it in the background.
 */
void daemonize() {
    // First fork: Parent exits, child continues.
    pid_t pid = fork();
    if (pid < 0) exit(1); // Fork error
    if (pid > 0) exit(0); // Parent exits

    // First child: Become session leader to detach from TTY.
    if (setsid() < 0) exit(1);

    // Ignore hangup signal, as the controlling TTY is gone.
    signal(SIGHUP, SIG_IGN);

    // Second fork: Ensure daemon is not session leader,
    // which prevents it from re-acquiring a TTY.
    pid = fork();
    if (pid < 0) exit(1); // Fork error
    if (pid > 0) exit(0); // Session leader (first child) exits

    // Second child (the daemon) continues.

    // Change CWD to root to prevent locking mounted filesystems.
    chdir("/");

    // Clear file mask to have full control over file permissions.
    umask(0);

    // Close standard file descriptors.
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    // Redirect stdin, stdout, and stderr to /dev/null.
    // This prevents library calls from failing if they
    // try to read/write from standard I/O.
    open("/dev/null", O_RDONLY); // stdin (fd 0)
    open("/dev/null", O_WRONLY); // stdout (fd 1)
    open("/dev/null", O_WRONLY); // stderr (fd 2)
}