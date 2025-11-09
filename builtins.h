/**
 * builtins.h
 *
 * Header file for the shell's built-in commands.
 */
#ifndef BUILTINS_H
#define BUILTINS_H

#include "sh_share.h"

// --- Local Job Management ---
#define MAX_LOCAL_JOBS 50

/**
 * struct local_job
 * Defines a single job running in the shell's local background.
 */
struct local_job {
    pid_t pid;
    char cmd[MAX_CMD_LEN];
    int running; // 1 = running, 0 = done (reaped)
};

// Functions for managing the local job list (called by shell.c)
void init_local_jobs();
void remove_local_job(pid_t pid);


// --- "SHare" Built-in Command Handlers (IPC) ---
// These commands interact with the shared memory and server.

void do_submit(char *line, int semid, struct sh_job_queue *shm_ptr);
void do_jobstatus(int semid, struct sh_job_queue *shm_ptr);
void do_jobstream(char *job_id_str, int semid, struct sh_job_queue *shm_ptr);
void do_joblog(char *job_id_str, int semid, struct sh_job_queue *shm_ptr);
void do_jobkill(char *job_id_str, int semid, struct sh_job_queue *shm_ptr);
void do_shutdown(int msgid);


// --- Local Built-in Command Handlers ---
// These commands run entirely within the shell's process.

void do_myls();
void do_cd(char *path);
void do_jobs();
void do_kill_local(char *pid_str);


// --- External Command Executor ---

/**
 * do_external_command
 * Forks and executes a non-builtin command.
 * @param tokens The tokenized command and its arguments.
 * @param background 1 if the command should run in the background (&), 0 otherwise.
 */
void do_external_command(char **tokens, int background);

#endif // BUILTINS_H