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
 * 7.  (Feature 2) Manage concurrent job limit.
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

// --- (Feature 2) Global count of running jobs ---
volatile sig_atomic_t running_job_count = 0;

// --- Function Prototypes ---
void initialize_shm();
void main_server_loop();
void scan_for_queued_job();
void check_for_shutdown_msg();
void shutdown_handler(int signal);
void sigchld_handler(int signal); // (Feature 2)


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
    // (Feature 2) Set up handler to reap finished P2 children
    signal(SIGCHLD, sigchld_handler);
    
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
 * (Feature 2) scan_for_queued_job
 *
 * Scans the shared memory job table for a job with
 * status STATUS_QUEUED. If one is found, AND
 * the running_job_count < MAX_CONCURRENT_JOBS,
 * it calls the job module to start it.
 *
 * *** THIS FUNCTION IS A CRITICAL SECTION REGARDING SIGCHLD ***
 */
void scan_for_queued_job() {
    // These types/functions are now visible thanks to sh_share.h
    sigset_t old_mask, block_mask;
    
    // Initialize the mask to block SIGCHLD
    sigemptyset(&block_mask);
    sigaddset(&block_mask, SIGCHLD);
    
    // Block SIGCHLD and save the old mask
    sigprocmask(SIG_BLOCK, &block_mask, &old_mask);
    
    // --- CRITICAL SECTION (for SHM) ---
    sem_lock(semid);

    // (Feature 2) Check if we are at capacity
    if (running_job_count >= MAX_CONCURRENT_JOBS) {
        sem_unlock(semid);
        // Unblock SIGCHLD before returning
        sigprocmask(SIG_SETMASK, &old_mask, NULL);
        return; // At capacity, try again later
    }

    // Find the first available queued job
    // (FIX) Corrected the typo 'ci' to 'i = 0'
    for (int i = 0; i < MAX_JOBS; i++) {
        if (shm_ptr->jobs[i].status == STATUS_QUEUED) {
            // Mark it as RUNNING immediately and fork
            shm_ptr->jobs[i].status = STATUS_RUNNING;
            
            // Delegate execution to the job module
            // job_start handles the fork, P2 logic, and P1 PID update.
            job_start(i, shm_ptr, semid);
            
            // (Feature 2) Increment the running job count
            // This is now safe because SIGCHLD is blocked
            running_job_count++;
            
            break; // Only start one job per scan loop
        }
    }

    sem_unlock(semid);
    // --- END CRITICAL SECTION (for SHM) ---
    
    // Unblock SIGCHLD. Any pending signals will be delivered now.
    sigprocmask(SIG_SETMASK, &old_mask, NULL);
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

/**
 * (Feature 2) sigchld_handler
 *
 * Reaps all finished P2 (Job Manager) processes
 * and decrements the global running_job_count.
 *
 * *** THIS IS A SIGNAL HANDLER - DO NOT CALL NON-SAFE FUNCTIONS ***
 */
void sigchld_handler(int signal) {
    (void)signal; // Unused
    pid_t pid;
    
    // Reap all finished children without blocking
    // waitpid() and assignment to a volatile sig_atomic_t
    // are async-signal-safe.
    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
        // A P2 process finished (or was killed)
        running_job_count--;
    }
}