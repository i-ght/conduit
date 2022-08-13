#define DEBUG

#include <sys/eventfd.h>
#include <unistd.h>

#include "conduit.h"

enum {ERR=-1, OK=0};

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

static int relenquish_mtx_ret(
    mtx_t* mtx,
    const int ret_val)
{
    if (thrd_success != mtx_unlock(mtx)) {
        return ERR;
    }
    return ret_val;
}


static void close_fd_ignore_error(
    int fd)
{
    if (0 != close(fd)) {
#ifdef DEBUG
        perror("failed to close fd");
        exit(1);
#endif
    }
}

static int release_all_mutexs_or_reterr(
    mtx_t** array,
    const size_t count)
{
    for (int i = 0; i < count; i++) {

        if (NULL != array[i]) {
            if (thrd_success != mtx_unlock(array[i])) {

#ifdef DEBUG
                perror("failed to release mutex\n");
                exit(1);
#endif

                return ERR;
            };
        }
    }

    return OK;
}


static int acquire_all_mutexs_or_relenquish(
    mtx_t** array,
    const size_t count)
{
    uint32_t acquired = 0;
    for (int i = 0; i < count; i++) {
        if (NULL != array[i]) {
            if (thrd_success != mtx_lock(array[i])) {
                const int _ =
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

int unbuf_conduit_close(
    struct UnbufConduit* unbuf_con)
{
    if (thrd_success != mtx_lock(&unbuf_con->mtx)) {
        return ERR;
    }

    if (unbuf_con->closed) {
        return
            relenquish_mtx_ret(
                &unbuf_con->mtx,
                ERR
            );
    }

    unbuf_con->closed = true;

    cnd_t* conditions_to_broadcast[] = {
        &unbuf_con->recv_event,
        &unbuf_con->send_event
    };

    for (int i = 0; i < ARRAY_SIZE(conditions_to_broadcast); i++) {
        if (thrd_success != cnd_broadcast(conditions_to_broadcast[i])) {
            return
                relenquish_mtx_ret(
                    &unbuf_con->mtx,
                    ERR
                );
        }
    }

    return
        relenquish_mtx_ret(
            &unbuf_con->mtx,
            OK
        );
}

int unbuf_conduit_destruct(
    struct UnbufConduit* unbuf_con)
{
    
    mtx_t* mutexs_to_destruct[] = {
        &unbuf_con->mtx,
        &unbuf_con->recv_mtx,
        &unbuf_con->send_mtx
    };

    destruct_mutexs(
        mutexs_to_destruct,
        ARRAY_SIZE(mutexs_to_destruct)
    );

    cnd_t* conditionals_to_destruct[] = {
        &unbuf_con->send_event,
        &unbuf_con->recv_event
    };
    destruct_conditionals(
        conditionals_to_destruct,
        ARRAY_SIZE(conditionals_to_destruct)
    );


    if (0 != close(unbuf_con->recv_event_fd)) {
        return ERR;
    }

    return OK;
}

int unbuf_conduit_recv_msg(
    struct UnbufConduit* unbuf_con,
    void** message)
{
    mtx_t* mutexs_to_acquire[] = {
        &unbuf_con->recv_mtx,
        &unbuf_con->mtx
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
        &unbuf_con->recv_mtx,
        &unbuf_con->mtx
    };

    bool err = false;
    while (!unbuf_con->closed && unbuf_con->awaiting_senders == 0) {
        unbuf_con->awaiting_receivers++;  

        /* await the event that there's a message to receive */
        if (thrd_success !=
            cnd_wait(
                &unbuf_con->recv_event,
                &unbuf_con->mtx
            )
        ) {
            const int _ =
                release_all_mutexs_or_reterr(
                    mutexs_to_release,
                    ARRAY_SIZE(mutexs_to_release)
                );

            err = true;
        }

        unbuf_con->awaiting_receivers--;

        if (err) {
            return ERR;
        }
    }

    if (unbuf_con->closed) {
        const int _ =
            release_all_mutexs_or_reterr(
                mutexs_to_release,
                ARRAY_SIZE(mutexs_to_release)
            );
        return ERR;
    }

    if (NULL != message) {
        *message = unbuf_con->data;
        unbuf_con->data = NULL;
    }

    unbuf_con->awaiting_senders--;

    /* EVENT: message consumed, now we're writeable again */
    if (thrd_success != cnd_signal(&unbuf_con->send_event)) {
        const int _ =
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

int unbuf_conduit_send_msg(
    struct UnbufConduit* unbuf_con,
    void* message)
{
    mtx_t* mutexs_to_acquire[] = {
        &unbuf_con->send_mtx,
        &unbuf_con->mtx
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
        &unbuf_con->send_mtx,
        &unbuf_con->mtx
    };

    if (unbuf_con->closed) {
        const int _ =
            release_all_mutexs_or_reterr(
                mutexs_to_release,
                ARRAY_SIZE(mutexs_to_release)
            );

        return ERR;
    }

    unbuf_con->data = message;
    unbuf_con->awaiting_senders++;

    if (unbuf_con->awaiting_receivers > 0) {
        /* EVENT: there's a message to receive now. */
        if (thrd_success != cnd_signal(&unbuf_con->recv_event)) {
            const int _ = 
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
            &unbuf_con->send_event,
            &unbuf_con->mtx)
        ) {
            const int _ = 
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

int unbuf_conduit_construct(
    struct UnbufConduit* unbuf_con)
{
    mtx_t* mutexs_to_construct[] = {
        &unbuf_con->mtx,
        &unbuf_con->recv_mtx,
        &unbuf_con->send_mtx
    };

    #define MUTEX_COUNT ARRAY_SIZE(mutexs_to_construct)
    
    mtx_t* mutexs_to_destruct[MUTEX_COUNT] = {0};

    int err_cnt = 0;
    for (int i = 0; i < ARRAY_SIZE(mutexs_to_construct); i++) {

        if (thrd_success !=
            mtx_init(
                mutexs_to_construct[i],
                mtx_plain
            )
        ) {
            mutexs_to_destruct[i] = mutexs_to_construct[i];
            err_cnt++;
            continue;
        }
    }

    if (err_cnt > 0) {
        destruct_mutexs(
            mutexs_to_destruct,
            ARRAY_SIZE(mutexs_to_construct)
        );
        return ERR;
    }

    cnd_t* conditionals_to_construct[] = {
        &unbuf_con->send_event,
        &unbuf_con->recv_event
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

            close_fd_ignore_error(unbuf_con->recv_event_fd);

            return ERR;
        } 
    }

    unbuf_con->closed = false;
    unbuf_con->awaiting_receivers = 0;
    unbuf_con->awaiting_senders = 0;
    unbuf_con->data = NULL;

    return OK;
}

int conduit_destruct(
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

int conduit_construct(
    struct Conduit* con,
    const size_t capacity,
    const MemoryAllocate mem_alloc)
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
        close_fd_ignore_error(con->recv_event_fd);
        return ERR;
    }

    cnd_t* conditionals[] = {
        &con->send_event,
        &con->recv_event
    };

    for (int i = 0; i < ARRAY_SIZE(conditionals); i++) {
        cnd_t* cnd = conditionals[i];
        if (thrd_success != cnd_init(cnd)) {

            if (1 == i) {
                cnd_destroy(conditionals[0]);
            }

            mtx_destroy(&con->mtx);
            ref_q_destruct(&con->queue);
            close_fd_ignore_error(con->recv_event_fd);

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

int conduit_recv_msg(
    struct Conduit* con,
    void** message)
{
    if (thrd_success != mtx_lock(&con->mtx)) {
        return ERR;
    }
   
    while (ref_q_empty(&con->queue)) { 
        if (con->closed) {
            return
                relenquish_mtx_ret(
                    &con->mtx,
                    ERR
                );
        }

        con->awaiting_recvrs++;

        
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

        con->awaiting_recvrs--;

        if (err) {
            return relenquish_mtx_ret(&con->mtx, ERR);
        }
    }

    void* msg = NULL;
    const int dequeue =
        ref_q_dequeue(
            &con->queue,
            &msg
        );
    
    if (dequeue == OK && NULL != message) {
        *message = msg;
    }

    if (con->awaiting_sendrs > 0) {
        if (thrd_success != cnd_signal(&con->send_event)) {
            return relenquish_mtx_ret(&con->mtx, ERR);
        }
    }

    if (OK == dequeue 
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
        && OK == dequeue
        ? OK
        : ERR;
}

int conduit_send_msg(
    struct Conduit* con,
    void* message)
{
    if (thrd_success != mtx_lock(&con->mtx)) {
        return ERR;
    }

    while (ref_q_full(&con->queue)) {

        con->awaiting_sendrs++;

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

        con->awaiting_sendrs--;
        
        if (err) {
            return relenquish_mtx_ret(&con->mtx, ERR);
        }
    }

    /* technically, can never return error if the queue loops while full*/
    const int enq =
        ref_q_enqueue(
            &con->queue,
            message
        );

    if (enq == OK && con->awaiting_recvrs > 0) {
        if (thrd_success !=
            cnd_signal(&con->recv_event)
        ) {
            return
                relenquish_mtx_ret(
                    &con->mtx,
                    ERR
                );
        }
    }

    if (OK == enq 
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
        && enq == OK
        ? OK
        : ERR;
}