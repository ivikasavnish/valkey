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
#define KEY_LOCK_TABLE_SIZE 1024

/* Key lock entry for concurrency control */
typedef struct key_lock_entry {
    sds key;
    int write_lock;       /* 1 if write lock, 0 if read lock */
    int ref_count;        /* Number of readers (for read locks) */
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    struct key_lock_entry *next;
} key_lock_entry;

/* Key lock hash table */
static struct {
    key_lock_entry **table;
    pthread_mutex_t global_lock;
} key_locks;

/* Command queue entry */
typedef struct command_queue_entry {
    client *c;
} command_queue_entry;

/* Worker pool state */
typedef struct {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    command_queue_entry *queue;
    int queue_size;
    int queue_head;
    int queue_tail;
    int queue_count;
    int shutdown;
    const char *name;
} worker_pool;

/* Worker pools for different command types */
static worker_pool worker_pools[WORKER_POOL_MAX];

/* Hash function for key lock table */
static unsigned int key_hash(sds key) {
    unsigned int hash = 5381;
    const char *str = key;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c;
    return hash % KEY_LOCK_TABLE_SIZE;
}

/* Initialize key lock system */
static void key_lock_init(void) {
    key_locks.table = zmalloc(sizeof(key_lock_entry*) * KEY_LOCK_TABLE_SIZE);
    for (int i = 0; i < KEY_LOCK_TABLE_SIZE; i++) {
        key_locks.table[i] = NULL;
    }
    pthread_mutex_init(&key_locks.global_lock, NULL);
    serverLog(LL_NOTICE, "Key lock system initialized");
}

/* Shutdown key lock system */
static void key_lock_shutdown(void) {
    pthread_mutex_lock(&key_locks.global_lock);
    for (int i = 0; i < KEY_LOCK_TABLE_SIZE; i++) {
        key_lock_entry *entry = key_locks.table[i];
        while (entry) {
            key_lock_entry *next = entry->next;
            sdsfree(entry->key);
            pthread_mutex_destroy(&entry->mutex);
            pthread_cond_destroy(&entry->cond);
            zfree(entry);
            entry = next;
        }
    }
    zfree(key_locks.table);
    pthread_mutex_unlock(&key_locks.global_lock);
    pthread_mutex_destroy(&key_locks.global_lock);
}

/* Acquire key lock - write_lock=1 for writes, 0 for guaranteed reads */
int key_lock_acquire(sds key, int write_lock) {
    if (!key) return -1;
    
    unsigned int hash = key_hash(key);
    
    pthread_mutex_lock(&key_locks.global_lock);
    
    /* Find or create lock entry */
    key_lock_entry *entry = key_locks.table[hash];
    key_lock_entry *prev = NULL;
    
    while (entry && sdscmp(entry->key, key) != 0) {
        prev = entry;
        entry = entry->next;
    }
    
    if (!entry) {
        /* Create new lock entry */
        entry = zmalloc(sizeof(key_lock_entry));
        entry->key = sdsdup(key);
        entry->write_lock = write_lock;
        entry->ref_count = write_lock ? 1 : 1;
        entry->next = NULL;
        pthread_mutex_init(&entry->mutex, NULL);
        pthread_cond_init(&entry->cond, NULL);
        
        if (prev) {
            prev->next = entry;
        } else {
            key_locks.table[hash] = entry;
        }
        pthread_mutex_unlock(&key_locks.global_lock);
        return 0;
    }
    
    pthread_mutex_unlock(&key_locks.global_lock);
    
    /* Wait for lock */
    pthread_mutex_lock(&entry->mutex);
    
    if (write_lock) {
        /* Write lock - wait until no readers or writers */
        while (entry->ref_count > 0) {
            pthread_cond_wait(&entry->cond, &entry->mutex);
        }
        entry->write_lock = 1;
        entry->ref_count = 1;
    } else {
        /* Read lock - wait if there's a write lock */
        while (entry->write_lock) {
            pthread_cond_wait(&entry->cond, &entry->mutex);
        }
        entry->ref_count++;
    }
    
    pthread_mutex_unlock(&entry->mutex);
    return 0;
}

/* Release key lock */
void key_lock_release(sds key) {
    if (!key) return;
    
    unsigned int hash = key_hash(key);
    
    pthread_mutex_lock(&key_locks.global_lock);
    
    key_lock_entry *entry = key_locks.table[hash];
    while (entry && sdscmp(entry->key, key) != 0) {
        entry = entry->next;
    }
    
    if (!entry) {
        pthread_mutex_unlock(&key_locks.global_lock);
        return;
    }
    
    pthread_mutex_unlock(&key_locks.global_lock);
    
    pthread_mutex_lock(&entry->mutex);
    
    if (entry->write_lock) {
        entry->write_lock = 0;
        entry->ref_count = 0;
    } else {
        entry->ref_count--;
    }
    
    /* Wake up waiting threads */
    pthread_cond_broadcast(&entry->cond);
    pthread_mutex_unlock(&entry->mutex);
}

