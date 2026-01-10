/* fastpath.c - Custom fast path implementations for accelerated commands
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

#include "fastpath.h"
#include "../expire.h"
#include <string.h>
#include <strings.h>

#ifdef ENABLE_ACCELERATOR

/* Create a new command result */
static command_result *create_result(void) {
    command_result *result = zmalloc(sizeof(command_result));
    result->success = 0;
    result->response = NULL;
    result->response_type = 0;
    result->int_value = 0;
    return result;
}

/* Free command result */
void fastpath_free_result(command_result *result) {
    if (!result) return;
    if (result->response) sdsfree(result->response);
    zfree(result);
}

/* Fast path GET implementation */
command_result *fastpath_get(client *c) {
    command_result *result = create_result();
    
    /* Validate arguments */
    if (c->argc != 2) {
        result->response_type = 3; /* error */
        result->response = sdsnew("ERR wrong number of arguments for 'get' command");
        return result;
    }
    
    /* Get key */
    sds key = objectGetVal(c->argv[1]);
    
    /* Lookup value in database */
    robj *val = lookupKeyRead(c->db, c->argv[1]);
    
    if (!val) {
        /* Key not found - return NULL bulk string */
        result->success = 1;
        result->response_type = 0; /* bulk */
        result->response = NULL;  /* NULL bulk string */
        return result;
    }
    
    /* Check type */
    if (val->type != OBJ_STRING) {
        result->response_type = 3; /* error */
        result->response = sdsnew("WRONGTYPE Operation against a key holding the wrong kind of value");
        return result;
    }
    
    /* Get string value */
    val = getDecodedObject(val);
    result->success = 1;
    result->response_type = 0; /* bulk */
    result->response = sdsdup(objectGetVal(val));
    decrRefCount(val);
    
    return result;
}

/* Fast path SET implementation */
command_result *fastpath_set(client *c) {
    command_result *result = create_result();
    
    /* Basic SET (no options for v3 fast path) */
    if (c->argc < 3) {
        result->response_type = 3; /* error */
        result->response = sdsnew("ERR wrong number of arguments for 'set' command");
        return result;
    }
    
    /* For simplicity, only handle basic SET key value */
    if (c->argc != 3) {
        /* Fallback to normal path for complex SET */
        result->response_type = 3;
        result->response = sdsnew("ERR fastpath only supports basic SET");
        return result;
    }
    
    /* Set the value */
    robj *val_ref = c->argv[2];
    setKey(c, c->db, c->argv[1], &val_ref, 0);
    
    result->success = 1;
    result->response_type = 2; /* simple string */
    result->response = sdsnew("OK");
    
    return result;
}

/* Fast path INCR implementation */
command_result *fastpath_incr(client *c) {
    command_result *result = create_result();
    
    /* Validate arguments */
    if (c->argc != 2) {
        result->response_type = 3; /* error */
        result->response = sdsnew("ERR wrong number of arguments for 'incr' command");
        return result;
    }
    
    /* Lookup current value */
    robj *val = lookupKeyWrite(c->db, c->argv[1]);
    long long value;
    
    if (val) {
        /* Check type */
        if (val->type != OBJ_STRING) {
            result->response_type = 3;
            result->response = sdsnew("WRONGTYPE Operation against a key holding the wrong kind of value");
            return result;
        }
        
        /* Get current value */
        if (getLongLongFromObject(val, &value) != C_OK) {
            result->response_type = 3;
            result->response = sdsnew("ERR value is not an integer or out of range");
            return result;
        }
        
        /* Check overflow */
        if (value == LLONG_MAX) {
            result->response_type = 3;
            result->response = sdsnew("ERR increment or decrement would overflow");
            return result;
        }
    } else {
        value = 0;
    }
    
    /* Increment */
    value++;
    
    /* Create new object */
    robj *new_val = createStringObjectFromLongLong(value);
    robj *val_ref = new_val;
    setKey(c, c->db, c->argv[1], &val_ref, 0);
    decrRefCount(new_val);
    
    result->success = 1;
    result->response_type = 1; /* integer */
    result->int_value = value;
    
    return result;
}

/* Fast path HGET implementation */
command_result *fastpath_hget(client *c) {
    command_result *result = create_result();
    
    /* Validate arguments */
    if (c->argc != 3) {
        result->response_type = 3;
        result->response = sdsnew("ERR wrong number of arguments for 'hget' command");
        return result;
    }
    
    /* Lookup hash */
    robj *o = lookupKeyRead(c->db, c->argv[1]);
    
    if (!o) {
        /* Hash doesn't exist - return NULL */
        result->success = 1;
        result->response_type = 0; /* bulk */
        result->response = NULL;
        return result;
    }
    
    /* Check type */
    if (o->type != OBJ_HASH) {
        result->response_type = 3;
        result->response = sdsnew("WRONGTYPE Operation against a key holding the wrong kind of value");
        return result;
    }
    
    /* Get field value */
    robj *val = hashTypeGetValueObject(o, objectGetVal(c->argv[2]));
    
    if (!val) {
        /* Field doesn't exist */
        result->success = 1;
        result->response_type = 0; /* bulk */
        result->response = NULL;
        return result;
    }
    
    /* Return value */
    result->success = 1;
    result->response_type = 0; /* bulk */
    result->response = sdsdup(objectGetVal(val));
    decrRefCount(val);
    
    return result;
}

/* Fast path HSET implementation */
command_result *fastpath_hset(client *c) {
    command_result *result = create_result();
    
    /* Validate arguments - basic HSET hash field value */
    if (c->argc != 4) {
        result->response_type = 3;
        result->response = sdsnew("ERR wrong number of arguments for 'hset' command");
        return result;
    }
    
    /* Lookup or create hash */
    robj *o = lookupKeyWrite(c->db, c->argv[1]);
    
    if (!o) {
        o = createHashObject();
        robj *val_ref = o;
        dbAdd(c->db, c->argv[1], &val_ref);
    } else {
        /* Check type */
        if (o->type != OBJ_HASH) {
            result->response_type = 3;
            result->response = sdsnew("WRONGTYPE Operation against a key holding the wrong kind of value");
            return result;
        }
    }
    
    /* Set field - hashTypeSet signature: (robj *o, sds field, sds value, long long expiry, int flags) */
    int update = hashTypeSet(o, objectGetVal(c->argv[2]), objectGetVal(c->argv[3]), EXPIRY_NONE, HASH_SET_COPY);
    
    result->success = 1;
    result->response_type = 1; /* integer */
    result->int_value = update ? 0 : 1; /* 1 if new field, 0 if updated */
    
    return result;
}

/* Execute command using fast path based on command name */
command_result *fastpath_execute(client *c) {
    if (!c || !c->cmd || !c->cmd->declared_name) {
        return NULL;
    }
    
    const char *cmd_name = c->cmd->declared_name;
    
    if (strcasecmp(cmd_name, "get") == 0) {
        return fastpath_get(c);
    } else if (strcasecmp(cmd_name, "set") == 0) {
        return fastpath_set(c);
    } else if (strcasecmp(cmd_name, "incr") == 0) {
        return fastpath_incr(c);
    } else if (strcasecmp(cmd_name, "hget") == 0) {
        return fastpath_hget(c);
    } else if (strcasecmp(cmd_name, "hset") == 0) {
        return fastpath_hset(c);
    }
    
    return NULL;
}

#else /* !ENABLE_ACCELERATOR */

void fastpath_free_result(command_result *result) {
    UNUSED(result);
}

command_result *fastpath_execute(client *c) {
    UNUSED(c);
    return NULL;
}

#endif /* ENABLE_ACCELERATOR */
