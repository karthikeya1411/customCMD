/**
 * builtins.c
 *
 * Implementation of all the shell's built-in commands.
 *
 * --- (FIX) LIVE JOBSTATUS FEATURE ---
 * 1. do_submit() now sets the job->submit_time.
 * 2. do_jobkill() now sets the job->end_time.
 * 3. do_jobstatus() is completely rewritten to display
 * the new timestamps in a formatted way.
 * 4. Added a new static helper format_time().
 *
 * --- (FIX Bug D) ---
 * 1. do_jobkill() now sends the signal to -p2_pid
 * (the negative PID) to kill the entire
 * process group, preventing orphaned P3 processes.
 */
#include "builtins.h"

// --- (Feature 3) Global list for local background jobs ---
struct local_job local_job_list[MAX_LOCAL_JOBS];
int local_job_count = 0;


/**
 * (Feature 3) init_local_jobs
 * Initializes the local job list.
 */
void init_local_jobs() {
    for (int i = 0; i < MAX_LOCAL_JOBS; i++) {
        local_job_list[i].pid = 0;
        local_job_list[i].running = 0;
        local_job_list[i].cmd[0] = '\0';
    }
    local_job_count = 0;
}

/**
 * (Feature 3) add_local_job (Static Helper)
 * Adds a new locally-run background job to the list.
 */
static void add_local_job(pid_t pid, char **tokens) {
    if (local_job_count >= MAX_LOCAL_JOBS) {
        fprintf(stderr, "[SHell] Local job list is full.\n");
        return;
    }
    
    int slot = -1;
    for (int i = 0; i < MAX_LOCAL_JOBS; i++) {
        if (local_job_list[i].running == 0) {
            slot = i;
            break;
        }
    }
    
    if (slot == -1) slot = local_job_count; // Should be found, but fallback

    local_job_list[slot].pid = pid;
    local_job_list[slot].running = 1;
    
    // Reconstruct the command string for 'jobs'
    local_job_list[slot].cmd[0] = '\0';
    for (int i = 0; tokens[i] != NULL; i++) {
        strncat(local_job_list[slot].cmd, tokens[i], MAX_CMD_LEN - strlen(local_job_list[slot].cmd) - 1);
        if (tokens[i+1] != NULL) {
            strncat(local_job_list[slot].cmd, " ", MAX_CMD_LEN - strlen(local_job_list[slot].cmd) - 1);
        }
    }
    
    local_job_count++;
    printf("[SHell] Started local job (PID %d): %s\n", pid, local_job_list[slot].cmd);
}

/**
 * (Feature 3) remove_local_job
 * Marks a local job as 'done' after it has been reaped.
 * Called by the SIGCHLD handler in shell.c
 */
void remove_local_job(pid_t pid) {
    for (int i = 0; i < MAX_LOCAL_JOBS; i++) {
        if (local_job_list[i].pid == pid) {
            local_job_list[i].running = 0;
            local_job_count--;
            return;
        }
    }
}


/**
 * find_job_by_id (Static Helper)
 *
 * Searches the job table for a specific job ID.
 * NOTE: This function MUST be called *inside* a sem_lock/sem_unlock block.
 *
 * Returns:
 * - The array index (0 to MAX_JOBS-1) if found
 * - -1 if not found
 */
static int find_job_by_id(int job_id, struct sh_job_queue *shm_ptr) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (shm_ptr->jobs[i].status != STATUS_EMPTY &&
            shm_ptr->jobs[i].job_id == job_id)
        {
            return i;
        }
    }
    return -1; // Not found
}

/**
 * do_submit
 *
 * Parses the 'submit "..."' command, finds an empty job slot
 * in shared memory, and populates it.
 * --- (FIX) MODIFIED to set submit_time ---
 */
