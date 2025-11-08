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
    pid_t pid = fork();
    if (pid < 0) exit(1); // Fork error
    if (pid > 0) exit(0); // Parent exits

    // Child (P1) becomes session leader
    if (setsid() < 0) exit(1);

    // Handle signals
    signal(SIGHUP, SIG_IGN); // Ignore hangup signal

    // Fork again, let P1 exit
    pid = fork();
    if (pid < 0) exit(1); // Fork error
    if (pid > 0) exit(0); // P1 (session leader) exits

    // P2 (the daemon) continues
    
    // Change working directory to root
    chdir("/");

    // Set new file permissions mask
    umask(0);

    // Close standard file descriptors
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    // Redirect them to /dev/null
    open("/dev/null", O_RDONLY);
    open("/dev/null", O_WRONLY);
    open("/dev/null", O_WRONLY);
}