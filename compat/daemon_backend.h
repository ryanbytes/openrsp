/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef OPENRSP_DAEMON_BACKEND_H
#define OPENRSP_DAEMON_BACKEND_H

#include "openrsp/protocol.h"
#include <stddef.h>
#include <stdint.h>

typedef struct openrsp_daemon_backend openrsp_daemon_backend;
typedef struct openrsp_daemon_api_lock openrsp_daemon_api_lock;
typedef void (*openrsp_daemon_iq_callback)(const int16_t *interleaved,
                                           size_t iq_samples, uint32_t sequence,
                                           void *context);
typedef void (*openrsp_daemon_failure_callback)(void *context);

int openrsp_daemon_backend_list(openrsp_device_record *devices, size_t capacity);
int openrsp_daemon_api_lock_acquire(openrsp_daemon_api_lock **lock);
int openrsp_daemon_api_lock_list(openrsp_daemon_api_lock *lock,
                                 openrsp_device_record *devices, size_t capacity);
int openrsp_daemon_api_lock_release(openrsp_daemon_api_lock *lock);

int openrsp_daemon_backend_open(openrsp_daemon_backend **backend, uint32_t device_index);
int openrsp_daemon_backend_configure(openrsp_daemon_backend *backend,
                                     const openrsp_radio_config *config);
int openrsp_daemon_backend_start(openrsp_daemon_backend *backend,
                                 openrsp_daemon_iq_callback callback,
                                 openrsp_daemon_failure_callback failure_callback,
                                 void *context);
int openrsp_daemon_backend_update(openrsp_daemon_backend *backend,
                                  const openrsp_radio_config *config, uint32_t changed_flags);
int openrsp_daemon_backend_stop(openrsp_daemon_backend *backend);
void openrsp_daemon_backend_close(openrsp_daemon_backend *backend);

#endif