void do_submit(char *line, int semid, struct sh_job_queue *shm_ptr) {
    // Custom parsing for 'submit "command string"'
    char *cmd_start = strchr(line, '"');
    char *cmd_end = strrchr(line, '"');

    if (cmd_start == NULL || cmd_end == NULL || cmd_start == cmd_end) {
        fprintf(stderr, "Usage: submit \"<command>\"\n");
        return;
    }
    
    // Extract the command
    cmd_start++; // Move past the first quote
    char cmd_buf[MAX_CMD_LEN];
    size_t len = cmd_end - cmd_start;
    
    if (len >= MAX_CMD_LEN) {
        fprintf(stderr, "Error: Command is too long (max %d chars).\n", MAX_CMD_LEN - 1);
        return;
    }
    
    strncpy(cmd_buf, cmd_start, len);
    cmd_buf[len] = '\0';

    // --- CRITICAL SECTION ---
    sem_lock(semid);

    // Find an empty slot
    int slot = -1;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (shm_ptr->jobs[i].status == STATUS_EMPTY) {
            slot = i;
            break;
        }
    }

    if (slot == -1) {
        fprintf(stderr, "Error: Job queue is full.\n");
    } else {
        // Found a slot, populate the job
        struct sh_job *job = &shm_ptr->jobs[slot];
        
        job->job_id = shm_ptr->job_counter++;
        job->pid = 0;
        job->status = STATUS_QUEUED;
        strcpy(job->cmd, cmd_buf);
        sprintf(job->log_file, "/tmp/job-%d.log", job->job_id);
        sprintf(job->fifo_file, "/tmp/job-%d.fifo", job->job_id);

        // --- (FIX) Set timestamps ---
        job->submit_time = time(NULL);
        job->start_time = 0; // Not started yet
        job->end_time = 0;   // Not finished yet

        printf("[SHell] Job %d submitted: \"%s\"\n", job->job_id, job->cmd);
    }

    sem_unlock(semid);
    // --- END CRITICAL SECTION ---
}

/**
 * --- (FIX) NEW HELPER FUNCTION ---
 * format_time
 *
 * Helper to format time_t into "HH:MM:SS" or "---" if time is 0.
 */
static void format_time(time_t t, char *buf, size_t buf_size) {
    if (t == 0) {
        snprintf(buf, buf_size, "   ---    ");
    } else {
        // Use localtime_r for thread-safety, though not strictly needed here
        struct tm tm_info;
        localtime_r(&t, &tm_info);
        strftime(buf, buf_size, "%H:%M:%S", &tm_info);
    }
}

/**
 * do_jobstatus
 *
 * Reads the entire job table from shared memory and prints it.
 * --- (FIX) HEAVILY MODIFIED to print timestamps ---
 */
void do_jobstatus(int semid, struct sh_job_queue *shm_ptr) {
    // (Feature 1) Added "KILLED" to status strings
    const char* status_str[] = {"EMPTY", "QUEUED", "RUNNING", "DONE", "FAILED", "KILLED"};
    char cmd_preview[31]; // 30 chars + null
    char submit_buf[16], start_buf[16], end_buf[16]; // Buffers for time strings
    int active_jobs = 0;

    // --- CRITICAL SECTION ---
    sem_lock(semid);
    
    printf("\n--- SHare Job Status ---\n");
    printf("[ID]\t[PID]\t[STATUS]\t[SUBMIT]\t[START]\t\t[END]\t\t[COMMAND]\n");
    printf("--------------------------------------------------------------------------------------------------\n");

    for (int i = 0; i < MAX_JOBS; i++) {
        if (shm_ptr->jobs[i].status != STATUS_EMPTY) {
            active_jobs++;
            struct sh_job *job = &shm_ptr->jobs[i];
            
            // Create a truncated command for clean printing
            strncpy(cmd_preview, job->cmd, 30);
            if (strlen(job->cmd) > 30) {
                strcpy(cmd_preview + 27, "...");
            } else {
                cmd_preview[strlen(job->cmd)] = '\0';
            }
            
            // Format timestamps
            format_time(job->submit_time, submit_buf, sizeof(submit_buf));
            format_time(job->start_time, start_buf, sizeof(start_buf));
            format_time(job->end_time, end_buf, sizeof(end_buf));

            printf("%d\t%d\t%-7s\t%s\t%s\t%s\t\"%s\"\n",
                job->job_id,
                job->pid,
                status_str[job->status],
                submit_buf,
                start_buf,
                end_buf,
                cmd_preview
            );
        }
    }
    
    if (active_jobs == 0) {
        printf("No active or completed jobs in the queue.\n");
    }
    printf("--------------------------------------------------------------------------------------------------\n");

    sem_unlock(semid);
    // --- END CRITICAL SECTION ---
}


/**
 * do_jobstream
 *
 * Finds the FIFO path for a job, opens it, and reads
 * from it until EOF, printing the data to stdout.
 */
