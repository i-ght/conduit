#include <stdbool.h>
#include <threads.h>
#include "okorerr.h"
#include "reference_queue.h"
#include <sys/eventfd.h>

/* Inspired by: /tylertreat/chan */

struct UnbufConduit
{
    mtx_t mtx;
    mtx_t recv_mtx;
    mtx_t send_mtx;

    cnd_t recv_event;
    cnd_t send_event;
    
    void* data;

    bool closed;

    uint32_t awaiting_receivers;
    uint32_t awaiting_senders;
    int recv_event_fd;
};

enum OKorERR unbuf_conduit_construct(
    struct UnbufConduit* asy_con
);

enum OKorERR unbuf_conduit_destruct(
    struct UnbufConduit* asy_con
);

enum OKorERR unbuf_conduit_send_msg(
    struct UnbufConduit* asy_con,
    void* message
);

enum OKorERR unbuf_conduit_recv_msg(
    struct UnbufConduit* asy_con,
    void** message
);


struct Conduit
{
    struct RefQueue queue;
    mtx_t mtx;
    cnd_t recv_event;
    cnd_t send_event;
    uint32_t awaiting_recvrs;
    uint32_t awaiting_sendrs;
    bool closed;
    uint32_t capacity;
    int recv_event_fd;
};

int conduit_recv_event_fd(
    const struct Conduit* con
);

enum OKorERR conduit_destruct(
    struct Conduit* con
);

enum OKorERR conduit_construct(
    struct Conduit* con,
    const size_t capacity,
    const MemAlloc mem_alloc
);

enum OKorERR conduit_recv_msg(
    struct Conduit* con,
    void** message
);

enum OKorERR conduit_send_msg(
    struct Conduit* con,
    void* message
);