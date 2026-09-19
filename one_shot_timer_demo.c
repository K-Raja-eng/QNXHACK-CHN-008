/* Unit 4 lab-style demo: POSIX one-shot QNX timer that delivers one pulse. */
#include <stdio.h>
#include <sys/neutrino.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>

#define ONE_SHOT_CODE (_PULSE_CODE_MINAVAIL)

int main(void)
{
    int chid = ChannelCreate(0);
    if (chid < 0) return 1;
    int coid = ConnectAttach(ND_LOCAL_NODE, 0, chid, _NTO_SIDE_CHANNEL, 0);
    if (coid < 0) return 1;

    struct sigevent event;
    SIGEV_PULSE_INIT(&event, coid, SIGEV_PULSE_PRIO_INHERIT, ONE_SHOT_CODE, 0);
    timer_t timer;
    if (timer_create(CLOCK_MONOTONIC, &event, &timer) != 0) return 1;

    struct itimerspec it = {0};
    it.it_value.tv_sec = 3;  /* no interval => one-shot */
    if (timer_settime(timer, 0, &it, NULL) != 0) return 1;

    printf("[ONE-SHOT] waiting 3 seconds...\n");
    struct _pulse pulse;
    for (;;) {
        if (MsgReceivePulse(chid, &pulse, sizeof(pulse), NULL) < 0) return 1;
        if (pulse.code == ONE_SHOT_CODE) {
            printf("[ONE-SHOT] pulse received once; timer stopped automatically.\n");
            break;
        }
    }

    timer_delete(timer); ConnectDetach(coid); ChannelDestroy(chid);
    return 0;
}