void do_jobstream(char *job_id_str, int semid, struct sh_job_queue *shm_ptr) {
    int job_id = atoi(job_id_str);
    if (job_id == 0) {
        fprintf(stderr, "Invalid job ID.\n");
        return;
    }

    char fifo_path[64];
    int job_found = 0;

    // --- CRITICAL SECTION ---
    sem_lock(semid);
    
    int job_index = find_job_by_id(job_id, shm_ptr);
    
    if (job_index != -1) {
        // Copy the path *inside the lock*
        strcpy(fifo_path, shm_ptr->jobs[job_index].fifo_file);
        job_found = 1;
    }

    sem_unlock(semid);
    // --- END CRITICAL SECTION ---

    if (!job_found) {
        fprintf(stderr, "Error: Job ID %d not found.\n", job_id);
        return;
    }

    printf("[SHell] Tapping into live stream for Job %d (%s)...\n", job_id, fifo_path);

    // Open the FIFO for reading.
    int fifo_fd = open(fifo_path, O_RDONLY);
    if (fifo_fd == -1) {
        perror("Error opening FIFO");
        fprintf(stderr, "Hint: Has the job started running yet?\n");
        return;
    }

    // Loop, reading from FIFO and writing to our STDOUT
    char buffer[1024];
    ssize_t n_read;
    while ((n_read = read(fifo_fd, buffer, sizeof(buffer))) > 0) {
        write(STDOUT_FILENO, buffer, n_read);
    }

    close(fifo_fd);
    printf("\n[SHell] Stream for Job %d finished.\n", job_id);
}


/**
 * do_joblog
 *
 * Finds the log file path for a job and executes 'cat'
 * on it to display its contents.
 */
void do_joblog(char *job_id_str, int semid, struct sh_job_queue *shm_ptr) {
    int job_id = atoi(job_id_str);
    if (job_id == 0) {
        fprintf(stderr, "Invalid job ID.\n");
        return;
    }

    char log_path[64];
    int job_found = 0;
    int job_status = 0;

    // --- CRITICAL SECTION ---
    sem_lock(semid);
    
    int job_index = find_job_by_id(job_id, shm_ptr);
    
    if (job_index != -1) {
        strcpy(log_path, shm_ptr->jobs[job_index].log_file);
        job_status = shm_ptr->jobs[job_index].status;
        job_found = 1;
    }

    sem_unlock(semid);
    // --- END CRITICAL SECTION ---

    if (!job_found) {
        fprintf(stderr, "Error: Job ID %d not found.\n", job_id);
        return;
    }

    if (job_status < STATUS_DONE) {
        printf("[SHell] Job %d is not yet DONE. Log may be incomplete.\n", job_id);
    }
    printf("[SHell] Displaying log for Job %d (%s):\n", job_id, log_path);
    printf("--- BEGIN LOG ---\n");

    // Fork/exec 'cat' on the log file, as per the PDF
    pid_t pid = fork();
    if (pid == 0) {
        // Child
        execlp("cat", "cat", log_path, NULL);
        perror("execlp(cat)"); // Only runs if exec fails
        exit(1);
    } else if (pid > 0) {
        // Parent
        waitpid(pid, NULL, 0); // Wait for 'cat' to finish
    } else {
        perror("fork (do_joblog)");
    }
    printf("--- END LOG ---\n");
}


/**
 * (Feature 1) do_jobkill
 *
 * --- (FIX) MODIFIED to set end_time ---
 * --- (FIX Bug D) ---
 * Sends SIGKILL to the entire process group.
 */
void do_jobkill(char *job_id_str, int semid, struct sh_job_queue *shm_ptr) {
    int job_id = atoi(job_id_str);
    if (job_id == 0) {
        fprintf(stderr, "Invalid job ID.\n");
        return;
    }
    
    pid_t p2_pid = 0;
    int job_found = 0;

    // --- CRITICAL SECTION ---
    sem_lock(semid);

    int job_index = find_job_by_id(job_id, shm_ptr);
    if (job_index == -1) {
        fprintf(stderr, "Error: Job ID %d not found.\n", job_id);
    } else {
        struct sh_job *job = &shm_ptr->jobs[job_index];
        if (job->status != STATUS_RUNNING) {
            fprintf(stderr, "Error: Job %d is not currently running.\n", job_id);
        } else {
            p2_pid = job->pid;
            job_found = 1;
            // Mark it as KILLED immediately.
            job->status = STATUS_KILLED;
            
            // --- (FIX) Set the end time ---
            job->end_time = time(NULL);
        }
    }
    
    sem_unlock(semid);
    // --- END CRITICAL SECTION ---
    
    if (job_found && p2_pid > 0) {
        // --- (FIX Bug D) ---
        // Send signal to the *negative* PID. This kills the
        // entire process group (P2 and P3).
        if (kill(-p2_pid, SIGKILL) == 0) {
            printf("[SHell] Kill signal sent to Job %d (Process Group %d).\n", job_id, p2_pid);
        } else {
            perror("kill");
        }
    }
}


/**
 * (Feature 3) do_jobs
 *
 * Lists all locally-run background jobs.
 */
void do_jobs() {
    printf("\n--- Local Background Jobs ---\n");
    printf("[PID]\t[STATUS]\t[COMMAND]\n");
    printf("----------------------------------------------------------\n");
    
    int count = 0;
    for (int i = 0; i < MAX_LOCAL_JOBS; i++) {
        if (local_job_list[i].running) {
            printf("%d\tRunning\t\t%s\n", local_job_list[i].pid, local_job_list[i].cmd);
            count++;
        }
    }
    
    if (count == 0) {
        printf("No local background jobs.\n");
    }
    printf("----------------------------------------------------------\n");
}

