/**
 * shell.c
 *
 * The "SHell" Frontend Client (Refactored)
 *
 * This is the user-facing interactive REPL (Read-Eval-Print Loop).
 * Its responsibilities are now simpler:
 * 1.  Attach to IPC resources (via ipc.c).
 * 2.  Run the REPL.
 * 3.  Parse user input.
 * 4.  Dispatch commands to the 'builtins' module.
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


/**
 * Main shell entry point
 */
int main() {
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

    // 3. Run the Read-Eval-Print Loop
    repl_loop();

    // 4. Detach from shared memory before exiting
    if (shm_ptr) {
        shmdt(shm_ptr);
    }
    
    return 0;
}


/**
 * repl_loop
 *
 * The main Read-Eval-Print Loop for the shell.
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
 */
void execute_command(int n_tokens, char **tokens) {
    char *cmd = tokens[0];

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
        if (n_tokens > 1 && strcmp(tokens[1], "-1") == 0) {
            do_myls();
        } else {
            fprintf(stderr, "Usage: myls -1\n");
        }
    } else {
        // Not a built-in, execute as external command
        do_external_command(tokens);
    }
}