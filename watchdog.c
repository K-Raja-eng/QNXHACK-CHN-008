/*
 * watchdog.c
 * Educational process supervisor.
 *
 * Run:
 *   ./watchdog --start
 *
 * It creates child processes using spawnl(P_NOWAIT), demonstrating QNX process
 * creation and concurrent execution. The main project run_all.sh can also launch
 * processes with the shell '&' operator when you want to demonstrate the shell path.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <process.h>
#include <errno.h>

#define APP_DIR "/usr/usr/level1"
#define MAX_CHILDREN 7

typedef struct {
    const char *name;
    const char *arg1;
    pid_t pid;
} Child;

static Child children[MAX_CHILDREN] = {
    {"sensor_simulator", NULL, -1},
    {"weather_simulator", "heavy", -1},
    {"upstream_simulator", "heavy", -1},
    {"reservoir_engine", NULL, -1},
    {"rtos_timer_engine", NULL, -1},
    {"operator_server", NULL, -1},
    {"ws_bridge", NULL, -1}
};
static volatile int running = 1;

static void stop_handler(int sig) { (void)sig; running = 0; }

static pid_t start_child(Child *c, int engine_first)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", APP_DIR, c->name);

    pid_t pid;
    if (strcmp(c->name, "reservoir_engine") == 0) {
        if (engine_first) pid = spawnl(P_NOWAIT, path, c->name, "--create", NULL);
        else pid = spawnl(P_NOWAIT, path, c->name, NULL);
    } else if (strcmp(c->name, "weather_simulator") == 0) {
        pid = spawnl(P_NOWAIT, path, c->name, "--scenario", c->arg1, NULL);
    } else if (strcmp(c->name, "upstream_simulator") == 0) {
        pid = spawnl(P_NOWAIT, path, c->name, "--scenario", c->arg1, NULL);
    } else {
        pid = spawnl(P_NOWAIT, path, c->name, NULL);
    }
    if (pid < 0)
        fprintf(stderr, "[WD] spawn %s failed: %s\n", c->name, strerror(errno));
    else
        printf("[WD] started %-20s pid=%d\n", c->name, pid);
    return pid;
}

int main(int argc, char *argv[])
{
    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);

    if (argc < 2 || strcmp(argv[1], "--start") != 0) {
        printf("Usage: %s --start\n", argv[0]);
        return 0;
    }

    printf("[WD] Starting QNX Reservoir Digital Twin processes...\n");
    /* Engine creates shared memory. Start it first, then allow dependents to attach. */
    children[3].pid = start_child(&children[3], 1);
    sleep(1);
    for (int i = 0; i < MAX_CHILDREN; ++i) {
        if (i == 3) continue;
        children[i].pid = start_child(&children[i], 0);
    }

    while (running) {
        sleep(2);
        for (int i = 0; i < MAX_CHILDREN; ++i) {
            if (children[i].pid <= 0) continue;
            int status;
            pid_t r = waitpid(children[i].pid, &status, WNOHANG);
            if (r == children[i].pid) {
                printf("[WD] %s stopped; restarting...\n", children[i].name);
                children[i].pid = start_child(&children[i], 0);
            }
        }
    }

    printf("[WD] Supervisor stopping.\n");
    for (int i = 0; i < MAX_CHILDREN; ++i)
        if (children[i].pid > 0) kill(children[i].pid, SIGTERM);
    return 0;
}
