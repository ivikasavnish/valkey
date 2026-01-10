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
#include "../executor/worker.h"
#include <string.h>

/* Metrics counters */
long long accelerated_commands_total = 0;
long long legacy_commands_total = 0;
long long fallback_invocations = 0;

#ifdef ENABLE_ACCELERATOR

/* Whitelist of accelerated commands */
static const char *accelerated_command_names[] = {
    "get",
    "set",
    "incr",
    "hget",
    "hset",
    NULL
};

/* Check if a command is in the acceleration whitelist */
int is_accelerated_command(const struct serverCommand *cmd) {
    if (!cmd || !cmd->declared_name) {
        return 0;
    }
    
    for (int i = 0; accelerated_command_names[i] != NULL; i++) {
        if (strcasecmp(cmd->declared_name, accelerated_command_names[i]) == 0) {
            return 1;
        }
    }
    
    return 0;
}

/* Route command through accelerated execution path
 * 
 * For v1, we execute commands synchronously in the main thread,
 * just like the legacy path. The difference is that we're routing
 * through this entrypoint which can be extended in the future
 * to use worker threads or other optimizations.
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
    
    /* For now, execute directly in main thread (same as legacy path)
     * This maintains correctness while allowing future optimization */
    accelerated_commands_total++;
    call(c, CMD_CALL_FULL);
}

/* Initialize the accelerator system */
void accelerator_init(void) {
    /* For v1, we don't actually start worker threads yet
     * This is a placeholder for future enhancements */
    serverLog(LL_NOTICE, "Accelerator enabled (routing mode)");
}

/* Shutdown the accelerator system */
void accelerator_shutdown(void) {
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
