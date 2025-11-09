

# SHare: A POSIX Job Management System

`SHare` is a client-server application for managing and executing background jobs on a Linux system. It features a daemonized server, an interactive client shell, and robust process management using System V IPC and POSIX signals.

This project demonstrates advanced C programming concepts, including:

  * Multi-process architecture (`fork`, `execvp`, `waitpid`)
  * System V IPC (Shared Memory, Semaphores, and Message Queues)
  * POSIX signal handling (including `SIGCHLD`, `SIGPIPE`, and process groups)
  * I/O redirection with pipes and FIFOs (named pipes)
  * Daemonization

## Features

  * **Daemonized Server:** The `shared` server runs as a background daemon, detached from any terminal.
  * **Interactive Shell:** The `shell` client provides a REPL for submitting and managing jobs.
  * **Live Job Status Dashboard:** A real-time, `watch`-compatible `jobstatus` command shows all jobs with submit, start, and end timestamps.
  * **Live Output Streaming:** The `jobstream` command allows you to tap into the live `stdout`/`stderr` of any running job.
  * **Persistent Logging:** All job output is saved to a permanent log file (`/tmp/job-ID.log`).
  * **Robust Job Killing:** `jobkill` terminates the entire job process group (P2 Manager and P3 Executor), preventing orphan processes.
  * **Concurrency Limiting:** The server manages a `MAX_CONCURRENT_JOBS` limit (e.g., 5 jobs) to prevent system overload.
  * **Local Shell Commands:** The shell also supports standard built-ins like `cd`, `myls`, and local background tasks (`&`).

## Architecture

The system is built on a client-server model communicating via System V IPC.

  * **Server (`shared`):** The daemon (P1) manages the job queue. Its only job is to scan shared memory for `QUEUED` jobs and `fork` a new process (P2) to handle them.
  * **IPC:**
      * **Shared Memory:** A single `shmget` segment holds the main job table (`struct sh_job_queue`).
      * **Semaphores:** A `semget` semaphore provides a mutex lock to ensure all reads/writes to shared memory are atomic and safe.
      * **Message Queues:** A `msgget` queue is used *only* for sending the `shutdown_server` command.

### The "Tee Executor" Process Model

When a job is started, a 3-level process chain is created:

1.  **P1 (The Daemon):** Scans for a `QUEUED` job. When found, it `fork()`s once to create P2.
2.  **P2 (The Job Manager):** This process is responsible for *one* job. It calls `setpgid` to become a new process group leader, ignores `SIGPIPE`, and resets its `SIGCHLD` handler. It then `fork()`s again to create P3.
3.  **P3 (The Executor):** This process `execvp`s the user's command (e.g., `sleep 10`).

### I/O Redirection

  * P3's `stdout` and `stderr` are redirected via a `pipe()` to P2.
  * P2 reads from this pipe and "tees" the output to two destinations simultaneously:
    1.  A persistent log file (e.g., `/tmp/job-101.log`).
    2.  A FIFO/named pipe (e.g., `/tmp/job-101.fifo`) which the `jobstream` command can read from.

## Prerequisites

  * A POSIX-compliant system (Linux, macOS, or WSL on Windows)
  * `gcc` (C compiler)
  * `make` (build tool)
  * `watch` (optional, for the live dashboard)

## How to Build

A `Makefile` is included.

1.  To build both the server and the shell:

    ```bash
    make
    ```

2.  This will create two executables in your directory: `shared` (the server) and `shell` (the client).

3.  To clean up build files:

    ```bash
    make clean
    ```

## How to Run

You will need at least two separate terminal sessions.

### 1\. Terminal 1: Start the Server

Start the `shared` daemon. It will fork itself into the background and you will get your prompt back.

```bash
./shared
```

### 2\. Terminal 2: Run the Interactive Shell

Start the `shell` client to submit and manage jobs.

```bash
./shell
SNIST-SHell >
```

### 3\. Terminal 3 (Optional): Start the Live Dashboard

For the best experience, open a third terminal and run `jobstatus` using `watch`. This will give you a live-updating dashboard of all jobs.

```bash
watch -n 1 ./shell jobstatus
```

*(`watch -n 1` runs the command `./shell jobstatus` every 1 second).*

## Shell Commands

  * `submit "command"`
    Submits a new job to the daemon. The command *must* be in quotes.
    *Example: `submit "sleep 10 && echo 'Job 1 done'"`*

  * `jobstatus`
    Displays the status of all submitted jobs, including Job ID, PID, Status, and Submit/Start/End times.

  * `jobstream <job_id>`
    Taps into the live `stdout`/`stderr` stream of a running job. Press `Ctrl+C` to disconnect from the stream (this does not kill the job).

  * `joblog <job_id>`
    Displays the *entire* persistent log for a job (running or finished).

  * `jobkill <job_id>`
    Kills a running job. This sends `SIGKILL` to the job's entire process group, ensuring both the Job Manager (P2) and the Executor (P3) are terminated.

  * `shutdown_server`
    Sends a message to the daemon, causing it to shut down gracefully and clean up all IPC resources.

  * `myls`
    A custom implementation of `ls -l` using `opendir`/`readdir`.

  * `cd <directory>`
    Changes the shell's current working directory.

  * `jobs` / `kill <pid>` / `cmd &`
    The shell also supports local (non-daemon) job control.

  * `exit`
    Exits the `shell` client (this does not stop the server).

## Technical Robustness

This project includes solutions for several complex concurrency problems:

  * **Daemon `SIGCHLD` Race Condition:** The server daemon (`shared`) uses a `volatile sig_atomic_t` flag (`sigchld_received`). The `sigchld_handler` *only* sets this flag. The main loop checks this flag and *then* safely calls `waitpid` and decrements the `running_job_count` from within a semaphore lock, preventing race conditions.
  * **Signal Handler Inheritance:** The P2 and P3 processes explicitly reset their `SIGCHLD` handler to `SIG_DFL`. This prevents the P2 (Job Manager) from accidentally "stealing" the `SIGCHLD` signal from its P3 child, which would break the status update logic.
  * **`SIGPIPE` Crashes:** The P2 Job Manager ignores `SIGPIPE`. If a `jobstream` client disconnects, the `write` call fails with `EPIPE` instead of crashing the entire job.
  * **Orphan Process Prevention:** `jobkill` sends `SIGKILL` to the *negative PID* of the P2 process. Because P2 set itself as a process group leader (`setpgid`), this signal kills the entire group (P2 and P3), ensuring no processes are left running as orphans.
