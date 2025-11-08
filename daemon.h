/**
 * daemon.h
 *
 * Header file for the daemon utility.
 */
#ifndef DAEMON_H
#define DAEMON_H

/**
 * daemonize
 *
 * Performs the standard "double-fork" to detach the process
 * from the controlling terminal and run it in the background.
 */
void daemonize();

#endif // DAEMON_H