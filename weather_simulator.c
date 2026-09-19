/* Weather forecast simulator. Scenario: dry, moderate, heavy, extreme. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../include/reservoir_state.h"

typedef struct {
    const char *name;
    float now, h1, h6, h12, h24, h72, confidence;
} Scenario;

static const Scenario scenarios[] = {
    {"dry",      2,  5,  10,  20,  35,  60, 90},
    {"moderate", 12, 18, 35,  62, 120, 210, 85},
    {"heavy",    38, 45, 95, 160, 310, 480, 78},
    {"extreme",  70, 85, 180, 300, 520, 760, 68}
};

int main(int argc, char *argv[])
{
    const char *wanted = (argc > 2 && strcmp(argv[1], "--scenario") == 0) ? argv[2] : "moderate";
    const Scenario *sc = &scenarios[1];
    for (size_t i = 0; i < sizeof(scenarios)/sizeof(scenarios[0]); ++i)
        if (strcmp(wanted, scenarios[i].name) == 0) sc = &scenarios[i];

    ShmHandle shm = { .fd = -1 };
    ReservoirInputBus *bus = NULL;
    while (input_bus_open(&shm, &bus) != 0) sleep(1);

    printf("[WEATHER] Scenario=%s\n", sc->name);
    for (;;) {
        WeatherSample w = {
            .rain_now_mmph = sc->now,
            .rain_1h_mm = sc->h1,
            .rain_6h_mm = sc->h6,
            .rain_12h_mm = sc->h12,
            .rain_24h_mm = sc->h24,
            .rain_72h_mm = sc->h72,
            .confidence_percent = sc->confidence,
            .scenario = (uint32_t)(sc - scenarios),
            .timestamp_ns = rt_now_ns(),
            .valid = 1
        };
        pthread_mutex_lock(&bus->lock);
        bus->weather = w;
        bus->update_count++;
        pthread_mutex_unlock(&bus->lock);
        sleep(1);
    }
}
