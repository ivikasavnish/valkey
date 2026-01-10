/* Coroutine-based execution system for accelerated commands
 * 
 * This module implements a lightweight coroutine system that integrates
 * with Redis's epoll event loop. Coroutines execute fast path commands
 * cooperatively, yielding when necessary rather than blocking threads.
 *
 * Key benefits over threads:
 * - Lower overhead (no thread context switches)
 * - Better integration with event loop
 * - Simpler reasoning (cooperative scheduling)
 * - Higher scalability (thousands of coroutines)
 *
 * Architecture:
 * - Coroutines use ucontext for stackful execution
 * - Each coroutine has 64KB stack
 * - Coroutine pool per command type (string/hash)
 * - Yields to main event loop for I/O
 */

#ifndef __COROUTINE_H
#define __COROUTINE_H

#include "server.h"
#include <ucontext.h>

/* Coroutine states */
typedef enum {
    CORO_STATE_FREE = 0,      /* Available for use */
    CORO_STATE_READY,         /* Ready to run */
    CORO_STATE_RUNNING,       /* Currently executing */
    CORO_STATE_SUSPENDED,     /* Yielded, waiting to resume */
    CORO_STATE_COMPLETED      /* Finished execution */
} coro_state_t;

/* Coroutine pool types */
typedef enum {
    CORO_POOL_STRING = 0,     /* GET, SET, INCR */
    CORO_POOL_HASH = 1,       /* HGET, HSET */
    CORO_POOL_COUNT = 2
} coro_pool_type_t;

/* Coroutine structure */
typedef struct coroutine {
    int id;                   /* Coroutine identifier */
    coro_state_t state;       /* Current state */
    ucontext_t context;       /* Execution context */
    char stack[65536];        /* 64KB stack */
    
    /* Command context */
    client *c;                /* Client that issued command */
    sds key;                  /* Key being operated on */
    int is_write;             /* Is this a write operation? */
    
    /* Response */
    int response_type;        /* 0=bulk, 1=integer, 2=simple, 3=error */
    sds response_data;        /* Response string */
    long long int_value;      /* Integer response value */
    
    /* Linked list */
    struct coroutine *next;
} coroutine_t;

/* Coroutine pool */
typedef struct coro_pool {
    coroutine_t *coroutines;  /* Array of coroutines */
    int count;                /* Total coroutines in pool */
    coroutine_t *free_list;   /* Free coroutines list */
    coroutine_t *ready_queue; /* Ready to run queue */
    int active_count;         /* Number of active coroutines */
} coro_pool_t;

/* Coroutine scheduler */
typedef struct coro_scheduler {
    coro_pool_t pools[CORO_POOL_COUNT];
    coroutine_t *current;     /* Currently running coroutine */
    ucontext_t main_context;  /* Main event loop context */
    int enabled;              /* Is scheduler enabled? */
} coro_scheduler_t;

/* Global scheduler */
extern coro_scheduler_t coro_scheduler;

/* Initialize coroutine system */
void coroutine_init(void);

/* Shutdown coroutine system */
void coroutine_shutdown(void);

/* Schedule a command for coroutine execution
 * Returns 1 on success, 0 if pool is full */
int coroutine_schedule(client *c, sds key, int is_write, coro_pool_type_t pool_type);

/* Run ready coroutines (called from event loop)
 * Executes up to max_count coroutines, returns number executed */
int coroutine_run_ready(int max_count);

/* Yield from current coroutine back to scheduler */
void coroutine_yield(void);

/* Resume a coroutine */
void coroutine_resume(coroutine_t *coro);

/* Get pool for command */
coro_pool_type_t coroutine_get_pool_type(const char *cmd_name);

#endif /* __COROUTINE_H */
