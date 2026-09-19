/*
 * rtos_timer_engine.c
 * Demonstrates QNX periodic timing using timer_create + SIGEV_PULSE + MsgReceivePulse.
 * FAST=100 Hz, MED=20 Hz, SLOW=1 Hz.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/neutrino.h>
#include "../include/reservoir_state.h"

#define FAST_CODE (_PULSE_CODE_MINAVAIL)
#define MED_CODE  (_PULSE_CODE_MINAVAIL + 1)
#define SLOW_CODE (_PULSE_CODE_MINAVAIL + 2)
#define QUIT_CODE (_PULSE_CODE_MINAVAIL + 3)

#define FAST_PERIOD_NS 10000000ULL
#define MED_PERIOD_NS  50000000ULL
#define SLOW_PERIOD_NS 1000000000ULL
#define JITTER_LIMIT_NS 1000000ULL

typedef struct {
    uint64_t last_fire;
    uint64_t period;
    uint64_t count;
    uint64_t max_jitter;
    uint64_t misses;
} TimerStat;

static ShmHandle twin_shm = { .fd = -1 };
static ReservoirTwinState *twin = NULL;
static volatile int running = 1;
static TimerStat stat_fast, stat_med, stat_slow;

static uint64_t jitter_from(TimerStat *s, uint64_t now)
{
    if (s->last_fire == 0) {
        s->last_fire = now;
        return 0;
    }
    uint64_t expected = s->last_fire + s->period;
    uint64_t j = (now > expected) ? (now - expected) : (expected - now);
    s->last_fire = now;
    s->count++;
    if (j > s->max_jitter) s->max_jitter = j;
    if (j > JITTER_LIMIT_NS) s->misses++;
    return j;
}

static timer_t create_periodic_timer(int coid, int code, uint64_t ns)
{
    struct sigevent event;
    struct itimerspec value;
    timer_t id;

    SIGEV_PULSE_INIT(&event, coid, SIGEV_PULSE_PRIO_INHERIT, code, 0);
    if (timer_create(CLOCK_MONOTONIC, &event, &id) != 0) {
        perror("timer_create");
        return (timer_t)-1;
    }
    memset(&value, 0, sizeof(value));
    value.it_value.tv_sec = (time_t)(ns / 1000000000ULL);
    value.it_value.tv_nsec = (long)(ns % 1000000000ULL);
    value.it_interval = value.it_value;
    if (timer_settime(id, 0, &value, NULL) != 0) {
        perror("timer_settime");
        timer_delete(id);
        return (timer_t)-1;
    }
    return id;
}

static void update_timing(uint64_t jitter, int which)
{
    pthread_mutex_lock(&twin->lock);
    twin->data.last_jitter_ns = jitter;
    if (jitter > twin->data.max_jitter_ns) twin->data.max_jitter_ns = jitter;
    twin->data.deadline_miss_count = stat_fast.misses + stat_med.misses + stat_slow.misses;
    if (which == 0) twin->data.fast_count++;
    if (which == 1) twin->data.med_count++;
    if (which == 2) twin->data.slow_count++;
    pthread_mutex_unlock(&twin->lock);
}

static void on_signal(int sig) { (void)sig; running = 0; }

int main(void)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (twin_open(&twin_shm, &twin) != 0) {
        fprintf(stderr, "[TIMER] /reservoir_twin not found. Start reservoir_engine --create first.\n");
        return 1;
    }

    int chid = ChannelCreate(0);
    if (chid < 0) { perror("ChannelCreate"); return 1; }
    int coid = ConnectAttach(ND_LOCAL_NODE, 0, chid, _NTO_SIDE_CHANNEL, 0);
    if (coid < 0) { perror("ConnectAttach"); return 1; }

    stat_fast.period = FAST_PERIOD_NS;
    stat_med.period = MED_PERIOD_NS;
    stat_slow.period = SLOW_PERIOD_NS;

    timer_t tf = create_periodic_timer(coid, FAST_CODE, FAST_PERIOD_NS);
    timer_t tm = create_periodic_timer(coid, MED_CODE, MED_PERIOD_NS);
    timer_t ts = create_periodic_timer(coid, SLOW_CODE, SLOW_PERIOD_NS);
    if (tf == (timer_t)-1 || tm == (timer_t)-1 || ts == (timer_t)-1) return 1;

    printf("[TIMER] FAST=100Hz MED=20Hz SLOW=1Hz using SIGEV_PULSE\n");

    struct _pulse pulse;
    while (running) {
        int rc = MsgReceivePulse(chid, &pulse, sizeof(pulse), NULL);
        if (rc < 0) {
            if (errno == EINTR) continue;
            perror("MsgReceivePulse");
            break;
        }
        uint64_t now = rt_now_ns();
        if (pulse.code == FAST_CODE) {
            uint64_t j = jitter_from(&stat_fast, now);
            update_timing(j, 0);
        } else if (pulse.code == MED_CODE) {
            uint64_t j = jitter_from(&stat_med, now);
            update_timing(j, 1);
        } else if (pulse.code == SLOW_CODE) {
            uint64_t j = jitter_from(&stat_slow, now);
            update_timing(j, 2);
            pthread_mutex_lock(&twin->lock);
            printf("[TIMER] 1Hz seq=%llu risk=%u jitter=%llu ns max=%llu ns misses=%llu\n",
                   (unsigned long long)twin->data.sequence,
                   twin->data.flood_risk_level,
                   (unsigned long long)twin->data.last_jitter_ns,
                   (unsigned long long)twin->data.max_jitter_ns,
                   (unsigned long long)twin->data.deadline_miss_count);
            pthread_mutex_unlock(&twin->lock);
            fflush(stdout);
        }
    }

    timer_delete(tf);
    timer_delete(tm);
    timer_delete(ts);
    ConnectDetach(coid);
    ChannelDestroy(chid);
    shm_close(&twin_shm);
    return 0;
}
