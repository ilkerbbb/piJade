/*
 * This file replaces semphr.h on Darwin, where unnamed POSIX semaphores are
 * unavailable. It preserves the FreeRTOS semaphore API used by libjade while
 * using libdispatch for binary semaphores and pthread mutexes for mutexes.
 */

#ifndef _LIBJADE_FREERTOS_SEMPHR_H_
#define _LIBJADE_FREERTOS_SEMPHR_H_ 1

#include <dispatch/dispatch.h>
#include <errno.h>
#include <freertos/projdefs.h>
#include <freertos/task.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "jade_log.h"

typedef enum {
    SEMAPHORE_TYPE_BINARY,
    SEMAPHORE_TYPE_MUTEX,
} SemaphoreType_t;

typedef struct Semaphore {
    SemaphoreType_t type;
    union {
        dispatch_semaphore_t binary;
        pthread_mutex_t mutex;
    };
}* SemaphoreHandle_t;

#define LIBJADE_NSEC_PER_MSEC 1000000ULL
#define LIBJADE_NSEC_PER_SEC 1000000000ULL

static inline uint64_t libjade_monotonic_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * LIBJADE_NSEC_PER_SEC + (uint64_t)now.tv_nsec;
}

static inline int libjade_pthread_mutex_timedlock(pthread_mutex_t* mutex, TickType_t timeout)
{
    const uint64_t timeout_ns = (uint64_t)timeout * portTICK_PERIOD_MS * LIBJADE_NSEC_PER_MSEC;
    const uint64_t deadline_ns = libjade_monotonic_ns() + timeout_ns;
    for (;;) {
        const int ret = pthread_mutex_trylock(mutex);
        if (ret != EBUSY) {
            return ret; // Either succeeded or an unknown error occurred
        }
        if (!timeout_ns) {
            return ETIMEDOUT;
        }

        const uint64_t now_ns = libjade_monotonic_ns();
        if (now_ns >= deadline_ns) {
            return ETIMEDOUT;
        }

        const uint64_t remaining_ns = deadline_ns - now_ns;
        const long sleep_ns = (long)(remaining_ns < LIBJADE_NSEC_PER_MSEC ? remaining_ns : LIBJADE_NSEC_PER_MSEC);
        const struct timespec sleep_time = { .tv_sec = 0, .tv_nsec = sleep_ns };
        nanosleep(&sleep_time, NULL);
    }
}

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    SemaphoreHandle_t out = malloc(sizeof(struct Semaphore));
    if (out) {
        out->type = SEMAPHORE_TYPE_MUTEX;
        if (pthread_mutex_init(&out->mutex, NULL)) {
            free(out);
            out = NULL;
        }
    }
    return out;
}

static inline SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    SemaphoreHandle_t out = malloc(sizeof(struct Semaphore));
    if (out) {
        out->type = SEMAPHORE_TYPE_BINARY;
        out->binary = dispatch_semaphore_create(0);
        if (!out->binary) {
            free(out);
            out = NULL;
        }
    }
    return out;
}

static inline void vSemaphoreDelete(SemaphoreHandle_t s)
{
    switch (s->type) {
    case SEMAPHORE_TYPE_BINARY:
#if !OS_OBJECT_USE_OBJC
        dispatch_release(s->binary);
#endif
        break;
    case SEMAPHORE_TYPE_MUTEX:
        if (pthread_mutex_destroy(&s->mutex)) {
            abort();
        }
        break;
    }
    free(s);
}

static inline int xSemaphoreTake(SemaphoreHandle_t s, TickType_t timeout)
{
    switch (s->type) {
    case SEMAPHORE_TYPE_BINARY:
        if (timeout == portMAX_DELAY) {
            dispatch_semaphore_wait(s->binary, DISPATCH_TIME_FOREVER);
            return pdTRUE;
        }
        const int64_t timeout_ns = (int64_t)timeout * portTICK_PERIOD_MS * LIBJADE_NSEC_PER_MSEC;
        if (dispatch_semaphore_wait(s->binary, dispatch_time(DISPATCH_TIME_NOW, timeout_ns))) {
            return pdFALSE;
        }
        break;
    case SEMAPHORE_TYPE_MUTEX:
        if (timeout == portMAX_DELAY) {
            const int ret = pthread_mutex_lock(&s->mutex);
            if (ret) {
                JADE_LOGE("Unknown error aquiring mutex (pthread_mutex_lock): %d", ret);
                abort();
            }
            return pdTRUE;
        }
        const int ret = libjade_pthread_mutex_timedlock(&s->mutex, timeout);
        switch (ret) {
        case 0:
            break;
        case ETIMEDOUT:
            return pdFALSE;
        default:
            JADE_LOGE("Unknown error aquiring mutex (pthread_mutex_timedlock): %d", ret);
            abort();
        }
        break;
    }
    return pdTRUE;
}

static inline void xSemaphoreGive(SemaphoreHandle_t s)
{
    switch (s->type) {
    case SEMAPHORE_TYPE_BINARY:
        dispatch_semaphore_signal(s->binary);
        break;
    case SEMAPHORE_TYPE_MUTEX:
        if (pthread_mutex_unlock(&s->mutex)) {
            abort();
        }
        break;
    }
}

#endif // _LIBJADE_FREERTOS_SEMPHR_H_
