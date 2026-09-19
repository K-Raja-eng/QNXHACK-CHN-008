#ifndef RESERVOIR_STATE_H
#define RESERVOIR_STATE_H

/*
 * QNX Reservoir Digital Twin
 * Shared-memory and data definitions.
 *
 * Course concepts used here:
 *   - shm_open()/mmap() for IPC
 *   - process-shared pthread mutex for synchronization
 *   - POSIX/POSIX-like interfaces used by QNX
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define INPUT_SHM_NAME "/reservoir_input"
#define TWIN_SHM_NAME  "/reservoir_twin"

#define STATE_MAGIC 0x52545631u   /* RTV1 */
#define STATE_VERSION 1u

#define SOURCE_ARDUINO   0x01u
#define SOURCE_SIM       0x02u
#define SOURCE_WEATHER   0x04u
#define SOURCE_UPSTREAM  0x08u

#define FLOOD_NORMAL   0u
#define FLOOD_MODERATE 1u
#define FLOOD_HIGH     2u
#define FLOOD_CRITICAL 3u

#define ADVISORY_NORMAL      0u
#define ADVISORY_PREPARE     1u
#define ADVISORY_REVIEW      2u
#define ADVISORY_EMERGENCY   3u

#define HEALTH_OK        0u
#define HEALTH_STALE     1u
#define HEALTH_INVALID   2u

#define MAX_LEVEL_M 10.0f
#define MIN_LEVEL_M 2.0f
#define EFFECTIVE_AREA_M2 500000.0f
#define PREDICTION_HORIZON_S 3600.0f
#define DESIGN_RELEASE_M3S 900.0f
#define DOWNSTREAM_LIMIT_M 5.0f

static inline uint64_t rt_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Physical Arduino measurements. */
typedef struct {
    /* Existing reservoir-facing Arduino fields. */
    float reservoir_level_m;
    float rainfall_mmph;
    float gate_percent;
    float downstream_level_m;
    float temperature_c;
    int32_t emergency;

    /* Raw Arduino telemetry from the laptop serial bridge. */
    float mic1;
    float mic2;
    float gas;
    float ir;
    float humidity;
    uint32_t aux_valid;

    uint64_t timestamp_ns;
    uint32_t valid;
} ArduinoSample;

/* Extra simulated field measurements used when only a small physical node is available. */
typedef struct {
    float catchment_saturation_percent;
    float soil_moisture_percent;
    float virtual_downstream_m;
    uint64_t timestamp_ns;
    uint32_t valid;
} SimSensorSample;

/* Weather forecast/simulation horizon. */
typedef struct {
    float rain_now_mmph;
    float rain_1h_mm;
    float rain_6h_mm;
    float rain_12h_mm;
    float rain_24h_mm;
    float rain_72h_mm;
    float confidence_percent;
    uint32_t scenario;
    uint64_t timestamp_ns;
    uint32_t valid;
} WeatherSample;

/* Upstream reservoir/river simulation. */
typedef struct {
    float inflow_m3s;
    float release_m3s;
    float water_level_m;
    uint32_t scenario;
    uint64_t timestamp_ns;
    uint32_t valid;
} UpstreamSample;

/* One shared input bus. Multiple producer processes write one protected member each. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    pthread_mutex_t lock;       /* PTHREAD_PROCESS_SHARED */
    uint64_t update_count;
    ArduinoSample arduino;
    SimSensorSample simulator;
    WeatherSample weather;
    UpstreamSample upstream;
} ReservoirInputBus;

/* Final digital-twin payload. Kept separate from the process-shared mutex. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t sequence;
    uint64_t timestamp_ns;

    /* Current measurements */
    float reservoir_level_m;
    float storage_percent;
    float rainfall_mmph;
    float gate_percent;
    float downstream_level_m;
    float temperature_c;

    /* Direct Arduino telemetry for dashboard/diagnostics. */
    float arduino_mic1;
    float arduino_mic2;
    float arduino_gas;
    float arduino_ir;
    float arduino_humidity;
    uint32_t arduino_aux_valid;

    float upstream_inflow_m3s;
    float upstream_release_m3s;

    /* Forecast */
    float rain_1h_mm;
    float rain_6h_mm;
    float rain_12h_mm;
    float rain_24h_mm;
    float rain_72h_mm;
    float weather_confidence_percent;

    /* Prediction */
    float predicted_inflow_m3s;
    float predicted_level_m;
    float predicted_storage_percent;
    float recommended_release_m3s; /* prototype advisory only */

    uint32_t flood_risk_level;
    uint32_t release_advisory;
    uint32_t emergency;
    uint32_t source_flags;
    uint32_t stale_flags;
    uint32_t health;

    /* Timing telemetry written by rtos_timer_engine. */
    uint64_t fast_count;
    uint64_t med_count;
    uint64_t slow_count;
    uint64_t last_jitter_ns;
    uint64_t max_jitter_ns;
    uint64_t deadline_miss_count;
} ReservoirTwinPayload;

