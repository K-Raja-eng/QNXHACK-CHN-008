/* Upstream inflow/release simulator. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include "../include/reservoir_state.h"

static int scenario_index(const char *name)
{
    if (strcmp(name, "dry") == 0) return 0;
    if (strcmp(name, "moderate") == 0) return 1;
    if (strcmp(name, "heavy") == 0) return 2;
    if (strcmp(name, "extreme") == 0) return 3;
    return 1;
}

int main(int argc, char *argv[])
{
    const char *name = (argc > 2 && strcmp(argv[1], "--scenario") == 0) ? argv[2] : "moderate";
    int scenario = scenario_index(name);
    float base_inflow[] = {350, 700, 1250, 1800};
    float base_release[] = {100, 200, 400, 550};

    ShmHandle shm = { .fd = -1 };
    ReservoirInputBus *bus = NULL;
    while (input_bus_open(&shm, &bus) != 0) sleep(1);

    printf("[UPSTREAM] Scenario=%s\n", name);
    float phase = 0.0f;
    for (;;) {
        float inflow = base_inflow[scenario] * (0.85f + 0.15f * (sinf(phase) + 1.0f));
        float release = base_release[scenario];
        UpstreamSample u = {
            .inflow_m3s = inflow,
            .release_m3s = release,
            .water_level_m = 4.0f + inflow * 0.0006f,
            .scenario = (uint32_t)scenario,
            .timestamp_ns = rt_now_ns(),
            .valid = 1
        };
        pthread_mutex_lock(&bus->lock);
        bus->upstream = u;
        bus->update_count++;
        pthread_mutex_unlock(&bus->lock);

        phase += 0.2f;
        usleep(500000);
    }
}
