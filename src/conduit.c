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

static enum OKorERR relenquish_mtx_ret(
    mtx_t* mtx,
    const enum OKorERR ret_val)
{
    if (thrd_success != mtx_unlock(mtx)) {
        return ERR;
    }
    return ret_val;
}


static enum OKorERR release_all_mutexs_or_reterr(
    mtx_t** array,
    const size_t count)
{
    for (int i = 0; i < count; i++) {

        if (NULL != array[i]) {
            if (thrd_success != mtx_unlock(array[i])) {

#if DEBUG
                perror("failed to release mutex\n");
                exit(1);
#endif

                return ERR;
            };
        }
    }

    return OK;
}


static enum OKorERR acquire_all_mutexs_or_relenquish(
    mtx_t** array,
    const size_t count)
{
    uint32_t acquired = 0;
    for (int i = 0; i < count; i++) {
        if (NULL != array[i]) {
            if (thrd_success != mtx_lock(array[i])) {
                const enum OKorERR _ =
                    release_all_mutexs_or_reterr(
                        array,
                        acquired
                    );
                return ERR;
            }

            acquired++;
        }
    }

    return OK;
}

enum OKorERR async_conduit_close(
    struct AsyncConduit* asy_con)
{
    if (thrd_success != mtx_lock(&asy_con->mtx)) {
        return ERR;
    }

    if (asy_con->closed) {
        return
            relenquish_mtx_ret(
                &asy_con->mtx,
                ERR
            );
    }

    asy_con->closed = true;

    cnd_t* conditions_to_broadcast[] = {
        &asy_con->recv_event,
        &asy_con->send_event
    };

    for (int i = 0; i < ARRAY_SIZE(conditions_to_broadcast); i++) {
        if (thrd_success != cnd_broadcast(conditions_to_broadcast[i])) {
            return
                relenquish_mtx_ret(
                    &asy_con->mtx,
                    ERR
                );
        }
    }

    return
        relenquish_mtx_ret(
            &asy_con->mtx,
            OK
        );
}

enum OKorERR async_conduit_destruct(
    struct AsyncConduit* asy_con)
{
    
    mtx_t* mutexs_to_destruct[] = {
        &asy_con->mtx,
        &asy_con->recv_mtx,
        &asy_con->send_mtx
    };

    destruct_mutexs(
        mutexs_to_destruct,
        ARRAY_SIZE(mutexs_to_destruct)
    );

    cnd_t* conditionals_to_destruct[] = {
        &asy_con->send_event,
        &asy_con->recv_event
    };
    destruct_conditionals(
        conditionals_to_destruct,
        ARRAY_SIZE(conditionals_to_destruct)
    );


    if (0 != close(asy_con->recv_event_fd)) {
        return ERR;
    }

    return OK;
}

enum OKorERR async_conduit_recv_msg(
    struct AsyncConduit* asy_con,
    void** message)
{
    mtx_t* mutexs_to_acquire[] = {
        &asy_con->recv_mtx,
        &asy_con->mtx
    };

    if (OK !=
        acquire_all_mutexs_or_relenquish(
            mutexs_to_acquire,
            ARRAY_SIZE(mutexs_to_acquire)
        )
    ) {
        return ERR;
    }

    mtx_t* mutexs_to_release[] = {
        &asy_con->recv_mtx,
        &asy_con->mtx
    };

    bool err = false;
    while (!asy_con->closed && asy_con->awaiting_senders == 0) {
        asy_con->awaiting_receivers++;  

        /* await the event that there's a message to receive */
        if (thrd_success !=
            cnd_wait(
                &asy_con->recv_event,
                &asy_con->mtx
            )
        ) {
            const enum OKorERR _ =
                release_all_mutexs_or_reterr(
                    mutexs_to_release,
                    ARRAY_SIZE(mutexs_to_release)
                );

            err = true;
        }

        asy_con->awaiting_receivers--;

        if (err) {
            return ERR;
        }
    }

    if (asy_con->closed) {
        const enum OKorERR _ =
            release_all_mutexs_or_reterr(
                mutexs_to_release,
                ARRAY_SIZE(mutexs_to_release)
            );
        return ERR;
    }

    if (NULL != message) {
        *message = asy_con->data;
        asy_con->data = NULL;
    }

    asy_con->awaiting_senders--;

    /* EVENT: message consumed, now we're writeable again */
    if (thrd_success != cnd_signal(&asy_con->send_event)) {
        const enum OKorERR _ =
            release_all_mutexs_or_reterr(
                mutexs_to_release,
                ARRAY_SIZE(mutexs_to_release)
            );
        return ERR;
    } 

    return release_all_mutexs_or_reterr(
        mutexs_to_release,
        ARRAY_SIZE(mutexs_to_release)
    );
}

enum OKorERR async_conduit_send_msg(
    struct AsyncConduit* asy_con,
    void* message)
{
    mtx_t* mutexs_to_acquire[] = {
        &asy_con->send_mtx,
        &asy_con->mtx
    };

    if (OK !=
        acquire_all_mutexs_or_relenquish(
            mutexs_to_acquire,
            ARRAY_SIZE(mutexs_to_acquire)
        )
    ) {
        return ERR;
    }

    mtx_t* mutexs_to_release[] = {
        &asy_con->send_mtx,
        &asy_con->mtx
    };

    if (asy_con->closed) {
        const enum OKorERR _ =
            release_all_mutexs_or_reterr(
                mutexs_to_release,
                ARRAY_SIZE(mutexs_to_release)
            );

        return ERR;
    }

    asy_con->data = message;
    asy_con->awaiting_senders++;

    if (asy_con->awaiting_receivers > 0) {
        /* EVENT: there's a message to receive now. */
        if (thrd_success != cnd_signal(&asy_con->recv_event)) {
            const enum OKorERR _ = 
                release_all_mutexs_or_reterr(
                    mutexs_to_release,
                    ARRAY_SIZE(mutexs_to_release)
                );
            return ERR;
        }
    }

    /* await the event that the message was consumed. */
    if (thrd_success != 
        cnd_wait(
            &asy_con->send_event,
            &asy_con->mtx)
        ) {
            const enum OKorERR _ = 
                release_all_mutexs_or_reterr(
                    mutexs_to_release,
                    ARRAY_SIZE(mutexs_to_release)
                );

            return ERR;
        }

    
    return
        release_all_mutexs_or_reterr(
            mutexs_to_release,
            ARRAY_SIZE(mutexs_to_release)
        );
}

enum OKorERR async_conduit_construct(
    struct AsyncConduit* asy_con)
{
    mtx_t* mutexs_to_construct[] = {
        &asy_con->mtx,
        &asy_con->recv_mtx,
        &asy_con->send_mtx
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

    asy_con->closed = false;
    asy_con->awaiting_receivers = 0;
    asy_con->awaiting_senders = 0;
    asy_con->data = NULL;

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