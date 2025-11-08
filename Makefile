# Makefile for the SH-ell and SH-are System (Modular)
#
# This Makefile builds the modular version of the project.

# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -g

# --- Source Files ---
# Common
COMMON_SRC = ipc.c

# Server
SERVER_SRC = shared.c daemon.c job.c $(COMMON_SRC)

# Client
CLIENT_SRC = shell.c builtins.c $(COMMON_SRC)

# --- Object Files ---
SERVER_OBJS = $(SERVER_SRC:.c=.o)
CLIENT_OBJS = $(CLIENT_SRC:.c=.o)

# --- Targets ---

# Default target
all: shell shared

# Target for the client
shell: $(CLIENT_OBJS)
	$(CC) $(CFLAGS) -o shell $(CLIENT_OBJS)

# Target for the server/daemon
shared: $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o shared $(SERVER_OBJS)

# Generic rule to compile .c to .o
%.o: %.c %.h
	$(CC) $(CFLAGS) -c $< -o $@

# Specific rule for files that include sh_share.h
%.o: %.c sh_share.h
	$(CC) $(CFLAGS) -c $< -o $@

# Target to clean up
clean:
	rm -f shell shared *.o
	rm -f /tmp/sh_key /tmp/job-*.log /tmp/job-*.fifo

.PHONY: all clean