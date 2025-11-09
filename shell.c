/**
 * shell.c
 *
 * The "SHell" Frontend Client.
 *
 * This program acts as the user interface for the SH-are system.
 * It connects to the IPC resources created by the 'shared' daemon
 * and provides a Read-Eval-Print Loop (REPL) for submitting
 * and managing jobs.
 *
 * It can also run in "non-interactive" mode. For example,
 * "./shell jobstatus" will execute the 'jobstatus' command
 * once and exit, which is useful for scripts or 'watch'.
 */

#include "sh_share.h"
#include "ipc.h"
#include "builtins.h"

// --- Global variables for IPC identifiers ---
// These are initialized once in main() and passed to builtins
int shmid, semid, msgid;
struct sh_job_queue *shm_ptr = NULL; // Pointer to our shared memory segment

#define MAX_LINE_LEN 512
#define MAX_TOKENS 32

// --- Function Prototypes ---
void repl_loop();
int parse_line(char *line, char **tokens);
void execute_command(int n_tokens, char **tokens);
void reap_local_jobs_handler(int signal); // Handler for local 'cmd &' jobs


/**
 * main
 * Shell entry point.
 * Connects to IPC, sets up handlers, and decides whether
 * to run in interactive (REPL) or non-interactive mode.
 */
int main(int argc, char *argv[]) {
    // 1. Connect to existing IPC resources
    if (ipc_setup_client(&shmid, &semid, &msgid) == -1) {
        fprintf(stderr, "Failed to connect to SHare server. Exiting.\n");
        exit(1);
    }
    
    // 2. Attach to Shared Memory
    shm_ptr = ipc_get_shm_ptr(shmid);
    if (shm_ptr == (void*)-1) {
        fprintf(stderr, "Failed to attach to SHM. Exiting.\n");
        exit(1);
    }

    // Initialize local job list and setup handler
    init_local_jobs();
    signal(SIGCHLD, reap_local_jobs_handler);


    // Check for non-interactive mode
    if (argc > 1) {
        // Arguments provided. Execute them directly and exit.
        // We pass (argc - 1) and (argv + 1) to skip the
        // program name itself ("./shell").
        execute_command(argc - 1, argv + 1);
    } else {
        // No arguments. Run the normal interactive REPL.
        repl_loop();
    }


    // 4. Detach from shared memory before exiting
    if (shm_ptr) {
        shmdt(shm_ptr);
    }
    
    return 0;
}

/**
 * reap_local_jobs_handler
 *
 * Catches SIGCHLD to clean up locally-run background jobs (e.g., 'sleep 10 &').
 * This handler is only for jobs launched by this shell, NOT for
 * jobs submitted to the 'shared' daemon.
 */
void reap_local_jobs_handler(int signal) {
    (void)signal; // Unused
    pid_t pid;
    
    // Reap all finished children without blocking
    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
        printf("\n[SHell] Local job (PID %d) finished.\n", pid);
        remove_local_job(pid); // Mark as reaped
        printf("SNIST-SHell > "); // Re-draw prompt
        fflush(stdout);
    }
}


/**
 * repl_loop
 *
 * The main Read-Eval-Print Loop for the interactive shell.
 */
void repl_loop() {
    char line[MAX_LINE_LEN];
    char *tokens[MAX_TOKENS];
    int n_tokens;

    while (1) {
        printf("SNIST-SHell > ");
        fflush(stdout);

        // Read
        if (fgets(line, sizeof(line), stdin) == NULL) {
            printf("\n");
            break; // EOF (Ctrl+D)
        }
        
        // --- Eval ---
        // Special case for "submit" which has custom parsing
        // (to handle quoted strings)
        if (strncmp(line, "submit ", 7) == 0) {
            do_submit(line, semid, shm_ptr);
            continue; // Skip normal tokenizing
        }

        n_tokens = parse_line(line, tokens);
        if (n_tokens == 0) {
            continue; // Empty line
        }

        // --- Print (Execute) ---
        execute_command(n_tokens, tokens);
    }
}

/**
 * parse_line
 *
 * Simple parser that tokenizes a line by whitespace.
 * Populates the 'tokens' array and returns the token count.
 */
int parse_line(char *line, char **tokens) {
    int n = 0;
    const char *delim = " \t\n\r";
    char *token = strtok(line, delim);
    
    while (token != NULL && n < MAX_TOKENS - 1) {
        tokens[n++] = token;
        token = strtok(NULL, delim);
    }
    tokens[n] = NULL; // Null-terminate the token list (for execvp)
    return n;
}


/**
 * execute_command
 *
 * The main command dispatcher. It checks for built-in commands
 * and calls the appropriate handler from the 'builtins' module.
 * If not a built-in, it executes it as an external command.
 */
void execute_command(int n_tokens, char **tokens) {
    char *cmd = tokens[0];

    // Check for background execution ('&')
    int background = 0;
    if (n_tokens > 0 && strcmp(tokens[n_tokens - 1], "&") == 0) {
        background = 1;
        tokens[n_tokens - 1] = NULL; // Remove '&' from token list
        n_tokens--;
    }

    if (strcmp(cmd, "exit") == 0) {
        exit(0);
    } else if (strcmp(cmd, "cd") == 0) {
        do_cd(tokens[1]);
    } else if (strcmp(cmd, "jobstatus") == 0) {
        do_jobstatus(semid, shm_ptr);
    } else if (strcmp(cmd, "jobstream") == 0) {
        if (n_tokens < 2) {
            fprintf(stderr, "Usage: jobstream <job_id>\n");
        } else {
            do_jobstream(tokens[1], semid, shm_ptr);
        }
    } else if (strcmp(cmd, "joblog") == 0) {
        if (n_tokens < 2) {
            fprintf(stderr, "Usage: joblog <job_id>\n");
        } else {
            do_joblog(tokens[1], semid, shm_ptr);
        }
    } else if (strcmp(cmd, "shutdown_server") == 0) {
        do_shutdown(msgid);
    } else if (strcmp(cmd, "myls") == 0) {
        do_myls();
    } 
    else if (strcmp(cmd, "jobkill") == 0) {
        if (n_tokens < 2) {
            fprintf(stderr, "Usage: jobkill <job_id>\n");
        } else {
            do_jobkill(tokens[1], semid, shm_ptr);
        }
    }
    else if (strcmp(cmd, "jobs") == 0) {
        do_jobs(); // List local background jobs
    }
    else if (strcmp(cmd, "kill") == 0) {
         if (n_tokens < 2) {
            fprintf(stderr, "Usage: kill <pid>\n");
        } else {
            do_kill_local(tokens[1]); // Kill a local background job
        }
    }
    else {
        // Not a built-in, execute as external command
        do_external_command(tokens, background);
    }
}