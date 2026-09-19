#ifndef CITY_STATE_COMPAT_H
#define CITY_STATE_COMPAT_H
/* Compatibility header for the previous City Digital Twin project. */
#include "reservoir_state.h"
typedef ReservoirTwinState CityTwinState;
typedef ShmHandle CityShm;
#define CITY_SHM_NAME TWIN_SHM_NAME
static inline int city_shm_open(CityShm *shm, int create) {
    ReservoirTwinState *twin = NULL;
    return create ? twin_create(shm, &twin) : twin_open(shm, &twin);
}
static inline void city_shm_close(CityShm *shm, int destroy) {
    (void)destroy;
    shm_close(shm);
}
#endif
