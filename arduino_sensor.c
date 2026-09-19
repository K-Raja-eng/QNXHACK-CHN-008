/*
 * arduino_sensor.c
 *
 * Reads Arduino frames into /reservoir_input.
 *
 * Two input modes are supported:
 *   1) Legacy QNX serial mode:
 *        ./arduino_sensor /dev/serusb1
 *      Expected legacy frame:
 *        REAL,level_m,rain_mmph,gate_pct,downstream_m,temp_c,emergency
 *
 *   2) Laptop TCP bridge mode:
 *        ./arduino_sensor --tcp 9100
 *      The Windows laptop reads the Arduino USB serial port and forwards
 *      the existing Arduino text line over TCP without changing its format:
 *        Mic1:12 Mic2:15 Gas:220 IR:700 Temp:28.4 Hum:61
 *
 * The TCP path is deliberately additive: the existing reservoir model,
 * simulators, dashboard WebSocket, and other QNX processes remain intact.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>
#include <stdint.h>
#include <math.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../include/reservoir_state.h"

#define DEFAULT_SERIAL "/dev/serusb1"
#define DEFAULT_TCP_PORT 9100
#define BAUD B9600
#define LINE_SIZE 256
#define BACKLOG 4

static int configure_serial(int fd)
{
    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) return -1;
    cfmakeraw(&tio);
    cfsetispeed(&tio, BAUD);
    cfsetospeed(&tio, BAUD);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~CSTOPB;
    tio.c_cflag &= ~PARENB;
    tio.c_cflag &= ~CSIZE;
    tio.c_cflag |= CS8;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 5; /* 0.5 s read timeout */
    return tcsetattr(fd, TCSANOW, &tio);
}

/* Existing reservoir frame kept for backwards compatibility. */
static int parse_real_frame(const char *line, ArduinoSample *s)
{
    char tag[16];
    int emergency;
    int n = sscanf(line, "%15[^,],%f,%f,%f,%f,%f,%d",
                   tag,
                   &s->reservoir_level_m,
                   &s->rainfall_mmph,
                   &s->gate_percent,
                   &s->downstream_level_m,
                   &s->temperature_c,
                   &emergency);
    if (n != 7 || strcmp(tag, "REAL") != 0)
        return -1;

    if (s->reservoir_level_m < 0.0f || s->reservoir_level_m > 20.0f) return -1;
    if (s->rainfall_mmph < 0.0f || s->rainfall_mmph > 500.0f) return -1;
    if (s->gate_percent < 0.0f || s->gate_percent > 100.0f) return -1;
    if (s->downstream_level_m < 0.0f || s->downstream_level_m > 20.0f) return -1;

    s->emergency = emergency ? 1 : 0;
    s->timestamp_ns = rt_now_ns();
    s->valid = 1;
    return 0;
}

/*
 * Parse the exact line currently produced by hack_final1.ino.
 * This is intentionally not converted into fake reservoir measurements.
 * Temperature/emergency are usable by the existing model; the other fields
 * are exposed as direct Arduino telemetry.
 */
static int parse_sensor_frame(const char *line, ArduinoSample *s)
{
    float mic1, mic2, gas, ir, temp, hum;
    int n = sscanf(line,
                   "Mic1:%f Mic2:%f Gas:%f IR:%f Temp:%f Hum:%f",
                   &mic1, &mic2, &gas, &ir, &temp, &hum);
    if (n != 6)
        return -1;

    if (!isfinite(mic1) || !isfinite(mic2) || !isfinite(gas) ||
        !isfinite(ir) || !isfinite(temp) || !isfinite(hum))
        return -1;

    if (mic1 < 0.0f || mic1 > 1023.0f) return -1;
    if (mic2 < 0.0f || mic2 > 1023.0f) return -1;
    if (gas  < 0.0f || gas  > 1023.0f) return -1;
    if (ir   < 0.0f || ir   > 1023.0f) return -1;
    if (hum < 0.0f || hum > 100.0f) return -1;

    s->mic1 = mic1;
    s->mic2 = mic2;
    s->gas = gas;
    s->ir = ir;
    s->temperature_c = temp;
    s->humidity = hum;
    s->emergency = (gas >= 800.0f) ? 1 : 0;
    s->aux_valid = 1;
    s->timestamp_ns = rt_now_ns();
    /* Keep an existing REAL sample alive; otherwise remain invalid for
       the reservoir-specific fields so weather/sim inputs keep working. */
    return 0;
}

