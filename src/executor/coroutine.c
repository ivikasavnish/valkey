/* Coroutine-based execution implementation
 *
 * This implements a coroutine scheduler that integrates with Redis's
 * event loop for high-performance command execution.
 */

#include "server.h"
#include "coroutine.h"
#include "fastpath.h"
#include <ucontext.h>
#include <string.h>

/* Global scheduler instance */
coro_scheduler_t coro_scheduler = {0};

/* Number of coroutines per pool */
#define COROUTINES_PER_POOL 1000

/* Forward declarations */
static void coroutine_entry_point(void);
static coroutine_t* coro_pool_get_free(coro_pool_t *pool);
static void coro_pool_enqueue_ready(coro_pool_t *pool, coroutine_t *coro);
static coroutine_t* coro_pool_dequeue_ready(coro_pool_t *pool);
static void coroutine_execute(coroutine_t *coro);

/* Initialize coroutine system */
void coroutine_init(void) {
    serverLog(LL_NOTICE, "Initializing coroutine system...");
    
    memset(&coro_scheduler, 0, sizeof(coro_scheduler_t));
    
    /* Initialize each pool */
    for (int p = 0; p < CORO_POOL_COUNT; p++) {
        coro_pool_t *pool = &coro_scheduler.pools[p];
        pool->count = COROUTINES_PER_POOL;
        pool->coroutines = zmalloc(sizeof(coroutine_t) * COROUTINES_PER_POOL);
        pool->free_list = NULL;
        pool->ready_queue = NULL;
        pool->active_count = 0;
        
        /* Initialize all coroutines and add to free list */
        for (int i = 0; i < COROUTINES_PER_POOL; i++) {
            coroutine_t *coro = &pool->coroutines[i];
            coro->id = i;
            coro->state = CORO_STATE_FREE;
            coro->c = NULL;
            coro->key = NULL;
            coro->is_write = 0;
            coro->response_type = 0;
            coro->response_data = NULL;
            coro->int_value = 0;
            
            /* Add to free list */
            coro->next = pool->free_list;
            pool->free_list = coro;
        }
    }
    
    coro_scheduler.current = NULL;
    coro_scheduler.enabled = 1;
    
    serverLog(LL_NOTICE, "Coroutine system initialized: %d pools, %d coroutines per pool",
              CORO_POOL_COUNT, COROUTINES_PER_POOL);
}

/* Shutdown coroutine system */
void coroutine_shutdown(void) {
    if (!coro_scheduler.enabled) return;
    
    serverLog(LL_NOTICE, "Shutting down coroutine system...");
    
    /* Free all pools */
    for (int p = 0; p < CORO_POOL_COUNT; p++) {
        coro_pool_t *pool = &coro_scheduler.pools[p];
        
        /* Free any allocated response data */
        for (int i = 0; i < pool->count; i++) {
            coroutine_t *coro = &pool->coroutines[i];
            if (coro->key) sdsfree(coro->key);
            if (coro->response_data) sdsfree(coro->response_data);
        }
        
        zfree(pool->coroutines);
    }
    
    coro_scheduler.enabled = 0;
    serverLog(LL_NOTICE, "Coroutine system shut down");
}

/* Get pool type for command */
coro_pool_type_t coroutine_get_pool_type(const char *cmd_name) {
    if (strcasecmp(cmd_name, "get") == 0 ||
        strcasecmp(cmd_name, "set") == 0 ||
        strcasecmp(cmd_name, "incr") == 0) {
        return CORO_POOL_STRING;
    }
    return CORO_POOL_HASH;
}

/* Get a free coroutine from pool */
static coroutine_t* coro_pool_get_free(coro_pool_t *pool) {
    if (!pool->free_list) return NULL;
    
    coroutine_t *coro = pool->free_list;
    pool->free_list = coro->next;
    coro->next = NULL;
    return coro;
}

/* Enqueue coroutine to ready queue */
static void coro_pool_enqueue_ready(coro_pool_t *pool, coroutine_t *coro) {
    coro->next = NULL;
    if (!pool->ready_queue) {
        pool->ready_queue = coro;
    } else {
        /* Find end of queue */
        coroutine_t *tail = pool->ready_queue;
        while (tail->next) tail = tail->next;
        tail->next = coro;
    }
}

/* Dequeue coroutine from ready queue */
static coroutine_t* coro_pool_dequeue_ready(coro_pool_t *pool) {
    if (!pool->ready_queue) return NULL;
    
    coroutine_t *coro = pool->ready_queue;
    pool->ready_queue = coro->next;
    coro->next = NULL;
    return coro;
}

