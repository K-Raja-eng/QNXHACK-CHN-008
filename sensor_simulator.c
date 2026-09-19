/* Simulated secondary field sensors. Kept separate from the Arduino real-sensor process. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "../include/reservoir_state.h"

int main(void)
{
    ShmHandle shm = { .fd = -1 };
    ReservoirInputBus *bus = NULL;

    while (input_bus_open(&shm, &bus) != 0) sleep(1);

    printf("[SIM-SENSOR] Secondary field sensor simulation started\n");

    float soil = 45.0f;
    for (;;) {
        SimSensorSample s;
        s.soil_moisture_percent = soil;
        s.catchment_saturation_percent = 50.0f + soil * 0.45f;
        s.virtual_downstream_m = 2.0f + soil * 0.015f;
        s.timestamp_ns = rt_now_ns();
        s.valid = 1;

        pthread_mutex_lock(&bus->lock);
        bus->simulator = s;
        bus->update_count++;
        pthread_mutex_unlock(&bus->lock);

        soil += 2.0f;
        if (soil > 95.0f) soil = 45.0f;
        usleep(500000);
    }
}
