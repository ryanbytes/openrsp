/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "mirisdr.h"
#include "libusb.h"
#include "structs.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void *complete_cancellation(void *opaque)
{
    mirisdr_dev_t *device = opaque;
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000L};
    for (unsigned int attempt = 0u; attempt < 1000u; ++attempt) {
        if (atomic_load(&device->async_status) == MIRISDR_ASYNC_CANCELING) {
            atomic_store(&device->async_status, MIRISDR_ASYNC_INACTIVE);
            return NULL;
        }
        (void)nanosleep(&delay, NULL);
    }
    return (void *)1;
}

int main(void)
{
    mirisdr_dev_t device;
    memset(&device, 0, sizeof(device));
    atomic_init(&device.async_status, MIRISDR_ASYNC_INACTIVE);
    assert(mirisdr_cancel_async(&device) == -2);

    atomic_store(&device.async_status, MIRISDR_ASYNC_RUNNING);
    assert(mirisdr_cancel_async(&device) == 0);
    assert(atomic_load(&device.async_status) == MIRISDR_ASYNC_CANCELING);

    atomic_store(&device.async_status, MIRISDR_ASYNC_RUNNING);
    pthread_t completion;
    assert(pthread_create(&completion, NULL, complete_cancellation, &device) == 0);
    assert(mirisdr_cancel_async_now(&device) == 0);
    void *thread_result = NULL;
    assert(pthread_join(completion, &thread_result) == 0);
    assert(thread_result == NULL);
    assert(atomic_load(&device.async_status) == MIRISDR_ASYNC_INACTIVE);
    puts("RSPDUO_ASYNC_STATE_OK");
    return 0;
}
