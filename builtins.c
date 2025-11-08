/**
 * builtins.c
 *
 * Implementation of all the shell's built-in commands.
 */
#include "builtins.h"

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

        printf("[SHell] Job %d submitted: \"%s\"\n", job->job_id, job->cmd);
    }

    sem_unlock(semid);
    // --- END CRITICAL SECTION ---
}

/**
 * do_jobstatus
 *
 * Reads the entire job table from shared memory and prints it.
 */
void do_jobstatus(int semid, struct sh_job_queue *shm_ptr) {
    const char* status_str[] = {"EMPTY", "QUEUED", "RUNNING", "DONE", "FAILED"};
    char cmd_preview[31]; // 30 chars + null

    // --- CRITICAL SECTION ---
    sem_lock(semid);
    
    printf("\n--- SHare Job Status ---\n");
    printf("[ID]\t[PID]\t[STATUS]\t[COMMAND]\n");
    printf("----------------------------------------------------------\n");

    for (int i = 0; i < MAX_JOBS; i++) {
        if (shm_ptr->jobs[i].status != STATUS_EMPTY) {
            struct sh_job *job = &shm_ptr->jobs[i];
            
            // Create a truncated command for clean printing
            strncpy(cmd_preview, job->cmd, 30);
            if (strlen(job->cmd) > 30) {
                strcpy(cmd_preview + 27, "...");
            } else {
                cmd_preview[strlen(job->cmd)] = '\0';
            }

            printf("%d\t%d\t%-7s\t\"%s\"\n",
                job->job_id,
                job->pid,
                status_str[job->status],
                cmd_preview
            );
        }
    }
    printf("----------------------------------------------------------\n");

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
 * do_myls
 *
 * Implements 'myls -1' using opendir/readdir.
 */
void do_myls() {
    DIR *d;
    struct dirent *dir;
    
    d = opendir("."); // Open current directory
    if (d == NULL) {
        perror("opendir");
        return;
    }

    while ((dir = readdir(d)) != NULL) {
        // Simple 'ls -1' doesn't show hidden files (starting with '.')
        if (dir->d_name[0] != '.') {
            printf("%s\n", dir->d_name);
        }
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
 * do_external_command
 *
 * A simple fork/exec/wait executor for any command that
 * is not a built-in.
 */
void do_external_command(char **tokens) {
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
        waitpid(pid, NULL, 0); // Wait for the child to complete
    }
}