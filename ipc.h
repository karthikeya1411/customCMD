/**
 * ipc.h
 *
 * Header file for the IPC utility module.
 * Declares functions for setting up and tearing down
 * all System V IPC resources for the server and client.
 */
#ifndef IPC_H
#define IPC_H

#include "sh_share.h"

// --- Server Functions ---

/**
 * ipc_setup_server
 * Creates and initializes all IPC resources (SHM, SEM, MSGQ).
 * Populates the provided integer pointers with their IDs.
 * Returns 0 on success, -1 on failure.
 */
int ipc_setup_server(int *shmid, int *semid, int *msgid);

/**
 * ipc_cleanup_server
 * Removes all IPC resources given their IDs.
 */
void ipc_cleanup_server(int shmid, int semid, int msgid);


// --- Client Functions ---

/**
 * ipc_setup_client
 * Attaches to all existing IPC resources.
 * Populates the provided integer pointers with their IDs.
 * Returns 0 on success, -1 on failure.
 */
int ipc_setup_client(int *shmid, int *semid, int *msgid);


// --- Common Functions ---

/**
 * ipc_get_shm_ptr
 * Attaches to the shared memory segment given its ID.
 * Returns a pointer to the shared memory or (void*)-1 on error.
 */
struct sh_job_queue* ipc_get_shm_ptr(int shmid);


#endif // IPC_H