static int publish_line(ReservoirInputBus *bus, const char *line,
                        unsigned long *good, unsigned long *bad)
{
    ArduinoSample sample;
    int ok = -1;
    int is_real = 0;

    memset(&sample, 0, sizeof(sample));

    /* Preserve the last reservoir fields while adding raw sensor telemetry. */
    pthread_mutex_lock(&bus->lock);
    sample = bus->arduino;
    pthread_mutex_unlock(&bus->lock);

    if (parse_real_frame(line, &sample) == 0) {
        ok = 0;
        is_real = 1;
    } else if (parse_sensor_frame(line, &sample) == 0) {
        ok = 0;
    }

    if (ok == 0) {
        pthread_mutex_lock(&bus->lock);
        bus->arduino = sample;
        bus->update_count++;
        pthread_mutex_unlock(&bus->lock);
        (*good)++;

        if ((*good % 10UL) == 0UL) {
            printf("[ARDUINO] temp=%.1fC hum=%.1f%% mic1=%.0f mic2=%.0f gas=%.0f ir=%.0f%s\n",
                   sample.temperature_c, sample.humidity,
                   sample.mic1, sample.mic2, sample.gas, sample.ir,
                   is_real ? " [REAL]" : "");
            fflush(stdout);
        }
        return 0;
    }

    (*bad)++;
    if ((*bad % 10UL) == 1UL)
        fprintf(stderr, "[ARDUINO] Invalid frame: %s\n", line);
    return -1;
}

static int run_serial(const char *device, ReservoirInputBus *bus)
{
    printf("[ARDUINO] Legacy serial input: %s @ 9600\n", device);

    int fd = open(device, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        perror("[ARDUINO] open serial");
        return 1;
    }
    if (configure_serial(fd) != 0) {
        perror("[ARDUINO] tcsetattr");
        close(fd);
        return 1;
    }

    char line[LINE_SIZE];
    size_t used = 0;
    unsigned long good = 0, bad = 0;

    for (;;) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("[ARDUINO] read");
            break;
        }
        if (n == 0) continue;

        if (c == '\r') continue;
        if (c != '\n') {
            if (used < sizeof(line) - 1) line[used++] = c;
            continue;
        }

        line[used] = '\0';
        used = 0;
        publish_line(bus, line, &good, &bad);
    }

    close(fd);
    return 1;
}

static int create_tcp_server(unsigned short port)
{
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        perror("[ARDUINO] socket");
        return -1;
    }

    int one = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[ARDUINO] bind");
        close(server);
        return -1;
    }
    if (listen(server, BACKLOG) < 0) {
        perror("[ARDUINO] listen");
        close(server);
        return -1;
    }

    return server;
}

static int run_tcp(unsigned short port, ReservoirInputBus *bus)
{
    int server = create_tcp_server(port);
    if (server < 0) return 1;

    printf("[ARDUINO] TCP input server listening on 0.0.0.0:%u\n", port);
    printf("[ARDUINO] Laptop should connect and forward Arduino serial lines.\n");
    fflush(stdout);

    unsigned long good = 0, bad = 0;

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client = accept(server, (struct sockaddr *)&client_addr, &client_len);
        if (client < 0) {
            if (errno == EINTR) continue;
            perror("[ARDUINO] accept");
            sleep(1);
            continue;
        }

        printf("[ARDUINO] Laptop connected from %s:%u\n",
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        fflush(stdout);

        char line[LINE_SIZE];
        size_t used = 0;

        for (;;) {
            char buf[256];
            ssize_t n = recv(client, buf, sizeof(buf), 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                perror("[ARDUINO] recv");
                break;
            }
            if (n == 0) {
                printf("[ARDUINO] Laptop disconnected; waiting for reconnect.\n");
                fflush(stdout);
                break;
            }

            for (ssize_t i = 0; i < n; ++i) {
                char c = buf[i];
                if (c == '\r') continue;

                if (c != '\n') {
                    if (used < sizeof(line) - 1) {
                        line[used++] = c;
                    } else {
                        /* Drop an overlong line and resynchronise at newline. */
                        used = 0;
                    }
                    continue;
                }

                line[used] = '\0';
                used = 0;
                publish_line(bus, line, &good, &bad);
            }
        }

        close(client);
    }

    close(server);
    return 0;
}

int main(int argc, char *argv[])
{
    signal(SIGPIPE, SIG_IGN);

    ShmHandle shm = { .fd = -1 };
    ReservoirInputBus *bus = NULL;

    printf("[ARDUINO] Sensor gateway: Arduino -> laptop -> QNX TCP/serial\n");

    while (input_bus_open(&shm, &bus) != 0) {
        fprintf(stderr, "[ARDUINO] Waiting for /reservoir_input ...\n");
        sleep(1);
    }

    int rc;
    if (argc == 1 || (argc >= 2 && strcmp(argv[1], "--tcp") == 0)) {
        /* TCP is the normal path: Arduino USB -> Windows laptop -> QNX. */
        unsigned long p = (argc >= 3) ? strtoul(argv[2], NULL, 10)
                                      : DEFAULT_TCP_PORT;
        if (p == 0 || p > 65535) {
            fprintf(stderr, "[ARDUINO] Invalid TCP port: %lu\n", p);
            shm_close(&shm);
            return 1;
        }
        rc = run_tcp((unsigned short)p, bus);
    } else {
        /* Explicit legacy direct-serial mode: ./arduino_sensor /dev/serusb1 */
        rc = run_serial(argv[1], bus);
    }

    shm_close(&shm);
    return rc;
}