/* Schedule a command for coroutine execution */
int coroutine_schedule(client *c, sds key, int is_write, coro_pool_type_t pool_type) {
    if (!coro_scheduler.enabled) return 0;
    if (pool_type >= CORO_POOL_COUNT) return 0;
    
    coro_pool_t *pool = &coro_scheduler.pools[pool_type];
    
    /* Get free coroutine */
    coroutine_t *coro = coro_pool_get_free(pool);
    if (!coro) {
        serverLog(LL_WARNING, "Coroutine pool %d exhausted", pool_type);
        return 0;
    }
    
    /* Initialize coroutine */
    coro->state = CORO_STATE_READY;
    coro->c = c;
    coro->key = sdsdup(key);
    coro->is_write = is_write;
    coro->response_type = 0;
    coro->response_data = NULL;
    coro->int_value = 0;
    
    /* Setup context */
    getcontext(&coro->context);
    coro->context.uc_stack.ss_sp = coro->stack;
    coro->context.uc_stack.ss_size = sizeof(coro->stack);
    coro->context.uc_link = &coro_scheduler.main_context;
    makecontext(&coro->context, coroutine_entry_point, 0);
    
    /* Add to ready queue */
    coro_pool_enqueue_ready(pool, coro);
    pool->active_count++;
    
    return 1;
}

/* Coroutine entry point */
static void coroutine_entry_point(void) {
    coroutine_t *coro = coro_scheduler.current;
    if (!coro) return;
    
    coro->state = CORO_STATE_RUNNING;
    
    /* Execute the command via fast path */
    coroutine_execute(coro);
    
    coro->state = CORO_STATE_COMPLETED;
    
    /* Coroutine finished - context automatically returns to main */
}

/* Execute coroutine's command */
static void coroutine_execute(coroutine_t *coro) {
    client *c = coro->c;
    
    /* Execute via fast path */
    command_result *result = fastpath_execute(c);
    
    if (result) {
        /* Store result */
        coro->response_type = result->response_type;
        if (result->response) {
            coro->response_data = sdsdup(result->response);
        }
        coro->int_value = result->int_value;
        
        /* Free fast path result */
        fastpath_free_result(result);
    } else {
        /* Error - set error response */
        coro->response_type = 3;
        coro->response_data = sdsnew("ERR Fast path execution failed");
    }
}

/* Run ready coroutines */
int coroutine_run_ready(int max_count) {
    if (!coro_scheduler.enabled) return 0;
    
    int executed = 0;
    
    /* Process coroutines from all pools */
    for (int p = 0; p < CORO_POOL_COUNT && executed < max_count; p++) {
        coro_pool_t *pool = &coro_scheduler.pools[p];
        
        while (executed < max_count) {
            coroutine_t *coro = coro_pool_dequeue_ready(pool);
            if (!coro) break;
            
            /* Save main context */
            coro_scheduler.current = coro;
            
            if (coro->state == CORO_STATE_READY) {
                /* First run - start coroutine */
                swapcontext(&coro_scheduler.main_context, &coro->context);
            } else if (coro->state == CORO_STATE_SUSPENDED) {
                /* Resume suspended coroutine */
                coro->state = CORO_STATE_RUNNING;
                swapcontext(&coro_scheduler.main_context, &coro->context);
            }
            
            /* Check if coroutine completed */
            if (coro->state == CORO_STATE_COMPLETED) {
                /* Send response to client */
                client *c = coro->c;
                
                switch (coro->response_type) {
                    case 0: /* Bulk string */
                        if (coro->response_data) {
                            addReplyBulkCBuffer(c, coro->response_data, sdslen(coro->response_data));
                        } else {
                            addReplyNull(c);
                        }
                        break;
                    case 1: /* Integer */
                        addReplyLongLong(c, coro->int_value);
                        break;
                    case 2: /* Simple string */
                        if (coro->response_data) {
                            addReplyProto(c, coro->response_data, sdslen(coro->response_data));
                        } else {
                            addReply(c, shared.ok);
                        }
                        break;
                    case 3: /* Error */
                        if (coro->response_data) {
                            addReplyErrorSds(c, coro->response_data);
                            coro->response_data = NULL; /* Ownership transferred */
                        } else {
                            addReplyError(c, "Coroutine execution error");
                        }
                        break;
                }
                
                /* Clean up and return to free list */
                if (coro->key) {
                    sdsfree(coro->key);
                    coro->key = NULL;
                }
                if (coro->response_data) {
                    sdsfree(coro->response_data);
                    coro->response_data = NULL;
                }
                
                coro->state = CORO_STATE_FREE;
                coro->next = pool->free_list;
                pool->free_list = coro;
                pool->active_count--;
            } else if (coro->state == CORO_STATE_SUSPENDED) {
                /* Re-queue for later */
                coro_pool_enqueue_ready(pool, coro);
            }
            
            executed++;
        }
    }
    
    coro_scheduler.current = NULL;
    return executed;
}

/* Yield from current coroutine */
void coroutine_yield(void) {
    coroutine_t *coro = coro_scheduler.current;
    if (!coro) return;
    
    coro->state = CORO_STATE_SUSPENDED;
    swapcontext(&coro->context, &coro_scheduler.main_context);
}

/* Resume a coroutine */
void coroutine_resume(coroutine_t *coro) {
    if (!coro || coro->state != CORO_STATE_SUSPENDED) return;
    
    coro->state = CORO_STATE_RUNNING;
    coro_scheduler.current = coro;
    swapcontext(&coro_scheduler.main_context, &coro->context);
    coro_scheduler.current = NULL;
}
