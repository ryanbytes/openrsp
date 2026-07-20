/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "mirisdr.h"
#include "async.h"
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
    assert(mirisdr_wall_clock_milliseconds() != 0u);
    assert(mirisdr_rspduo_bulk_status_requires_restart(LIBUSB_TRANSFER_STALL));
    assert(!mirisdr_rspduo_bulk_status_requires_restart(LIBUSB_TRANSFER_OVERFLOW));
    assert(!mirisdr_rspduo_bulk_status_requires_restart(LIBUSB_TRANSFER_ERROR));
    assert(mirisdr_async_status_allows_resubmit(MIRISDR_ASYNC_INACTIVE));
    assert(mirisdr_async_status_allows_resubmit(MIRISDR_ASYNC_RUNNING));
    assert(mirisdr_async_status_allows_resubmit(MIRISDR_ASYNC_PAUSED));
    assert(!mirisdr_async_status_allows_resubmit(MIRISDR_ASYNC_CANCELING));
    assert(!mirisdr_async_status_allows_resubmit(MIRISDR_ASYNC_FAILED));

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
