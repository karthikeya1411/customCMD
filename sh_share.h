/**
 * sh_share.h
 *
 * This is the common header file for the SH-ell and SH-are system.
 * It defines all the shared data structures, IPC keys, and constants
 * used by both the 'shell' (client) and 'shared' (server) executables.
 *
 * This ensures both processes are operating on the same definitions.
 */

#ifndef SH_SHARE_H
#define SH_SHARE_H

// --- Standard Library Includes ---
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>     // for fork, exec, pid_t, chdir, etc.
#include <errno.h>      // for perror, errno
#include <signal.h>     // for signal, kill
#include <sys/types.h>  // for key_t, pid_t, mode_t
#include <sys/wait.h>   // for wait, waitpid
#include <fcntl.h>      // for open, O_RDONLY, O_WRONLY
#include <dirent.h>     // for opendir, readdir
#include <sys/stat.h>   // for stat, mkfifo

// --- IPC Includes ---
#include <sys/ipc.h>   // for ftok, IPC_CREAT, etc.
#include <sys/shm.h>   // for shmget, shmat, shmdt, shmctl
#include <sys/sem.h>   // for semget, semop, semctl
#include <sys/msg.h>   // for msgget, msgsnd, msgrcv, msgctl

// --- Project Constants ---

// Path for ftok() to generate a consistent IPC key
#define KEY_PATH "/tmp/sh_key"
// Project ID for ftok()
#define PROJ_ID 'S'

#define MAX_JOBS 20         // Max concurrent jobs in the table
#define MAX_CMD_LEN 256     // Max length of a job's command string
#define MSG_Q_SHUTDOWN 1L   // Message type for server shutdown

// --- Job Status Codes ---
// These define the state of a job in the shared memory table
#define STATUS_EMPTY 0      // Slot is free
#define STATUS_QUEUED 1     // Job is waiting to be run
#define STATUS_RUNNING 2    // Job is currently running
#define STATUS_DONE 3       // Job finished successfully
#define STATUS_FAILED 4     // Job terminated with an error

// --- Shared Data Structures ---

/**
 * struct sh_job
 * Defines a single job entry in the job table.
 * This structure is defined exactly as in the project guide.
 */
struct sh_job {
    int job_id;                 // Unique ID (e.g., 101, 102)
    pid_t pid;                  // PID of the "Job Manager" (P2) process
    int status;                 // One of the STATUS_ codes above
    char cmd[MAX_CMD_LEN];      // The full command string
    char log_file[64];          // Path to the persistent log file (e.g., "/tmp/job-101.log")
    char fifo_file[64];         // Path to the live stream FIFO (e.g., "/tmp/job-101.fifo")
};

/**
 * struct sh_job_queue
 * This is the layout of the entire Shared Memory segment.
 * It contains the job table and a persistent counter for new job IDs.
 */
struct sh_job_queue {
    struct sh_job jobs[MAX_JOBS];
    int job_counter; // A persistent counter to assign unique job_ids (e.g., starts at 101)
};

/**
 * struct msg_buf
 * A simple message buffer for the Message Queue.
 * Used exclusively for the shutdown_server command.
 */
struct msg_buf {
    long mtype;         // Message type (will be MSG_Q_SHUTDOWN)
    char mtext[1];      // Dummy payload
};


// --- Semaphore Operations ---
// These are the POSIX definitions for semaphore lock/unlock operations

// "lock" operation: wait for value to be > 0, then decrement by 1
static struct sembuf sop_lock = {0, -1, 0};

// "unlock" operation: increment value by 1
static struct sembuf sop_unlock = {0, 1, 0};

/**
 * sem_lock
 * Helper function to acquire the semaphore lock.
 * It will block until the semaphore is available.
 */
static inline void sem_lock(int semid) {
    if (semop(semid, &sop_lock, 1) == -1) {
        perror("semop(lock)");
    }
}

/**
 * sem_unlock
 * Helper function to release the semaphore lock.
 */
static inline void sem_unlock(int semid) {
    if (semop(semid, &sop_unlock, 1) == -1) {
        perror("semop(unlock)");
    }
}

#endif // SH_SHARE_H