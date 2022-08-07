#include <sys/eventfd.h>

#include <unistd.h>

#include "conduit.h"

enum OKorERR async_conduit_destruct(
    struct AsyncConduit* asy_con)
{

    return OK;
}

enum OKorERR conduit_destruct(
    struct Conduit* con)
{
    ref_q_destruct(&con->queue);  
        
    if (0 != close(con->recv_event_fd)) {
        return ERR;
    }

    mtx_destroy(&con->mtx);

    cnd_t* conditionals[] = {
        &con->send_event,
        &con->recv_event
    };
    for (int i = 0; i < 2; i++) {
        cnd_t* cnd = conditionals[i];
        cnd_destroy(cnd);
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

    con->recv_event_fd = eventfd(0, 0);

    if (thrd_success !=
        mtx_init(
            &con->mtx,
            mtx_plain
        )
    ) {    
        return ERR;
    }

    cnd_t* conditionals[] = {
        &con->send_event,
        &con->recv_event
    };
    for (int i = 0; i < 2; i++) {
        cnd_t* cnd = conditionals[i];
        if (thrd_success != cnd_init(cnd)) {
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

    static const char zero[] = {0};
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