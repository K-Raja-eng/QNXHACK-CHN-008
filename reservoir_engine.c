/*
 * reservoir_engine.c
 * Main digital-twin/data-fusion process.
 *
 * Threads:
 *   1. Prediction thread: 100 ms cycle
 *   2. Health thread:      1 s cycle
 *
 * Synchronization:
 *   - process-shared mutex for shared-memory input/output
 *   - semaphore used to confirm both worker threads have started
 *
 * The mathematical model is intentionally educational, not a dam operating model.
 * It produces a prototype advisory for demonstration and must not be connected to a
 * real gate controller.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <math.h>
#include "../include/reservoir_state.h"

static ShmHandle input_shm = { .fd = -1 };
static ShmHandle twin_shm = { .fd = -1 };
static ReservoirInputBus *input_bus;
static ReservoirTwinState *twin;
static volatile int running = 1;
static sem_t startup_sem;

static float clampf(float x, float lo, float hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static float storage_from_level(float level)
{
    return clampf((level - MIN_LEVEL_M) * 100.0f / (MAX_LEVEL_M - MIN_LEVEL_M), 0.0f, 100.0f);
}

static void calculate_state(const ReservoirInputBus *in, ReservoirTwinPayload *out)
{
    const ArduinoSample *a = &in->arduino;
    const WeatherSample *w = &in->weather;
    const UpstreamSample *u = &in->upstream;
    const SimSensorSample *sim = &in->simulator;
    uint64_t now = rt_now_ns();

    memset(out, 0, sizeof(*out));
    out->reservoir_level_m = a->valid ? a->reservoir_level_m : 6.0f;
    out->storage_percent = storage_from_level(out->reservoir_level_m);
    out->rainfall_mmph = a->valid ? a->rainfall_mmph : w->rain_now_mmph;
    out->gate_percent = a->valid ? a->gate_percent : 0.0f;
    out->downstream_level_m = a->valid ? a->downstream_level_m : sim->virtual_downstream_m;

    /* Direct Arduino temperature/emergency are useful even when the
       reservoir-specific REAL frame is not supplied. */
    out->temperature_c = (a->valid || a->aux_valid) ? a->temperature_c : 28.0f;
    out->emergency = (a->valid || a->aux_valid) ? (uint32_t)a->emergency : 0u;

    out->arduino_mic1 = a->mic1;
    out->arduino_mic2 = a->mic2;
    out->arduino_gas = a->gas;
    out->arduino_ir = a->ir;
    out->arduino_humidity = a->humidity;
    out->arduino_aux_valid = a->aux_valid;

    out->upstream_inflow_m3s = u->valid ? u->inflow_m3s : 0.0f;
    out->upstream_release_m3s = u->valid ? u->release_m3s : 0.0f;

    out->rain_1h_mm = w->valid ? w->rain_1h_mm : 0.0f;
    out->rain_6h_mm = w->valid ? w->rain_6h_mm : 0.0f;
    out->rain_12h_mm = w->valid ? w->rain_12h_mm : 0.0f;
    out->rain_24h_mm = w->valid ? w->rain_24h_mm : 0.0f;
    out->rain_72h_mm = w->valid ? w->rain_72h_mm : 0.0f;
    out->weather_confidence_percent = w->valid ? w->confidence_percent : 0.0f;

    /* Educational one-hour inflow estimate: current upstream + rainfall contribution. */
    float rain_factor = clampf(out->rain_6h_mm / 100.0f, 0.0f, 2.5f);
    out->predicted_inflow_m3s = out->upstream_inflow_m3s * (1.0f + 0.25f * rain_factor)
                              + 2.0f * out->rainfall_mmph
                              + 0.10f * out->upstream_release_m3s;

    float net_flow = out->predicted_inflow_m3s - out->upstream_release_m3s;
    float delta_level = net_flow * PREDICTION_HORIZON_S / EFFECTIVE_AREA_M2;
    out->predicted_level_m = clampf(out->reservoir_level_m + delta_level, 0.0f, MAX_LEVEL_M + 2.0f);
    out->predicted_storage_percent = storage_from_level(out->predicted_level_m);

    /* Prototype advisory logic: threshold-based status, not a real dam command. */
    if (out->predicted_storage_percent >= 95.0f || out->downstream_level_m >= DOWNSTREAM_LIMIT_M) {
        out->flood_risk_level = FLOOD_CRITICAL;
    } else if (out->predicted_storage_percent >= 85.0f || out->rain_24h_mm >= 300.0f) {
        out->flood_risk_level = FLOOD_HIGH;
    } else if (out->predicted_storage_percent >= 70.0f || out->rain_24h_mm >= 150.0f) {
        out->flood_risk_level = FLOOD_MODERATE;
    } else {
        out->flood_risk_level = FLOOD_NORMAL;
    }

    if (out->emergency) {
        out->release_advisory = ADVISORY_EMERGENCY;
    } else if (out->flood_risk_level >= FLOOD_CRITICAL) {
        out->release_advisory = ADVISORY_EMERGENCY;
    } else if (out->flood_risk_level == FLOOD_HIGH) {
        out->release_advisory = ADVISORY_REVIEW;
    } else if (out->flood_risk_level == FLOOD_MODERATE) {
        out->release_advisory = ADVISORY_PREPARE;
    } else {
        out->release_advisory = ADVISORY_NORMAL;
    }

    if (out->release_advisory == ADVISORY_NORMAL) {
        out->recommended_release_m3s = 0.0f;
    } else {
        float demand = out->predicted_inflow_m3s *
                       (out->release_advisory == ADVISORY_EMERGENCY ? 0.90f :
                        out->release_advisory == ADVISORY_REVIEW ? 0.70f : 0.45f);
        out->recommended_release_m3s = clampf(demand, 0.0f, DESIGN_RELEASE_M3S);
    }

    out->source_flags = 0;
    if (a->valid || a->aux_valid) out->source_flags |= SOURCE_ARDUINO;
    if (sim->valid) out->source_flags |= SOURCE_SIM;
    if (w->valid) out->source_flags |= SOURCE_WEATHER;
    if (u->valid) out->source_flags |= SOURCE_UPSTREAM;

    out->stale_flags = 0;
    const uint64_t timeout_ns = 3000000000ULL;
    if ((!a->valid && !a->aux_valid) || now - a->timestamp_ns > timeout_ns) out->stale_flags |= SOURCE_ARDUINO;
    if (!sim->valid || now - sim->timestamp_ns > timeout_ns) out->stale_flags |= SOURCE_SIM;
    if (!w->valid || now - w->timestamp_ns > 3000000000ULL) out->stale_flags |= SOURCE_WEATHER;
    if (!u->valid || now - u->timestamp_ns > 3000000000ULL) out->stale_flags |= SOURCE_UPSTREAM;
    out->health = (out->stale_flags == 0u) ? HEALTH_OK : HEALTH_STALE;

    out->timestamp_ns = now;
    out->sequence = now / 100000000ULL;
    out->magic = STATE_MAGIC;
    out->version = STATE_VERSION;
}

