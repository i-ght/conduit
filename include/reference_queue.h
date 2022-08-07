#ifndef _REF_QUEUE_H
#define _REF_QUEUE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "okorerr.h"
#include "stdlibfuncptrs.h"

struct RefQueue
{
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint32_t capacity;
    void** data;
};

/* Ref queue operation */
typedef enum RefQueueOp
{
    REFQ_OK,
    REFQ_FULL,
    REFQ_EMPTY,
} RefQOp;


int ref_q_construct(
    struct RefQueue* ref_q,
    const size_t capacity,
    const MemAlloc memAlloc
);

void ref_q_destruct(
    struct RefQueue* ref_q
);
bool ref_q_full(
    const struct RefQueue* ref_q
);

bool ref_q_empty(
    const struct RefQueue* ref_q
);

enum RefQueueOp ref_q_enqueue(
    struct RefQueue* ref_q,
    void* value
);

enum RefQueueOp ref_q_dequeue(
    struct RefQueue* ref_q,
    void** value
);

void* ref_q_dequeue_ref(
    struct RefQueue* ref_q
);


uint32_t ref_q_count(
    const struct RefQueue* queue
);


uint32_t ref_q_capacity(
    const struct RefQueue* queue
);

#endif