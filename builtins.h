/**
 * builtins.h
 *
 * Header file for the shell's built-in commands.
 */
#ifndef BUILTINS_H
#define BUILTINS_H

#include "sh_share.h"

// --- Built-in Command Handlers ---
// All handlers take the IPC resources as parameters.

void do_submit(char *line, int semid, struct sh_job_queue *shm_ptr);
void do_jobstatus(int semid, struct sh_job_queue *shm_ptr);
void do_jobstream(char *job_id_str, int semid, struct sh_job_queue *shm_ptr);
void do_joblog(char *job_id_str, int semid, struct sh_job_queue *shm_ptr);
void do_shutdown(int msgid);
void do_myls();
void do_cd(char *path);
void do_external_command(char **tokens);

#endif // BUILTINS_H