/* Shared wrapper. The mutex itself is never copied between processes. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    pthread_mutex_t lock;       /* PTHREAD_PROCESS_SHARED */
    ReservoirTwinPayload data;
} ReservoirTwinState;

typedef struct {
    int fd;
    void *ptr;
    size_t size;
} ShmHandle;

static inline int init_shared_mutex(pthread_mutex_t *m)
{
    pthread_mutexattr_t attr;
    int rc = pthread_mutexattr_init(&attr);
    if (rc != 0) return rc;
    rc = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    if (rc == 0) rc = pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
    return rc;
}

static inline int shm_create(ShmHandle *h, const char *name, size_t size)
{
    h->fd = shm_open(name, O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (h->fd < 0) return -1;
    if (ftruncate(h->fd, (off_t)size) != 0) {
        close(h->fd);
        h->fd = -1;
        return -1;
    }
    h->ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, h->fd, 0);
    if (h->ptr == MAP_FAILED) {
        close(h->fd);
        h->fd = -1;
        return -1;
    }
    h->size = size;
    memset(h->ptr, 0, size);
    return 0;
}

static inline int shm_open_existing(ShmHandle *h, const char *name, size_t size)
{
    h->fd = shm_open(name, O_RDWR, 0);
    if (h->fd < 0) return -1;
    h->ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, h->fd, 0);
    if (h->ptr == MAP_FAILED) {
        close(h->fd);
        h->fd = -1;
        return -1;
    }
    h->size = size;
    return 0;
}

static inline void shm_close(ShmHandle *h)
{
    if (h->ptr && h->ptr != MAP_FAILED)
        munmap(h->ptr, h->size);
    if (h->fd >= 0)
        close(h->fd);
    h->ptr = NULL;
    h->fd = -1;
    h->size = 0;
}

static inline int input_bus_create(ShmHandle *h, ReservoirInputBus **bus)
{
    if (shm_create(h, INPUT_SHM_NAME, sizeof(ReservoirInputBus)) != 0) return -1;
    *bus = (ReservoirInputBus *)h->ptr;
    (*bus)->magic = STATE_MAGIC;
    (*bus)->version = STATE_VERSION;
    if (init_shared_mutex(&(*bus)->lock) != 0) return -1;
    (*bus)->update_count = 0;
    return 0;
}

static inline int input_bus_open(ShmHandle *h, ReservoirInputBus **bus)
{
    if (shm_open_existing(h, INPUT_SHM_NAME, sizeof(ReservoirInputBus)) != 0) return -1;
    *bus = (ReservoirInputBus *)h->ptr;
    if ((*bus)->magic != STATE_MAGIC || (*bus)->version != STATE_VERSION) return -1;
    return 0;
}

static inline int twin_create(ShmHandle *h, ReservoirTwinState **twin)
{
    if (shm_create(h, TWIN_SHM_NAME, sizeof(ReservoirTwinState)) != 0) return -1;
    *twin = (ReservoirTwinState *)h->ptr;
    (*twin)->magic = STATE_MAGIC;
    (*twin)->version = STATE_VERSION;
    if (init_shared_mutex(&(*twin)->lock) != 0) return -1;
    return 0;
}

static inline int twin_open(ShmHandle *h, ReservoirTwinState **twin)
{
    if (shm_open_existing(h, TWIN_SHM_NAME, sizeof(ReservoirTwinState)) != 0) return -1;
    *twin = (ReservoirTwinState *)h->ptr;
    if ((*twin)->magic != STATE_MAGIC || (*twin)->version != STATE_VERSION) return -1;
    return 0;
}

static inline void input_copy(ReservoirInputBus *bus, ReservoirInputBus *dst)
{
    pthread_mutex_lock(&bus->lock);
    memcpy(&dst->arduino, &bus->arduino, sizeof(dst->arduino));
    memcpy(&dst->simulator, &bus->simulator, sizeof(dst->simulator));
    memcpy(&dst->weather, &bus->weather, sizeof(dst->weather));
    memcpy(&dst->upstream, &bus->upstream, sizeof(dst->upstream));
    dst->update_count = bus->update_count;
    pthread_mutex_unlock(&bus->lock);
}

static inline void twin_snapshot(ReservoirTwinState *twin, ReservoirTwinPayload *dst)
{
    pthread_mutex_lock(&twin->lock);
    *dst = twin->data;
    pthread_mutex_unlock(&twin->lock);
}

#endif