static void *prediction_thread(void *arg)
{
    (void)arg;
    ReservoirInputBus local;
    memset(&local, 0, sizeof(local));
    sem_post(&startup_sem);

    while (running) {
        input_copy(input_bus, &local);
        ReservoirTwinPayload next;
        calculate_state(&local, &next);

        pthread_mutex_lock(&twin->lock);
        /* Preserve timer telemetry generated by the timing process. */
        uint64_t fast = twin->data.fast_count;
        uint64_t med = twin->data.med_count;
        uint64_t slow = twin->data.slow_count;
        uint64_t last_j = twin->data.last_jitter_ns;
        uint64_t max_j = twin->data.max_jitter_ns;
        uint64_t miss = twin->data.deadline_miss_count;
        twin->data = next;
        twin->data.fast_count = fast;
        twin->data.med_count = med;
        twin->data.slow_count = slow;
        twin->data.last_jitter_ns = last_j;
        twin->data.max_jitter_ns = max_j;
        twin->data.deadline_miss_count = miss;
        pthread_mutex_unlock(&twin->lock);

        usleep(100000);
    }
    return NULL;
}

static void *health_thread(void *arg)
{
    (void)arg;
    sem_post(&startup_sem);
    while (running) {
        ReservoirTwinPayload snap;
        twin_snapshot(twin, &snap);
        uint32_t health = snap.health;
        uint32_t risk = snap.flood_risk_level;
        uint32_t stale = snap.stale_flags;
        printf("[HEALTH] health=%u risk=%u stale=0x%X\n", health, risk, stale);
        fflush(stdout);
        sleep(1);
    }
    return NULL;
}

static void signal_handler(int sig)
{
    (void)sig;
    running = 0;
}

int main(int argc, char *argv[])
{
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    int create = (argc > 1 && strcmp(argv[1], "--create") == 0);

    printf("[ENGINE] QNX Reservoir Digital Twin\n");
    printf("[ENGINE] Educational fusion model + protected shared memory\n");

    if (sem_init(&startup_sem, 0, 0) != 0) {
        perror("sem_init");
        return 1;
    }

    if (create) {
        ReservoirInputBus *ib = NULL;
        ReservoirTwinState *tw = NULL;
        if (input_bus_create(&input_shm, &ib) != 0 || twin_create(&twin_shm, &tw) != 0) {
            perror("shared memory create");
            return 1;
        }
        input_bus = ib;
        twin = tw;
    } else {
        if (input_bus_open(&input_shm, &input_bus) != 0 || twin_open(&twin_shm, &twin) != 0) {
            fprintf(stderr, "[ENGINE] Start once with --create first.\n");
            return 1;
        }
    }

    pthread_t prediction;
    pthread_t health;
    if (pthread_create(&prediction, NULL, prediction_thread, NULL) != 0) return 1;
    if (pthread_create(&health, NULL, health_thread, NULL) != 0) return 1;

    /* Wait for both threads to announce they are running. */
    sem_wait(&startup_sem);
    sem_wait(&startup_sem);
    printf("[ENGINE] Worker threads started: prediction=100ms health=1s\n");

    pthread_join(prediction, NULL);
    pthread_join(health, NULL);
    sem_destroy(&startup_sem);
    shm_close(&input_shm);
    shm_close(&twin_shm);
    return 0;
}
