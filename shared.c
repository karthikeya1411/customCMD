/**
 * shared.c
 *
 * The "SHare" Backend Job Server.
 *
 * This daemon monitors the shared memory queue. When a new job
 * is submitted (status=QUEUED), it forks a "Job Manager" (P2)
 * process to handle its execution, up to a concurrent limit.
 * It also reaps finished P2 children and handles shutdown requests.
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

// --- Global count of running jobs ---
volatile sig_atomic_t running_job_count = 0;

// --- Flag for a safe SIGCHLD handler ---
// The handler sets this to 1; the main loop resets it to 0.
volatile sig_atomic_t sigchld_received = 0;

// --- Function Prototypes ---
void initialize_shm();
void main_server_loop();
void scan_for_queued_job();
void check_for_shutdown_msg();
void shutdown_handler(int signal);
void sigchld_handler(int signal);
void reap_children_and_update_count();


/**
 * main
 * Server entry point.
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
    
    // 3. Set up signal handlers
    signal(SIGTERM, shutdown_handler); // Graceful shutdown
    signal(SIGINT, shutdown_handler);  // Graceful shutdown
    signal(SIGCHLD, sigchld_handler);  // To reap finished P2 children
    
    // 4. Initialize shared memory
    shm_ptr = ipc_get_shm_ptr(shmid);
    if (shm_ptr == (void*)-1) {
        // We are a daemon, so this stderr won't be seen,
        // but we must log the error and clean up.
        ipc_cleanup_server(shmid, semid, msgid);
        exit(1);
    }
    initialize_shm();
    
    // 5. Run the main loop
    main_server_loop();

    // 6. Loop has exited (server_running = 0), perform cleanup
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
 * Zeros out all job slots and timestamps.
 */
void initialize_shm() {
    // --- CRITICAL SECTION ---
    sem_lock(semid);
    
    shm_ptr->job_counter = 101; // Start job IDs from 101
    for (int i = 0; i < MAX_JOBS; i++) {
        shm_ptr->jobs[i].status = STATUS_EMPTY;
        shm_ptr->jobs[i].job_id = 0;
        shm_ptr->jobs[i].pid = 0;
        // Initialize timestamp fields
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
 * The daemon's primary loop. It checks for shutdown messages,
 * reaps finished children (if the signal flag is set), and
 * scans for new jobs to start.
 */
void main_server_loop() {
    while (server_running) {
        // Check for shutdown message from a client
        check_for_shutdown_msg();
        if (!server_running) break;
        
        // Check if the SIGCHLD handler has set the flag
        // Do this *before* scanning, so we have an
        // accurate running_job_count.
        if (sigchld_received) {
            sigchld_received = 0; // Reset flag
            reap_children_and_update_count();
        }
        
        // Look for new jobs to run
        scan_for_queued_job();
        
        sleep(1); // Poll every 1 second
    }
}

/**
 * check_for_shutdown_msg
 *
 * Checks the message queue for a shutdown command (non-blocking).
 * If a message is found, it sets the global server_running flag to 0.
 */
void check_for_shutdown_msg() {
    struct msg_buf msg;
    
    // Check for a message of type MSG_Q_SHUTDOWN without blocking
    if (msgrcv(msgid, &msg, sizeof(msg.mtext), MSG_Q_SHUTDOWN, IPC_NOWAIT) > 0) {
        server_running = 0;
    }
}

/**
 * scan_for_queued_job
 *
 * Scans the shared memory for jobs with STATUS_QUEUED.
 * If the server is not at capacity, it starts one or more jobs
 * until the MAX_CONCURRENT_JOBS limit is reached.
 */
void scan_for_queued_job() {
    // --- CRITICAL SECTION (for SHM) ---
    sem_lock(semid);

    // Check if we are at capacity *before* starting
    if (running_job_count >= MAX_CONCURRENT_JOBS) {
        sem_unlock(semid);
        return; // At capacity, try again later
    }

    // Find all available queued jobs
    for (int i = 0; i < MAX_JOBS; i++) {
        if (shm_ptr->jobs[i].status == STATUS_QUEUED) {
            // Mark it as RUNNING immediately
            shm_ptr->jobs[i].status = STATUS_RUNNING;
            
            // Set the start time
            shm_ptr->jobs[i].start_time = time(NULL);
            
            // Delegate execution to the job module
            // job_start handles the fork, P2 logic, and P1 PID update.
            job_start(i, shm_ptr, semid);
            
            // Increment the running job count
            running_job_count++;
            
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
 * Sets the global flag to terminate the main loop.
 */
void shutdown_handler(int signal) {
    (void)signal; // Unused parameter
    server_running = 0;
}

/**
 * sigchld_handler
 *
 * This handler is async-signal-safe. It *only*
 * sets a flag. The main loop will see this flag
 * and call the real reaping function safely.
 */
void sigchld_handler(int signal) {
    (void)signal; // Unused
    sigchld_received = 1;
}

/**
 * reap_children_and_update_count
 *
 * This function is called from the main loop, *not* the
 * signal handler. It can safely call waitpid() and
 * modify the shared running_job_count inside a semaphore lock.
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