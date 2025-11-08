/**
 * job.h
 *
 * Header file for the Job Executor module.
 * This module contains the entire P1/P2/P3 "Tee Executor" logic.
 */
#ifndef JOB_H
#define JOB_H

#include "sh_share.h"

/**
 * job_start
 *
 * This is the entry point called by the daemon (P1).
 * It handles forking P2 (Job Manager).
 * - The P1 (daemon) process returns after storing P2's PID.
 * - The P2 (Job Manager) process will detach from SHM,
 * call its internal logic, and exit when done.
 *
 * @param job_index The index (0..MAX_JOBS-1) of the job to run.
 * @param shm_ptr   A pointer to the shared memory queue.
 * @param semid     The semaphore ID (needed by P2).
 */
void job_start(int job_index, struct sh_job_queue *shm_ptr, int semid);

#endif // JOB_H