#include <sys/eventfd.h>

#include <unistd.h>

#include "conduit.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(*a))

static void destruct_conditionals(
    cnd_t** array,
    const size_t count)
{
    for (int i = 0; i < count; i++) {

        if (NULL != array[i]) {
            cnd_destroy(array[i]);
        }

    }
}

static void destruct_mutexs(
    mtx_t** array,
    const size_t count)
{
    for (int i = 0; i < count; i++) {

        if (NULL != array[i]) {
            mtx_destroy(array[i]);
        }

    }
}

enum OKorERR async_conduit_destruct(
    struct AsyncConduit* asy_con)
{
    
    mtx_t* mutexs_to_destruct[] = {
        &asy_con->mtx,
        &asy_con->r_mtx,
        &asy_con->w_mtx
    };

    #define MUTEX_COUNT ARRAY_SIZE(mutexs_to_destruct)

    destruct_mutexs(
        mutexs_to_destruct,
        MUTEX_COUNT
    );

    cnd_t* conditionals_to_destruct[] = {
        &asy_con->send_event,
        &asy_con->recv_event
    };
    destruct_conditionals(
        conditionals_to_destruct,
        ARRAY_SIZE(conditionals_to_destruct)
    );

}

enum OKorERR async_conduit_construct(
    struct AsyncConduit* asy_con)
{

    mtx_t* mutexs_to_construct[] = {
        &asy_con->mtx,
        &asy_con->r_mtx,
        &asy_con->w_mtx
    };

    #define MUTEX_COUNT ARRAY_SIZE(mutexs_to_construct)
    
    mtx_t* mutexs_to_destruct[MUTEX_COUNT] = {0};

    int err_cnt = 0;
    for (int i = 0; i < ARRAY_SIZE(mutexs_to_construct); i++) {
        if (thrd_success != mtx_init(mutexs_to_construct[i], mtx_plain)) {
            mutexs_to_destruct[i] = mutexs_to_construct[i];
        }
        err_cnt++;
    }

    if (err_cnt > 0) {
        destruct_mutexs(
            mutexs_to_destruct,
            err_cnt
        );
        return ERR;
    }

    cnd_t* conditionals_to_construct[] = {
        &asy_con->send_event,
        &asy_con->recv_event
    };

    for (int i = 0; i < ARRAY_SIZE(conditionals_to_construct); i++) {
        cnd_t* cnd = conditionals_to_construct[i];
        if (thrd_success != cnd_init(cnd)) {

            if (i >= 1) {
                cnd_destroy(conditionals_to_construct[0]);
            }

            destruct_mutexs(
                mutexs_to_construct,
                MUTEX_COUNT
            );

            if (0 != close(asy_con->recv_event_fd)) {
                
            }

            return ERR;
        } 
    }

    return OK;
}

enum OKorERR conduit_destruct(
    struct Conduit* con)
{
    ref_q_destruct(&con->queue);  

    mtx_destroy(&con->mtx);

    cnd_t* conditionals[] = {
        &con->send_event,
        &con->recv_event
    };
    destruct_conditionals(
        conditionals,
        ARRAY_SIZE(conditionals)
    );

    if (0 != close(con->recv_event_fd)) {
        return ERR;
    }

    return OK;
}

enum OKorERR conduit_construct(
    struct Conduit* con,
    const size_t capacity,
    const MemAlloc mem_alloc)
{
    if (OK !=
        ref_q_construct(
            &con->queue,
            capacity,
            mem_alloc
        )
    ) {
        return ERR;
    }

    int event_fd = eventfd(0, 0);

    if (-1 == event_fd) {
        ref_q_destruct(&con->queue);
        return ERR;
    }

    con->recv_event_fd = event_fd;

    if (thrd_success !=
        mtx_init(
            &con->mtx,
            mtx_plain
        )
    ) {    
        ref_q_destruct(&con->queue);
        if (0 != close(con->recv_event_fd)) {
            
        }
        return ERR;
    }

    cnd_t* conditionals[] = {
        &con->send_event,
        &con->recv_event
    };

    for (int i = 0; i < ARRAY_SIZE(conditionals); i++) {
        cnd_t* cnd = conditionals[i];
        if (thrd_success != cnd_init(cnd)) {

            if (i >= 1) {
                cnd_destroy(conditionals[0]);
            }

            mtx_destroy(&con->mtx);
            ref_q_destruct(&con->queue);
            if (0 != close(con->recv_event_fd)) {
                
            }

            return ERR;
        } 
    }
    

    return OK;
}


