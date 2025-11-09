/**
 * ipc.c
 *
 * Implementation file for the IPC utility module.
 * Handles creation, attachment, and deletion of IPC resources.
 */
#include "ipc.h"

/**
 * get_ipc_key (Static Helper)
 *
 * Generates the common System V IPC key.
 * It touches a file at KEY_PATH to ensure ftok() can use it.
 * @returns The generated key_t, or -1 on error.
 */
static key_t get_ipc_key() {
    // Create the key file if it doesn't exist. This file is just
    // a handle for ftok() and doesn't store data.
    int key_fd = open(KEY_PATH, O_CREAT | O_RDONLY, 0666);
    if (key_fd == -1) {
        perror("Failed to create key file");
        return -1;
    }
    close(key_fd);

    // Generate the common IPC key from the file path and project ID.
    return ftok(KEY_PATH, PROJ_ID);
}

/**
 * ipc_setup_server
 *
 * Creates and initializes all IPC resources (SHM, SEM, MSGQ)
 * for the server process.
 * @returns 0 on success, -1 on failure.
 */
int ipc_setup_server(int *shmid, int *semid, int *msgid) {
    key_t key = get_ipc_key();
    if (key == -1) {
        perror("ftok");
        return -1;
    }

    // 1. Create Shared Memory segment
    *shmid = shmget(key, sizeof(struct sh_job_queue), IPC_CREAT | 0666);
    if (*shmid == -1) {
        perror("shmget");
        return -1;
    }

    // 2. Create Semaphore (1 semaphore in the set)
    *semid = semget(key, 1, IPC_CREAT | 0666);
    if (*semid == -1) {
        perror("semget"); // Note: shmget was a typo in original, corrected to semget
        return -1;
    }

    // Initialize semaphore value to 1 (unlocked)
    if (semctl(*semid, 0, SETVAL, 1) == -1) {
        perror("semctl(SETVAL)");
        return -1;
    }

    // 3. Create Message Queue
    *msgid = msgget(key, IPC_CREAT | 0666);
    if (*msgid == -1) {
        perror("msgget");
        return -1;
    }
    
    return 0; // Success
}

/**
 * ipc_cleanup_server
 *
 * Removes all IPC resources given their IDs.
 * Called by the server on shutdown.
 */
void ipc_cleanup_server(int shmid, int semid, int msgid) {
    // Remove the shared memory segment
    shmctl(shmid, IPC_RMID, NULL);
    // Remove the semaphore set
    semctl(semid, 0, IPC_RMID);
    // Remove the message queue
    msgctl(msgid, IPC_RMID, NULL);
    // Clean up the key file
    unlink(KEY_PATH);
}

/**
 * ipc_setup_client
 *
 * Attaches to all existing IPC resources for a client process.
 * This function does not create resources, only accesses them.
 * @returns 0 on success, -1 on failure.
 */
int ipc_setup_client(int *shmid, int *semid, int *msgid) {
    key_t key = get_ipc_key();
    if (key == -1) {
        perror("ftok (client)");
        fprintf(stderr, "Error: Could not generate key. Is the server running?\n");
        return -1;
    }

    // 1. Get Shared Memory ID
    // Note: Size is 0 and no IPC_CREAT flag.
    *shmid = shmget(key, 0, 0666);
    if (*shmid == -1) {
        perror("shmget (client)");
        fprintf(stderr, "Error: Could not get SHM. Is the server running?\n");
        return -1;
    }

    // 2. Get Semaphore ID
    // Note: nsems is 0 and no IPC_CREAT flag.
    *semid = semget(key, 0, 0666);
    if (*semid == -1) {
        perror("semget (client)");
        fprintf(stderr, "Error: Could not get SEM. Is the server running?\n");
        return -1;
    }

    // 3. Get Message Queue ID
    // Note: No IPC_CREAT flag.
    *msgid = msgget(key, 0666);
    if (*msgid == -1) {
        perror("msgget (client)");
        fprintf(stderr, "Error: Could not get MSGQ. Is the server running?\n");
        return -1;
    }

    return 0; // Success
}

/**
 * ipc_get_shm_ptr
 *
 * Attaches to the shared memory segment given its ID.
 * @returns A pointer to the sh_job_queue struct, or (void*)-1 on error.
 */
struct sh_job_queue* ipc_get_shm_ptr(int shmid) {
    struct sh_job_queue* ptr = (struct sh_job_queue *)shmat(shmid, NULL, 0);
    if (ptr == (void *)-1) {
        perror("shmat");
        return (struct sh_job_queue*)-1;
    }
    return ptr;
}