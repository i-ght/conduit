#include "conduit.h"

enum{ERR = -1, OK = 0};

static int thread0(void* arg)
{

    fgetc(stdin);

    struct UnbufConduit* a = (struct UnbufConduit*)arg;

    void* msg = NULL;
    if (ERR ==
        unbuf_conduit_recv_msg(a, &msg)
    ) {
        return ERR;
    }

    const char* greetings = (const char*)msg;
    const int _ = printf("%s\n", greetings);

    return OK;
}

int main(void)
{
    struct UnbufConduit a = {0};
    if (ERR ==
        unbuf_conduit_construct(&a)
    ) {
        return ERR;
    }

    thrd_t t = {0};
    if (thrd_success != thrd_create(&t, thread0, &a)) {
        return ERR;
    }

    if (ERR ==
        unbuf_conduit_send_msg(
            &a,
            "hello world"
        )
    ) {
        return ERR;
    }


    int thread0_result = -23;
    if (thrd_success != thrd_join(t, &thread0_result)) {
        return ERR;
    }

    const int _ = printf("it's okay\n");
    return OK;
}