static enum OKorERR relenquish_mtx_ret(
    mtx_t* mtx,
    const enum OKorERR ret_val)
{
    if (thrd_success != mtx_unlock(mtx)) {
        return ERR;
    }
    return ret_val;
}

static struct timespec ms2ts(
    unsigned long ms)
{
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000
    };
    return ts;
}

enum OKorERR conduit_recv_msg(
    struct Conduit* con,
    void** message)
{
    if (thrd_success != mtx_lock(&con->mtx)) {
        return ERR;
    }
   
    while (ref_q_empty(&con->queue)) { 
        if (con->closed) {
            return relenquish_mtx_ret(&con->mtx, ERR);
        }

        con->awaiting_recievers++;

        
        bool err = false;

        /*
            The pthread_cond_wait() routine always returns with the mutex locked
            and owned by the calling thread, even when returning an error.
        */

        const int wait =
            cnd_wait(
                &con->recv_event,
                &con->mtx
            );

        switch (wait) {
            /*case thrd_timedout:
                *timed_out = true;
                break;*/
            case thrd_success:
                break;
            case thrd_error:
                err: err = true;
                break;
            default:
                exit(1);
                goto err;
        }

        con->awaiting_recievers--;

        if (err) {
            return relenquish_mtx_ret(&con->mtx, ERR);
        }
    }

    void* msg = NULL;
    const enum RefQueueOp dequeue =
        ref_q_dequeue(
            &con->queue,
            &msg
        );
    
    if (dequeue == REFQ_OK && NULL != message) {
        *message = msg;
    }

    if (con->awaiting_senders > 0) {
        if (thrd_success != cnd_signal(&con->send_event)) {
            return relenquish_mtx_ret(&con->mtx, ERR);
        }
    }

    if (REFQ_OK == dequeue 
    && 0 !=
        eventfd_read(
            con->recv_event_fd,
            NULL
        )
    ) {
        return relenquish_mtx_ret(&con->mtx, ERR);
    }

    return
        relenquish_mtx_ret(&con->mtx, OK) == OK 
        && REFQ_OK == dequeue
        ? OK
        : ERR;
}

enum OKorERR conduit_send_msg(
    struct Conduit* con,
    void* message)
{
    if (thrd_success != mtx_lock(&con->mtx)) {
        return ERR;
    }

    while (ref_q_full(&con->queue)) {

        con->awaiting_senders++;

        bool err = false;
        
        /*
            The pthread_cond_wait() routine always returns with the mutex locked
            and owned by the calling thread, even when returning an error.
        */
        if (thrd_success !=
            cnd_wait(
                &con->recv_event,
                &con->mtx
            )
        ) {
            err = true;
        }

        con->awaiting_senders--;
        
        if (err) {
            return relenquish_mtx_ret(&con->mtx, ERR);
        }
    }

    /* technically, can never return error if the queue loops while full*/
    const enum RefQueueOp enq =
        ref_q_enqueue(
            &con->queue,
            message
        );

    if (enq == REFQ_OK && con->awaiting_recievers > 0) {
        if (thrd_success !=
            cnd_signal(&con->recv_event)
        ) {
            return relenquish_mtx_ret(&con->mtx, ERR);
        }
    }

    if (REFQ_OK == enq 
    && 0 !=
        eventfd_write(
            con->recv_event_fd,
            1
        )
    ) {
        return relenquish_mtx_ret(&con->mtx, ERR);
    }
    
    return
        relenquish_mtx_ret(&con->mtx, OK) == OK
        && enq == REFQ_OK
        ? OK
        : ERR;
}