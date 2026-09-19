/* Native QNX synchronous IPC client: name_open() + MsgSend(). */
#include <stdio.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include "../include/reservoir_state.h"

#define SERVER_NAME "reservoir_operator"
#define REQ_STATUS 1

typedef struct { int type; } Request;
typedef struct {
    int ok;
    float level;
    float storage;
    float predicted_level;
    uint32_t risk;
    uint32_t advisory;
} Reply;

int main(void)
{
    int coid = -1;
    for (int attempt = 0; attempt < 10 && coid < 0; ++attempt) {
        coid = name_open(SERVER_NAME, 0);
        if (coid < 0) sleep(1);
    }
    if (coid < 0) { perror("name_open"); return 1; }

    Request req = { .type = REQ_STATUS };
    Reply reply;
    memset(&reply, 0, sizeof(reply));
    if (MsgSend(coid, &req, sizeof(req), &reply, sizeof(reply)) < 0) {
        perror("MsgSend");
        name_close(coid);
        return 1;
    }

    printf("[CLIENT] level=%.2fm storage=%.1f%% predicted=%.2fm risk=%u advisory=%u\n",
           reply.level, reply.storage, reply.predicted_level,
           reply.risk, reply.advisory);
    name_close(coid);
    return 0;
}