/* Check if key is locked for writing */
int key_is_locked(sds key) {
    if (!key) return 0;
    
    unsigned int hash = key_hash(key);
    
    pthread_mutex_lock(&key_locks.global_lock);
    
    key_lock_entry *entry = key_locks.table[hash];
    while (entry && sdscmp(entry->key, key) != 0) {
        entry = entry->next;
    }
    
    int locked = 0;
    if (entry) {
        pthread_mutex_lock(&entry->mutex);
        locked = entry->write_lock;
        pthread_mutex_unlock(&entry->mutex);
    }
    
    pthread_mutex_unlock(&key_locks.global_lock);
    return locked;
}

/* Worker thread main loop */
static void *worker_thread_main(void *arg) {
    worker_pool *pool = (worker_pool *)arg;
    
    while (1) {
        pthread_mutex_lock(&pool->lock);
        
        /* Wait for commands or shutdown signal */
        while (pool->queue_count == 0 && !pool->shutdown) {
            pthread_cond_wait(&pool->cond, &pool->lock);
        }
        
        if (pool->shutdown && pool->queue_count == 0) {
            pthread_mutex_unlock(&pool->lock);
            break;
        }
        
        /* Dequeue command */
        command_queue_entry entry = pool->queue[pool->queue_head];
        pool->queue_head = (pool->queue_head + 1) % pool->queue_size;
        pool->queue_count--;
        
        pthread_mutex_unlock(&pool->lock);
        
        /* Execute command with key locking */
        if (entry.c && entry.c->cmd && entry.c->cmd->proc) {
            /* Acquire key lock before execution */
            sds key = NULL;
            int write_op = 0;
            
            /* Determine if this is a write operation */
            if (entry.c->cmd->flags & CMD_WRITE) {
                write_op = 1;
            }
            
            /* Get the key from arguments */
            if (entry.c->argc > 1) {
                key = objectGetVal(entry.c->argv[1]);
            }
            
            if (key) {
                key_lock_acquire(key, write_op);
            }
            
            /* Execute command */
            call(entry.c, CMD_CALL_FULL);
            
            /* Release key lock after execution */
            if (key) {
                key_lock_release(key);
            }
        }
    }
    
    return NULL;
}

/* Initialize a single worker pool */
static void worker_pool_init(worker_pool *pool, const char *name) {
    pool->queue_size = WORKER_QUEUE_SIZE;
    pool->queue = zmalloc(sizeof(command_queue_entry) * pool->queue_size);
    pool->queue_head = 0;
    pool->queue_tail = 0;
    pool->queue_count = 0;
    pool->shutdown = 0;
    pool->name = name;
    
    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->cond, NULL);
    
    pthread_create(&pool->thread, NULL, worker_thread_main, pool);
    
    serverLog(LL_NOTICE, "Worker pool '%s' initialized", name);
}

/* Shutdown a single worker pool */
static void worker_pool_shutdown(worker_pool *pool) {
    pthread_mutex_lock(&pool->lock);
    pool->shutdown = 1;
    pthread_cond_signal(&pool->cond);
    pthread_mutex_unlock(&pool->lock);
    
    pthread_join(pool->thread, NULL);
    
    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->cond);
    
    zfree(pool->queue);
    
    serverLog(LL_NOTICE, "Worker pool '%s' shutdown complete", pool->name);
}

/* Initialize worker threads */
void worker_init(void) {
    /* Initialize key lock system */
    key_lock_init();
    
    /* Initialize worker pools */
    worker_pool_init(&worker_pools[WORKER_POOL_STRING], "string");
    worker_pool_init(&worker_pools[WORKER_POOL_HASH], "hash");
    
    serverLog(LL_NOTICE, "All worker pools initialized");
}

/* Shutdown worker threads */
void worker_shutdown(void) {
    /* Shutdown all worker pools */
    for (int i = 0; i < WORKER_POOL_MAX; i++) {
        worker_pool_shutdown(&worker_pools[i]);
    }
    
    /* Shutdown key lock system */
    key_lock_shutdown();
    
    serverLog(LL_NOTICE, "All worker pools shutdown complete");
}

/* Enqueue a command for execution by worker thread */
int worker_enqueue_command(client *c, worker_pool_type pool_type) {
    if (pool_type < 0 || pool_type >= WORKER_POOL_MAX) {
        return -1;
    }
    
    worker_pool *pool = &worker_pools[pool_type];
    
    pthread_mutex_lock(&pool->lock);
    
    /* Check if queue is full */
    if (pool->queue_count >= pool->queue_size) {
        pthread_mutex_unlock(&pool->lock);
        return -1; /* Queue full, caller should fallback */
    }
    
    /* Enqueue command */
    pool->queue[pool->queue_tail].c = c;
    pool->queue_tail = (pool->queue_tail + 1) % pool->queue_size;
    pool->queue_count++;
    
    /* Signal worker thread */
    pthread_cond_signal(&pool->cond);
    
    pthread_mutex_unlock(&pool->lock);
    
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

int worker_enqueue_command(client *c, worker_pool_type pool) {
    UNUSED(c);
    UNUSED(pool);
    return -1; /* Always fail */
}

int key_lock_acquire(sds key, int write_lock) {
    UNUSED(key);
    UNUSED(write_lock);
    return 0;
}

void key_lock_release(sds key) {
    UNUSED(key);
}

int key_is_locked(sds key) {
    UNUSED(key);
    return 0;
}

#endif /* ENABLE_ACCELERATOR */
