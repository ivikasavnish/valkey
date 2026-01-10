/* worker.c - Worker thread for accelerated command execution
 *
 * Copyright (c) 2024, Valkey contributors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "worker.h"
#include <pthread.h>
#include <stdlib.h>

#ifdef ENABLE_ACCELERATOR

#define WORKER_QUEUE_SIZE 10000

/* Command queue entry */
typedef struct command_queue_entry {
    client *c;
} command_queue_entry;

/* Worker thread state */
static struct {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    command_queue_entry *queue;
    int queue_size;
    int queue_head;
    int queue_tail;
    int queue_count;
    int shutdown;
} worker_state;

/* Worker thread main loop */
static void *worker_thread_main(void *arg) {
    UNUSED(arg);
    
    while (1) {
        pthread_mutex_lock(&worker_state.lock);
        
        /* Wait for commands or shutdown signal */
        while (worker_state.queue_count == 0 && !worker_state.shutdown) {
            pthread_cond_wait(&worker_state.cond, &worker_state.lock);
        }
        
        if (worker_state.shutdown && worker_state.queue_count == 0) {
            pthread_mutex_unlock(&worker_state.lock);
            break;
        }
        
        /* Dequeue command */
        command_queue_entry entry = worker_state.queue[worker_state.queue_head];
        worker_state.queue_head = (worker_state.queue_head + 1) % worker_state.queue_size;
        worker_state.queue_count--;
        
        pthread_mutex_unlock(&worker_state.lock);
        
        /* Execute command using existing Redis command processor */
        if (entry.c && entry.c->cmd && entry.c->cmd->proc) {
            call(entry.c, CMD_CALL_FULL);
        }
    }
    
    return NULL;
}

/* Initialize worker thread */
void worker_init(void) {
    worker_state.queue_size = WORKER_QUEUE_SIZE;
    worker_state.queue = zmalloc(sizeof(command_queue_entry) * worker_state.queue_size);
    worker_state.queue_head = 0;
    worker_state.queue_tail = 0;
    worker_state.queue_count = 0;
    worker_state.shutdown = 0;
    
    pthread_mutex_init(&worker_state.lock, NULL);
    pthread_cond_init(&worker_state.cond, NULL);
    
    pthread_create(&worker_state.thread, NULL, worker_thread_main, NULL);
    
    serverLog(LL_NOTICE, "Worker thread initialized");
}

/* Shutdown worker thread */
void worker_shutdown(void) {
    pthread_mutex_lock(&worker_state.lock);
    worker_state.shutdown = 1;
    pthread_cond_signal(&worker_state.cond);
    pthread_mutex_unlock(&worker_state.lock);
    
    pthread_join(worker_state.thread, NULL);
    
    pthread_mutex_destroy(&worker_state.lock);
    pthread_cond_destroy(&worker_state.cond);
    
    zfree(worker_state.queue);
    
    serverLog(LL_NOTICE, "Worker thread shutdown complete");
}

/* Enqueue a command for execution by worker thread */
int worker_enqueue_command(client *c) {
    pthread_mutex_lock(&worker_state.lock);
    
    /* Check if queue is full */
    if (worker_state.queue_count >= worker_state.queue_size) {
        pthread_mutex_unlock(&worker_state.lock);
        return -1; /* Queue full, caller should fallback */
    }
    
    /* Enqueue command */
    worker_state.queue[worker_state.queue_tail].c = c;
    worker_state.queue_tail = (worker_state.queue_tail + 1) % worker_state.queue_size;
    worker_state.queue_count++;
    
    /* Signal worker thread */
    pthread_cond_signal(&worker_state.cond);
    
    pthread_mutex_unlock(&worker_state.lock);
    
    return 0;
}

#else /* !ENABLE_ACCELERATOR */

/* Accelerator disabled - all functions are no-ops */

void worker_init(void) {
    /* No-op */
}

void worker_shutdown(void) {
    /* No-op */
}

int worker_enqueue_command(client *c) {
    UNUSED(c);
    return -1; /* Always fail */
}

#endif /* ENABLE_ACCELERATOR */
