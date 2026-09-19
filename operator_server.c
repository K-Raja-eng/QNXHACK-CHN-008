/* Native QNX synchronous IPC server: MsgReceive()/MsgReply(). */
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include "../include/reservoir_state.h"

#define SERVER_NAME "reservoir_operator"
#define REQ_STATUS  1
#define REQ_PING    2

typedef struct { int type; } Request;
typedef struct {
    int ok;
    float level;
    float storage;
    float predicted_level;
    uint32_t risk;
    uint32_t advisory;
} Reply;

static volatile int running = 1;
static void sig_handler(int sig) { (void)sig; running = 0; }

int main(void)
{
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    ShmHandle shm = { .fd = -1 };
    ReservoirTwinState *twin = NULL;
    while (twin_open(&shm, &twin) != 0) sleep(1);

    name_attach_t *attach = name_attach(NULL, SERVER_NAME, 0);
    if (!attach) { perror("name_attach"); return 1; }
    printf("[IPC] Server: /dev/name/local/%s\n", SERVER_NAME);

    while (running) {
        Request req;
        rcvid_t rcvid = MsgReceive(attach->chid, &req, sizeof(req), NULL);
        if (rcvid == -1) continue;
        if (rcvid == 0) continue; /* pulse */

        Reply r;
        memset(&r, 0, sizeof(r));
        if (req.type == REQ_PING) {
            r.ok = 1;
        } else if (req.type == REQ_STATUS) {
            pthread_mutex_lock(&twin->lock);
            r.ok = 1;
            r.level = twin->data.reservoir_level_m;
            r.storage = twin->data.storage_percent;
            r.predicted_level = twin->data.predicted_level_m;
            r.risk = twin->data.flood_risk_level;
            r.advisory = twin->data.release_advisory;
            pthread_mutex_unlock(&twin->lock);
        } else {
            r.ok = 0;
        }
        MsgReply(rcvid, EOK, &r, sizeof(r));
    }

    name_detach(attach, 0);
    shm_close(&shm);
    return 0;
}