/**
 * (Feature 3) do_kill_local
 *
 * Sends a SIGKILL to a locally-run background job.
 */
void do_kill_local(char *pid_str) {
    pid_t pid = atoi(pid_str);
    if (pid == 0) {
        fprintf(stderr, "Invalid PID.\n");
        return;
    }

    int found = 0;
    for (int i = 0; i < MAX_LOCAL_JOBS; i++) {
        if (local_job_list[i].running && local_job_list[i].pid == pid) {
            found = 1;
            break;
        }
    }

    if (!found) {
        fprintf(stderr, "Error: No running local job with PID %d.\n", pid);
        return;
    }
    
    if (kill(pid, SIGKILL) == 0) {
        printf("[SHell] Kill signal sent to local job (PID %d).\n", pid);
        // The SIGCHLD handler will reap it and call remove_local_job.
    } else {
        perror("kill (local)");
    }
}


/**
 * do_shutdown
 *
 * Sends the shutdown message to the server via the Message Queue.
 */
void do_shutdown(int msgid) {
    struct msg_buf msg;
    msg.mtype = MSG_Q_SHUTDOWN;
    // No payload needed, mtext[0] is fine

    if (msgsnd(msgid, &msg, 0, 0) == -1) {
        perror("msgsnd (shutdown)");
        fprintf(stderr, "Error: Failed to send shutdown signal.\n");
    } else {
        printf("[SHell] Shutdown signal sent to SHare server.\n");
    }
}


/**
 * (Refactor 1) do_myls
 *
 * Implements 'myls' using opendir/readdir and stat()
 * to show file permissions and size.
 */
void do_myls() {
    DIR *d;
    struct dirent *dir;
    struct stat st;
    char path_buf[512];
    
    d = opendir("."); // Open current directory
    if (d == NULL) {
        perror("opendir");
        return;
    }

    printf("[PERMS]\t\t[SIZE]\t[NAME]\n");
    printf("----------------------------------------------------------\n");

    while ((dir = readdir(d)) != NULL) {
        // Don't show hidden files (starting with '.')
        if (dir->d_name[0] == '.') {
            continue;
        }
        
        // We must stat the file to get its info
        // Note: stat() may fail on symlinks if not careful,
        // but this simple version should be fine.
        snprintf(path_buf, sizeof(path_buf), "./%s", dir->d_name);
        
        if (stat(path_buf, &st) == -1) {
            perror(dir->d_name);
            continue;
        }
        
        // 1. Print permissions (mode)
        printf( (S_ISDIR(st.st_mode)) ? "d" : "-");
        printf( (st.st_mode & S_IRUSR) ? "r" : "-");
        printf( (st.st_mode & S_IWUSR) ? "w" : "-");
        printf( (st.st_mode & S_IXUSR) ? "x" : "-");
        printf( (st.st_mode & S_IRGRP) ? "r" : "-");
        printf( (st.st_mode & S_IWGRP) ? "w" : "-");
        printf( (st.st_mode & S_IXGRP) ? "x" : "-");
        printf( (st.st_mode & S_IROTH) ? "r" : "-");
        printf( (st.st_mode & S_IWOTH) ? "w" : "-");
        printf( (st.st_mode & S_IXOTH) ? "x" : "-");

        // 2. Print size
        printf("\t%10ld", st.st_size);
        
        // 3. Print name
        printf("\t%s\n", dir->d_name);
    }

    closedir(d);
}


/**
 * do_cd
 *
 * Implements the 'cd' built-in command.
 */
void do_cd(char *path) {
    if (path == NULL) {
        // 'cd' with no args, go to HOME
        path = getenv("HOME");
    }
    
    if (chdir(path) != 0) {
        perror("cd");
    }
}


/**
 * (Feature 3) do_external_command
 *
 * A fork/exec executor.
 * - If background == 0, it waits for the child.
 * - If background == 1, it adds the child to the
 * local job list and returns immediately.
 */
void do_external_command(char **tokens, int background) {
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork (external_command)");
        return;
    }

    if (pid == 0) {
        // --- Child Process ---
        if (execvp(tokens[0], tokens) == -1) {
            perror("execvp");
            fprintf(stderr, "Error: Command not found: %s\n", tokens[0]);
            exit(1);
        }
    } else {
        // --- Parent Process ---
        if (background) {
            // (Feature 3) Add to local job list and return
            add_local_job(pid, tokens);
        } else {
            // Wait for the child to complete
            waitpid(pid, NULL, 0);
        }
    }
}