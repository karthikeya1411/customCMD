/**
 * shared.c
 *
 * The "SHare" Backend Job Server (Refactored)
 *
 * --- (FIX) LIVE JOBSTATUS FEATURE ---
 * 1. initialize_shm() now zeros out the new
 * timestamp fields.
 * 2. scan_for_queued_job() now sets job->start_time
 * when a job is moved to STATUS_RUNNING.
 *
 * --- FIXES (Bugs A & C) ---
 * 1. (Bug C) scan_for_queued_job() now starts all
 * available jobs in one pass, up to the
 * MAX_CONCURRENT_JOBS limit.
 * 2. (Bug A) Fixed a race condition with SIGCHLD
 * by using a volatile flag (sigchld_received)
 * and a safe reap function
 * (reap_children_and_update_count).
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

// --- (FIX Bug A) Flag for SIGCHLD handler ---
volatile sig_atomic_t sigchld_received = 0;

// --- Function Prototypes ---
void initialize_shm();
void main_server_loop();
void scan_for_queued_job();
void check_for_shutdown_msg();
void shutdown_handler(int signal);
void sigchld_handler(int signal); // (Feature 2)
// --- (FIX Bug A) New function to safely reap children ---
void reap_children_and_update_count();


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
 * Attaches to the shared memory segment and initializes it.
 * --- (FIX) MODIFIED to zero new time fields ---
 */
void initialize_shm() {
    // --- CRITICAL SECTION ---
    sem_lock(semid);
    
    shm_ptr->job_counter = 101; // Start job IDs from 101
    for (int i = 0; i < MAX_JOBS; i++) {
        shm_ptr->jobs[i].status = STATUS_EMPTY;
        shm_ptr->jobs[i].job_id = 0;
        shm_ptr->jobs[i].pid = 0;
        // --- (FIX) Initialize new fields ---
        shm_ptr->jobs[i].submit_time = 0;
        shm_ptr->jobs[i].start_time = 0;
        shm_ptr->jobs[i].end_time = 0;
    }
    
    sem_unlock(semid);
    // --- END CRITICAL SECTION ---
}

/**
 * main_server_loop
 *
 * The daemon's primary loop.
 * --- (FIX Bug A) MODIFIED to check signal flag ---
 */
void main_server_loop() {
    while (server_running) {
        check_for_shutdown_msg();
        if (!server_running) break;
        
        // --- (FIX Bug A) Check flag and reap children ---
        // Do this *before* scanning, so we have an
        // accurate running_job_count.
        if (sigchld_received) {
            sigchld_received = 0; // Reset flag
            reap_children_and_update_count();
        }
        
        scan_for_queued_job();
        
        sleep(1); // Poll every 1 second
    }
}

/**
 * check_for_shutdown_msg
 *
 * Checks the message queue for a shutdown command (non-blocking).
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
 * --- (FIX) MODIFIED to set start_time ---
 * --- (FIX Bug C) MODIFIED to start jobs in parallel ---
 * --- (FIX Bug A) REMOVED all sigprocmask calls ---
 */
void scan_for_queued_job() {
    // --- CRITICAL SECTION (for SHM) ---
    sem_lock(semid);

    // (Feature 2) Check if we are at capacity *before* starting
    if (running_job_count >= MAX_CONCURRENT_JOBS) {
        sem_unlock(semid);
        return; // At capacity, try again later
    }

    // Find all available queued jobs
    for (int i = 0; i < MAX_JOBS; i++) {
        if (shm_ptr->jobs[i].status == STATUS_QUEUED) {
            // Mark it as RUNNING immediately
            shm_ptr->jobs[i].status = STATUS_RUNNING;
            
            // --- (FIX) Set the start time ---
            shm_ptr->jobs[i].start_time = time(NULL);
            
            // Delegate execution to the job module
            // job_start handles the fork, P2 logic, and P1 PID update.
            job_start(i, shm_ptr, semid);
            
            // (Feature 2) Increment the running job count
            running_job_count++;
            
            // --- (FIX Bug C) ---
            // Check if we just hit the limit. If so,
            // stop scanning for more jobs.
            if (running_job_count >= MAX_CONCURRENT_JOBS) {
                break; // At capacity, stop scanning
            }
        }
    }

    sem_unlock(semid);
    // --- END CRITICAL SECTION (for SHM) ---
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
 * --- (FIX Bug A) MODIFIED for race condition ---
 * This handler is now async-signal-safe. It *only*
 * sets a flag. The main loop will do the real work.
 */
void sigchld_handler(int signal) {
    (void)signal; // Unused
    sigchld_received = 1;
}

/**
 * --- (FIX Bug A) NEW FUNCTION ---
 * reap_children_and_update_count
 *
 * This function is called from the main loop, so it
 * can safely lock the semaphore and modify shared state.
 */
void reap_children_and_update_count() {
    pid_t pid;
    
    // --- CRITICAL SECTION ---
    // We lock the semaphore to protect running_job_count
    sem_lock(semid);
    
    // Reap all finished children without blocking
    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
        // A P2 process finished (or was killed)
        if (running_job_count > 0) {
             running_job_count--;
        }
    }
    
    sem_unlock(semid);
    // --- END CRITICAL SECTION ---
}