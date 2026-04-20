/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define LOG_DIR "logs"
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    int head, tail, count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

bounded_buffer_t log_buffer;

/* ================= BUFFER ================= */

int bounded_buffer_push(bounded_buffer_t *b, const log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);

    while (b->count == LOG_BUFFER_CAPACITY && !b->shutting_down)
        pthread_cond_wait(&b->not_full, &b->mutex);

    if (b->shutting_down) {
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }

    b->items[b->tail] = *item;
    b->tail = (b->tail + 1) % LOG_BUFFER_CAPACITY;
    b->count++;

    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

int bounded_buffer_pop(bounded_buffer_t *b, log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);

    while (b->count == 0 && !b->shutting_down)
        pthread_cond_wait(&b->not_empty, &b->mutex);

    if (b->count == 0 && b->shutting_down) {
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }

    *item = b->items[b->head];
    b->head = (b->head + 1) % LOG_BUFFER_CAPACITY;
    b->count--;

    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/* ================= LOGGER ================= */

void *logging_thread(void *arg)
{
    (void)arg;
    log_item_t item;

    mkdir(LOG_DIR, 0777);

    while (bounded_buffer_pop(&log_buffer, &item) == 0) {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);

        FILE *f = fopen(path, "a");
        if (f) {
            fwrite(item.data, 1, item.length, f);
            fclose(f);
        }
    }
    return NULL;
}

/* ================= CHILD ================= */

typedef struct {
    char id[CONTAINER_ID_LEN];
    char command[256];
    int pipe_fd;
} child_config_t;

int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    dup2(cfg->pipe_fd, STDOUT_FILENO);
    dup2(cfg->pipe_fd, STDERR_FILENO);
    close(cfg->pipe_fd);

    execl("/bin/sh", "sh", "-c", cfg->command, NULL);
    perror("exec failed");
    return 1;
}

/* ================= SIMPLE RUN ================= */

int run_container(const char *id, const char *command)
{
    int pipefd[2];
    pipe(pipefd);

    child_config_t cfg;
    strcpy(cfg.id, id);
    strcpy(cfg.command, command);
    cfg.pipe_fd = pipefd[1];

    char *stack = malloc(STACK_SIZE);
    pid_t pid = clone(child_fn, stack + STACK_SIZE, SIGCHLD, &cfg);

    close(pipefd[1]);

    char buffer[LOG_CHUNK_SIZE];
    ssize_t n;

    while ((n = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
        log_item_t item;
        strcpy(item.container_id, id);
        memcpy(item.data, buffer, n);
        item.length = n;

        bounded_buffer_push(&log_buffer, &item);
    }

    waitpid(pid, NULL, 0);
    close(pipefd[0]);

    return 0;
}

/* ================= MAIN ================= */

int main(int argc, char *argv[])
{
    if (argc < 3) {
        printf("Usage: %s run <id> <command>\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "run") != 0) {
        printf("Only 'run' supported in this version\n");
        return 1;
    }

    pthread_mutex_init(&log_buffer.mutex, NULL);
    pthread_cond_init(&log_buffer.not_empty, NULL);
    pthread_cond_init(&log_buffer.not_full, NULL);

    pthread_t logger;
    pthread_create(&logger, NULL, logging_thread, NULL);

    run_container(argv[2], argv[3]);

    pthread_mutex_lock(&log_buffer.mutex);
    log_buffer.shutting_down = 1;
    pthread_cond_broadcast(&log_buffer.not_empty);
    pthread_cond_broadcast(&log_buffer.not_full);
    pthread_mutex_unlock(&log_buffer.mutex);

    pthread_join(logger, NULL);

    printf("Execution complete. Logs saved in /logs folder\n");
    return 0;
}

        
      
