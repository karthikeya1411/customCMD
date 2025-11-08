/**
 * builtins.h
 *
 * Header file for the shell's built-in commands.
 */
#ifndef BUILTINS_H
#define BUILTINS_H

#include "sh_share.h"

// --- (Feature 3) Local Job Management ---
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

// Functions to be called by shell.c
void init_local_jobs();
void remove_local_job(pid_t pid);


// --- Built-in Command Handlers ---
// All handlers take the IPC resources as parameters.

void do_submit(char *line, int semid, struct sh_job_queue *shm_ptr);
void do_jobstatus(int semid, struct sh_job_queue *shm_ptr);
void do_jobstream(char *job_id_str, int semid, struct sh_job_queue *shm_ptr);
void do_joblog(char *job_id_str, int semid, struct sh_job_queue *shm_ptr);
void do_shutdown(int msgid);
void do_myls();
void do_cd(char *path);

// (Feature 1) New command
void do_jobkill(char *job_id_str, int semid, struct sh_job_queue *shm_ptr);

// (Feature 3) New commands
void do_jobs();
void do_kill_local(char *pid_str);

// (Feature 3) Modified signature for background execution
void do_external_command(char **tokens, int background);

#endif // BUILTINS_H