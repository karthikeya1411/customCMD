/**
 * shared.c
 *
 * The "SHare" Backend Job Server (Refactored)
 *
 * This is the main entry point for the daemon.
 * Its responsibilities are now much simpler:
 * 1.  Initialize IPC (via ipc.c).
 * 2.  Daemonize (via daemon.c).
 * 3.  Initialize Shared Memory.
 * 4.  Run the main loop, scanning for jobs and checking for shutdown.
 * 5.  Delegate job execution (via job.c).
 * 6.  Clean up IPC (via ipc.c).
 */

#include "sh_share.h"
#include "ipc.h"
#include "daemon.h"
#include "job.h"

// --- Global variables for IPC identifiers ---
int shmid, semid, msgid;
struct sh_job_queue *shm_ptr = NULL; // Pointer to our shared memory segment

// --- Global flag for main loop ---
volatile sig_atomic_t server_running = 1;

// --- Function Prototypes ---
void initialize_shm();
void main_server_loop();
void scan_for_queued_job();
void check_for_shutdown_msg();
void shutdown_handler(int signal);


/**
 * Main server entry point
 */
int main() {
    printf("Starting SHare server daemon...\n");

    // 1. Create and initialize IPC resources
    if (ipc_setup_server(&shmid, &semid, &msgid) == -1) {
        fprintf(stderr, "Failed to setup IPC resources. Exiting.\n");
        exit(1);
    }

    // 2. Detach from terminal and run as a daemon
    daemonize();
    
    // 3. Set up signal handler for graceful shutdown
    signal(SIGTERM, shutdown_handler);
    signal(SIGINT, shutdown_handler);
    
    // 4. Initialize shared memory
    shm_ptr = ipc_get_shm_ptr(shmid);
    if (shm_ptr == (void*)-1) {
        fprintf(stderr, "Failed to attach to SHM. Exiting.\n");
        // We are a daemon, so we can't print, but we must exit.
        ipc_cleanup_server(shmid, semid, msgid);
        exit(1);
    }
    initialize_shm();
    
    // 5. Run the main loop
    main_server_loop();

    // 6. Loop has exited (server_running = 0), perform cleanup
    // (Daemons don't printf, but this is useful for debugging)
    // printf("SHare server shutting down.\n");
    if (shm_ptr) {
        shmdt(shm_ptr);
    }
    ipc_cleanup_server(shmid, semid, msgid);

    return 0;
}

/**
 * initialize_shm
 *
 * Attaches to the shared memory segment and initializes it,
 * setting all job slots to EMPTY and resetting the job counter.
 */
void initialize_shm() {
    // --- CRITICAL SECTION ---
    sem_lock(semid);
    
    shm_ptr->job_counter = 101; // Start job IDs from 101
    for (int i = 0; i < MAX_JOBS; i++) {
        shm_ptr->jobs[i].status = STATUS_EMPTY;
        shm_ptr->jobs[i].job_id = 0;
        shm_ptr->jobs[i].pid = 0;
    }
    
    sem_unlock(semid);
    // --- END CRITICAL SECTION ---
}

/**
 * main_server_loop
 *
 * The daemon's primary loop. It sleeps for 1 second, then
 * checks for shutdown messages and scans for new jobs.
 */
void main_server_loop() {
    while (server_running) {
        check_for_shutdown_msg();
        if (!server_running) break;
        
        scan_for_queued_job();
        
        sleep(1); // Poll every 1 second
    }
}

/**
 * check_for_shutdown_msg
 *
 * Checks the message queue for a shutdown command (non-blocking).
 * If a message is received, it sets the global 'server_running'
 * flag to 0, which will terminate the main_server_loop.
 */
void check_for_shutdown_msg() {
    struct msg_buf msg;
    
    if (msgrcv(msgid, &msg, sizeof(msg.mtext), MSG_Q_SHUTDOWN, IPC_NOWAIT) > 0) {
        server_running = 0;
    }
}

/**
 * scan_for_queued_job
 *
 * Scans the shared memory job table for a job with
 * status STATUS_QUEUED. If one is found, it calls
 * the job module to start it.
 */
void scan_for_queued_job() {
    // --- CRITICAL SECTION ---
    sem_lock(semid);

    // Find the first available queued job
    for (int i = 0; i < MAX_JOBS; i++) {
        if (shm_ptr->jobs[i].status == STATUS_QUEUED) {
            // Mark it as RUNNING immediately and fork
            shm_ptr->jobs[i].status = STATUS_RUNNING;
            
            // Delegate execution to the job module
            // job_start handles the fork, P2 logic, and P1 PID update.
            job_start(i, shm_ptr, semid);
            
            break; // Only start one job per scan loop
        }
    }

    sem_unlock(semid);
    // --- END CRITICAL SECTION ---
}


/**
 * shutdown_handler
 *
 * Signal handler to gracefully shut down the server.
 */
void shutdown_handler(int signal) {
    (void)signal; // Unused parameter
    server_running = 0;
}