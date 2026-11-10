/*
 * shared.c - Gemeinsame Datensaetze von Pi 2 mit je einem Mutex
 * (Regeln siehe shared.h).
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Fable 5.2),
 * geprueft von: <Name>
 */
#include "shared.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t g_sensor_mutex[SENSOR_COUNT];
static sensor_data_t g_sensor[SENSOR_COUNT];

static pthread_mutex_t g_comm_mutex;
static comm_data_t g_comm;

static pthread_mutex_t g_state_mutex;
static pthread_cond_t g_state_cond;
static safety_state_t g_state;

static pthread_mutex_t g_motor_mutex;
static motor_status_t g_motor;

static atomic_bool g_stop_requested;
static atomic_bool g_running;

static int32_t init_mutex(pthread_mutex_t *mutex)
{
    pthread_mutexattr_t attr;
    int32_t rc = pthread_mutexattr_init(&attr);

    if (rc == 0) {
        rc = pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
        if (rc == 0) {
            rc = pthread_mutex_init(mutex, &attr);
        }
        if (pthread_mutexattr_destroy(&attr) != 0) {
            rc = -1;
        }
    }
    return rc;
}

int32_t shared_init(void)
{
    pthread_condattr_t cattr;
    uint32_t i;
    int32_t rc = 0;

    (void)memset(g_sensor, 0, sizeof(g_sensor));
    (void)memset(&g_comm, 0, sizeof(g_comm));
    (void)memset(&g_state, 0, sizeof(g_state)); /* STATE_INIT, Motor AUS */
    (void)memset(&g_motor, 0, sizeof(g_motor));
    atomic_store(&g_stop_requested, false);
    atomic_store(&g_running, true);

    for (i = 0U; i < SENSOR_COUNT; i++) {
        if (init_mutex(&g_sensor_mutex[i]) != 0) {
            rc = -1;
        }
    }
    if (rc == 0) {
        rc = init_mutex(&g_comm_mutex);
    }
    if (rc == 0) {
        rc = init_mutex(&g_state_mutex);
    }
    if (rc == 0) {
        rc = init_mutex(&g_motor_mutex);
    }
    if (rc == 0) {
        rc = pthread_condattr_init(&cattr);
        if (rc == 0) {
            rc = pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
            if (rc == 0) {
                rc = pthread_cond_init(&g_state_cond, &cattr);
            }
            if (pthread_condattr_destroy(&cattr) != 0) {
                rc = -1;
            }
        }
    }
    return (rc == 0) ? 0 : -1;
}

void shared_sensor_write(uint32_t idx, const sensor_data_t *data)
{
    if ((idx < SENSOR_COUNT) && (data != NULL) &&
        (pthread_mutex_lock(&g_sensor_mutex[idx]) == 0)) {
        g_sensor[idx] = *data;
        (void)pthread_mutex_unlock(&g_sensor_mutex[idx]);
    }
}

void shared_sensor_read(uint32_t idx, sensor_data_t *data)
{
    if (data != NULL) {
        (void)memset(data, 0, sizeof(*data));
        if ((idx < SENSOR_COUNT) && (pthread_mutex_lock(&g_sensor_mutex[idx]) == 0)) {
            *data = g_sensor[idx];
            (void)pthread_mutex_unlock(&g_sensor_mutex[idx]);
        }
    }
}

void shared_comm_write(const comm_data_t *data)
{
    if ((data != NULL) && (pthread_mutex_lock(&g_comm_mutex) == 0)) {
        g_comm = *data;
        (void)pthread_mutex_unlock(&g_comm_mutex);
    }
}

void shared_comm_read(comm_data_t *data)
{
    if (data != NULL) {
        (void)memset(data, 0, sizeof(*data));
        if (pthread_mutex_lock(&g_comm_mutex) == 0) {
            *data = g_comm;
            (void)pthread_mutex_unlock(&g_comm_mutex);
        }
    }
}

void shared_state_write(const safety_state_t *state)
{
    if ((state != NULL) && (pthread_mutex_lock(&g_state_mutex) == 0)) {
        g_state = *state;
        (void)pthread_cond_broadcast(&g_state_cond);
        (void)pthread_mutex_unlock(&g_state_mutex);
    }
}

void shared_state_read(safety_state_t *state)
{
    if (state != NULL) {
        (void)memset(state, 0, sizeof(*state));
        if (pthread_mutex_lock(&g_state_mutex) == 0) {
            *state = g_state;
            (void)pthread_mutex_unlock(&g_state_mutex);
        }
    }
}

bool shared_state_wait(uint32_t last_generation, uint64_t deadline_us, safety_state_t *state)
{
    bool is_new = false;

    if (state != NULL) {
        (void)memset(state, 0, sizeof(*state));
        if (pthread_mutex_lock(&g_state_mutex) == 0) {
            struct timespec ts;
            int32_t rc = 0;

            ts.tv_sec = (time_t)(deadline_us / 1000000ULL);
            ts.tv_nsec = (int32_t)((deadline_us % 1000000ULL) * 1000ULL);

            while ((g_state.generation == last_generation) && (rc == 0)) {
                rc = pthread_cond_timedwait(&g_state_cond, &g_state_mutex, &ts);
            }
            *state = g_state;
            is_new = (g_state.generation != last_generation);
            (void)pthread_mutex_unlock(&g_state_mutex);
        }
    }
    return is_new;
}

void shared_motor_write(const motor_status_t *status)
{
    if ((status != NULL) && (pthread_mutex_lock(&g_motor_mutex) == 0)) {
        g_motor = *status;
        (void)pthread_mutex_unlock(&g_motor_mutex);
    }
}

void shared_motor_read(motor_status_t *status)
{
    if (status != NULL) {
        /* Bei Fehler: Generation 0 gilt nie als aktuelle Rueckmeldung. */
        (void)memset(status, 0, sizeof(*status));
        status->applied_level = MOTOR_LEVEL_UNKNOWN;
        if (pthread_mutex_lock(&g_motor_mutex) == 0) {
            *status = g_motor;
            (void)pthread_mutex_unlock(&g_motor_mutex);
        }
    }
}

void shared_request_stop(void)
{
    atomic_store(&g_stop_requested, true);
}

bool shared_stop_requested(void)
{
    return atomic_load(&g_stop_requested);
}

void shared_set_running(bool running)
{
    atomic_store(&g_running, running);
}

bool shared_running(void)
{
    return atomic_load(&g_running);
}
