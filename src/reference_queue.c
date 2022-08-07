#include "reference_queue.h"

enum OKorERR ref_q_construct(
    struct RefQueue* ref_q,
    size_t capacity,
    MemAlloc mem_alloc)
{
    void** tmp = 
        mem_alloc(
            capacity == 0 
            ? 1 * sizeof(void*)
            : capacity * sizeof(void*)        
        );
    if (NULL == tmp) {
        return ERR;
    }

    ref_q->capacity = capacity;
    ref_q->head = 0;
    ref_q->tail = 0;
    ref_q->data = tmp;

    return OK;
}

void ref_q_destruct(
    struct RefQueue* ref_q)
{
    free(ref_q->data);
    ref_q->data = NULL;
}

bool ref_q_full(
    const struct RefQueue* ref_q)
{
    return ref_q->count == ref_q->capacity;
}

bool ref_q_empty(
    const struct RefQueue* ref_q)
{
    return ref_q->count == 0;
}

static void incrOrLoopBack(
    uint32_t* i,
    const uint32_t max)
{
    uint32_t tmp = *i + 1;
    if (tmp >= max)
        tmp = 0;
    *i = tmp;
}

enum RefQueueOp ref_q_enqueue(
    struct RefQueue* ref_q,
    void* value)
{
    if (ref_q_full(ref_q))
        return REFQ_FULL;
    
    const uint32_t i = ref_q->tail;
    ref_q->data[i] = value;
    incrOrLoopBack(
        &ref_q->tail,
        ref_q->capacity
    );
    ref_q->count++;
    return REFQ_OK;
}

enum RefQueueOp ref_q_dequeue(
    struct RefQueue* ref_q,
    void** value)
{
    if (ref_q_empty(ref_q)) {
        return REFQ_EMPTY;
    }
    
    const uint32_t i = ref_q->head;
    *value = ref_q->data[i];
    incrOrLoopBack(
        &ref_q->head,
        ref_q->capacity
    );
    ref_q->count--;
    return REFQ_OK;
}

void* ref_q_dequeue_ref(
    struct RefQueue* ref_q)
{
    void* value = NULL;
    switch (ref_q_dequeue(ref_q, &value)) {
        case REFQ_OK: return value;
        default: return NULL;
    }
}

uint32_t ref_q_count(
    const struct RefQueue* queue)
{
    return queue->count;
}

uint32_t ref_q_capacity(
    const struct RefQueue* queue)
{
    return queue->capacity;
}