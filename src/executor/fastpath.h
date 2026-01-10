/* fastpath.h - Custom fast path implementations for accelerated commands
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

#ifndef __FASTPATH_H
#define __FASTPATH_H

#include "../server.h"

/* Command execution result */
typedef struct {
    int success;           /* 1 if successful, 0 if error */
    sds response;          /* Pre-built response string */
    int response_type;     /* Response type: 0=bulk, 1=integer, 2=simple, 3=error */
    long long int_value;   /* For integer responses */
} command_result;

/* Fast path command implementations */
command_result *fastpath_get(client *c);
command_result *fastpath_set(client *c);
command_result *fastpath_incr(client *c);
command_result *fastpath_hget(client *c);
command_result *fastpath_hset(client *c);

/* Execute command using fast path */
command_result *fastpath_execute(client *c);

/* Free command result */
void fastpath_free_result(command_result *result);

#endif /* __FASTPATH_H */
