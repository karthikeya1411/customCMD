/**
 * job.c
 *
 * Implementation of the Job Executor module.
 * Contains the P1 fork, P2 Job Manager, and P3 Executor logic.
 */
#include "job.h"

// --- Private Function Prototypes ---
static void run_job_manager(int job_index, int semid);
static void run_executor(const char* cmd, int p[2]);

/**
 * job_start (P1 - The Daemon's action)
 *
 * This function is called by the daemon (P1) *while it holds the semaphore lock*.
 * It forks to create the Job Manager (P2).
 * The daemon (P1) stores P2's PID in shared memory and returns.
 * The Job Manager (P2) detaches and calls run_job_manager().
 */
void job_start(int job_index, struct sh_job_queue *shm_ptr, int semid) {
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork (job_start)");
        shm_ptr->jobs[job_index].status = STATUS_FAILED; // Mark as failed
        return;
    }

    if (pid == 0) {
        // --- CHILD (P2 - Job Manager) ---
        
        // Detach from daemon's shared memory pointer
        shmdt(shm_ptr); 
        
        // This child process (P2) will now run the job
        // It will create P3, manage I/O, and update SHM when done.
        run_job_manager(job_index, semid);
        
        // P2 is done, it must exit.
        exit(0); 
    }

    if (pid > 0) {
        // --- PARENT (P1 - Daemon) ---
        // Store the Job Manager's (P2) PID in the job table
        shm_ptr->jobs[job_index].pid = pid;
        // P1 returns to the scan loop. The lock is released in scan_for_queued_job()
    }
}


/**
 * run_job_manager (P2 - The Job Manager)
 *
 * This is the "Tee Executor" logic from the project guide.
 * This code is run by the P2 process.
 * This function is STATIC and only called from job_start.
 */
static void run_job_manager(int job_index, int semid) {
    // P2 must re-attach to shared memory
    // Note: We don't need to check for errors here, as P1 already created it.
    int local_shmid = shmget(ftok(KEY_PATH, PROJ_ID), 0, 0666);
    struct sh_job_queue *local_shm_ptr = (struct sh_job_queue *)shmat(local_shmid, NULL, 0);

    // Get a local pointer to our job
    struct sh_job *job = &local_shm_ptr->jobs[job_index];

    // 1. Create the pipe (for P3 to write to P2)
    int p[2];
    if (pipe(p) == -1) {
        perror("pipe");
        // We can't update SHM easily here, just exit.
        shmdt(local_shm_ptr);
        exit(1);
    }
    
    // 2. Create the FIFO (for P2 to write to 'jobstream' client)
    mkfifo(job->fifo_file, 0666); // Ignore error if it exists

    // 3. Open the log file
    int log_fd = open(job->log_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (log_fd == -1) {
        perror("open log_file");
        // No log file, but we can still stream. Continue.
    }

    // 4. Fork to create the Executor (P3)
    pid_t executor_pid = fork();
    if (executor_pid < 0) {
        perror("fork (run_job_manager)");
        close(p[0]); close(p[1]);
        if (log_fd != -1) close(log_fd);
        shmdt(local_shm_ptr);
        exit(1);
    }

    if (executor_pid == 0) {
        // --- GRANDCHILD (P3 - Executor) ---
        run_executor(job->cmd, p);
        // run_executor only returns on error
        exit(1);
    }

    // --- PARENT (P2 - Job Manager) ---
    // 5. P2's "Tee" logic
    close(p[1]); // P2 only *reads* from the pipe

    // (FIX) Open the FIFO for reading in non-blocking mode first.
    // This acts as a "keep-alive" and prevents the O_WRONLY
    // open from blocking, allowing the job to run even if
    // no client is connected via 'jobstream'.
    int fifo_dummy_rd_fd = open(job->fifo_file, O_RDONLY | O_NONBLOCK);
    if (fifo_dummy_rd_fd == -1) {
        perror("open (fifo_dummy_rd_fd)"); 
    }

    // Open the FIFO for writing. This will no longer block
    // because we are holding the read-end open ourselves.
    int fifo_fd = open(job->fifo_file, O_WRONLY);
    if (fifo_fd == -1) {
        perror("open fifo_file");
        // FIFO failed, but we must still log. Continue.
    }
    
    char buffer[1024];
    ssize_t n_read;
    
    // Read from P3's stdout/stderr (via pipe)
    while ((n_read = read(p[0], buffer, sizeof(buffer))) > 0) {
        // Write to log file
        if (log_fd != -1) {
            write(log_fd, buffer, n_read);
        }
        // Write to FIFO (for live streaming)
        if (fifo_fd != -1) {
            write(fifo_fd, buffer, n_read);
        }
    }
    
    // 6. Cleanup P2's file descriptors
    close(p[0]);
    if (log_fd != -1) close(log_fd);
    if (fifo_fd != -1) close(fifo_fd);
    // (FIX) Close the dummy read descriptor
    if (fifo_dummy_rd_fd != -1) close(fifo_dummy_rd_fd);

    // 7. Wait for P3 (Executor) to finish
    int exec_status;
    waitpid(executor_pid, &exec_status, 0);

    // 8. Update job status in Shared Memory
    // --- CRITICAL SECTION ---
    sem_lock(semid);
    
    // Check if P3 exited normally and with status 0
    if (WIFEXITED(exec_status) && WEXITSTATUS(exec_status) == 0) {
        job->status = STATUS_DONE;
    } else {
        job->status = STATUS_FAILED;
    }
    
    sem_unlock(semid);
    // --- END CRITICAL SECTION ---

    // 9. Clean up the FIFO file (the log file remains)
    unlink(job->fifo_file);

    // 10. Detach from shared memory and exit
    shmdt(local_shm_ptr);
}


/**
 * run_executor (P3 - The Executor)
 *
 * This code is run by the P3 (grandchild) process.
 * Its only job is to redirect its output and execute the command.
 * This function is STATIC and only called from run_job_manager.
 */
static void run_executor(const char* cmd, int p[2]) {
    // Close the read end of the pipe
    close(p[0]);

    // Redirect STDOUT to the pipe's write end
    if (dup2(p[1], STDOUT_FILENO) == -1) {
        perror("dup2(stdout)");
        exit(1);
    }
    // Redirect STDERR to the pipe's write end
    if (dup2(p[1], STDERR_FILENO) == -1) {
        perror("dup2(stderr)");
        exit(1);
    }

    // Close the original pipe descriptor (it's now duplicated)
    close(p[1]);

    // Use /bin/sh -c to execute the command string
    // This correctly handles pipelines, quotes, and complex commands.
    char* argv[] = {"/bin/sh", "-c", (char*)cmd, NULL};
    execvp(argv[0], argv);

    // execvp only returns if an error occurred
    perror("execvp");
    exit(1);
}