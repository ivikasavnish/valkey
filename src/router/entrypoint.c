/* entrypoint.c - Command routing entrypoint for accelerated execution
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

#include "entrypoint.h"
#include "../executor/coroutine.h"
#include <string.h>

/* Metrics counters */
long long accelerated_commands_total = 0;
long long legacy_commands_total = 0;
long long fallback_invocations = 0;

#ifdef ENABLE_ACCELERATOR

/* Command configuration: name, coroutine pool, and write flag */
typedef struct {
    const char *name;
    coro_pool_type_t pool;
    int is_write;
} command_config;

/* Whitelist of accelerated commands with their coroutine pools */
static const command_config accelerated_commands[] = {
    {"get", CORO_POOL_STRING, 0},     /* Read operation */
    {"set", CORO_POOL_STRING, 1},     /* Write operation */
    {"incr", CORO_POOL_STRING, 1},    /* Write operation */
    {"hget", CORO_POOL_HASH, 0},      /* Read operation */
    {"hset", CORO_POOL_HASH, 1},      /* Write operation */
    {NULL, 0, 0}
};

/* Check if a command is in the acceleration whitelist and return its pool */
static int get_accelerated_command_pool(const struct serverCommand *cmd, 
                                        coro_pool_type_t *pool, int *is_write) {
    if (!cmd || !cmd->declared_name) {
        return 0;
    }
    
    for (int i = 0; accelerated_commands[i].name != NULL; i++) {
        if (strcasecmp(cmd->declared_name, accelerated_commands[i].name) == 0) {
            *pool = accelerated_commands[i].pool;
            *is_write = accelerated_commands[i].is_write;
            return 1;
        }
    }
    
    return 0;
}

/* Check if a command is in the acceleration whitelist */
int is_accelerated_command(const struct serverCommand *cmd) {
    coro_pool_type_t pool;
    int is_write;
    return get_accelerated_command_pool(cmd, &pool, &is_write);
}

/* Route command through accelerated execution path
 * 
 * v4: Coroutine-based execution integrating with epoll event loop.
 * Lightweight coroutines execute commands cooperatively, yielding when necessary.
 * Much lower overhead than threads, better integration with event loop.
 */
void command_entrypoint(client *c) {
    /* Safety checks - if anything looks wrong, fallback immediately */
    if (!c || !c->cmd || !c->cmd->proc) {
        fallback_invocations++;
        if (c && c->cmd && c->cmd->proc) {
            call(c, CMD_CALL_FULL);
        }
        return;
    }
    
    /* Determine which coroutine pool to use */
    coro_pool_type_t pool;
    int is_write;
    if (!get_accelerated_command_pool(c->cmd, &pool, &is_write)) {
        /* Not in whitelist - fallback */
        fallback_invocations++;
        call(c, CMD_CALL_FULL);
        return;
    }
    
    /* Extract key from command */
    sds key = NULL;
    if (c->argc >= 2 && c->argv[1]) {
        key = objectGetVal(c->argv[1]);
    }
    
    if (!key) {
        /* No key - fallback */
        fallback_invocations++;
        call(c, CMD_CALL_FULL);
        return;
    }
    
    /* v4: Schedule command in coroutine */
    int result = coroutine_schedule(c, key, is_write, pool);
    
    if (result == 0) {
        /* Failed to schedule - coroutine pool full, fallback to legacy path */
        fallback_invocations++;
        call(c, CMD_CALL_FULL);
    } else {
        accelerated_commands_total++;
        /* Command will be executed by coroutine when scheduled */
    }
}

/* Initialize the accelerator system */
void accelerator_init(void) {
    /* Initialize coroutine system (v4) */
    coroutine_init();
    serverLog(LL_NOTICE, "Accelerator v4 enabled (coroutine-based execution)");
}

/* Shutdown the accelerator system */
void accelerator_shutdown(void) {
    coroutine_shutdown();
    serverLog(LL_NOTICE, "Accelerator shutdown complete");
}

#else /* !ENABLE_ACCELERATOR */

/* Accelerator disabled at compile time - all functions are no-ops */

int is_accelerated_command(const struct serverCommand *cmd) {
    UNUSED(cmd);
    return 0;
}

void command_entrypoint(client *c) {
    /* Should never be called when accelerator is disabled */
    UNUSED(c);
}

void accelerator_init(void) {
    /* No-op */
}

void accelerator_shutdown(void) {
    /* No-op */
}

#endif /* ENABLE_ACCELERATOR */
