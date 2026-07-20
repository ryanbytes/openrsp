/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef OPENRSPD_DUO_SESSION_H
#define OPENRSPD_DUO_SESSION_H

#include <stdint.h>

typedef intptr_t openrspd_client_id;
#define OPENRSPD_INVALID_CLIENT ((openrspd_client_id)-1)

/* This module deliberately has no USB or socket dependencies.  It is the
 * ownership/state part of the RSPduo master/slave contract; the daemon owns
 * the hardware and translates these decisions to protocol operations. */
#define OPENRSPD_DUO_TUNER_A 1u
#define OPENRSPD_DUO_TUNER_B 2u
#define OPENRSPD_DUO_RATE_UNSPECIFIED 0u

typedef enum {
    OPENRSPD_DUO_ROLE_NONE = 0,
    OPENRSPD_DUO_ROLE_MASTER = 1,
    OPENRSPD_DUO_ROLE_SLAVE = 2
} openrspd_duo_role;

typedef enum {
    OPENRSPD_DUO_OK = 0,
    OPENRSPD_DUO_START_PENDING = 1,
    OPENRSPD_DUO_STOP_PENDING = 2,
    OPENRSPD_DUO_BUSY = 3,
    OPENRSPD_DUO_INVALID = 4
} openrspd_duo_result;

typedef enum {
    OPENRSPD_DUO_EVENT_NONE = 0,
    OPENRSPD_DUO_EVENT_MASTER_INITIALISED = 1,
    OPENRSPD_DUO_EVENT_SLAVE_ATTACHED = 2,
    OPENRSPD_DUO_EVENT_SLAVE_DETACHED = 3,
    OPENRSPD_DUO_EVENT_SLAVE_INITIALISED = 4,
    OPENRSPD_DUO_EVENT_SLAVE_UNINITIALISED = 5,
    OPENRSPD_DUO_EVENT_MASTER_DLL_DISAPPEARED = 6,
    OPENRSPD_DUO_EVENT_SLAVE_DLL_DISAPPEARED = 7
} openrspd_duo_event_kind;

typedef struct {
    openrspd_duo_event_kind kind;
    openrspd_client_id target_fd;
    openrspd_client_id source_fd;
    uint32_t tuner;
} openrspd_duo_event;

typedef struct {
    openrspd_client_id master_fd;
    openrspd_client_id slave_fd;
    uint32_t master_tuner;
    uint32_t slave_tuner;
    uint32_t sample_rate_hz;
    uint8_t master_selected;
    uint8_t slave_selected;
    uint8_t master_initialised;
    uint8_t slave_initialised;
} openrspd_duo_session;

void openrspd_duo_session_init(openrspd_duo_session *session);
void openrspd_duo_event_clear(openrspd_duo_event *event);

openrspd_duo_result openrspd_duo_acquire(openrspd_duo_session *session,
                                         openrspd_client_id descriptor,
                                         openrspd_duo_role role,
                                         uint32_t tuner,
                                         uint32_t sample_rate_hz,
                                         openrspd_duo_event *event);

openrspd_duo_result openrspd_duo_initialise(openrspd_duo_session *session,
                                            openrspd_client_id descriptor,
                                            openrspd_duo_event *event);

openrspd_duo_result openrspd_duo_uninitialise(openrspd_duo_session *session,
                                              openrspd_client_id descriptor,
                                              openrspd_duo_event *event);

openrspd_duo_result openrspd_duo_release(openrspd_duo_session *session,
                                         openrspd_client_id descriptor,
                                         openrspd_duo_event *event);

/* A disconnect is stronger than an API release: it clears the disconnected
 * role and, for a master loss, tears down the peer reservation as well. */
openrspd_duo_result openrspd_duo_disconnect(openrspd_duo_session *session,
                                            openrspd_client_id descriptor,
                                            openrspd_duo_event *event);

/* Returns the descriptor that should receive a direct-dual IQ lane.  A return
 * value of zero means that the lane is not currently routable; -1 means an
 * invalid tuner argument. */
int openrspd_duo_route(const openrspd_duo_session *session, uint32_t tuner,
                       openrspd_client_id *descriptor);

openrspd_duo_role openrspd_duo_role_for_descriptor(
    const openrspd_duo_session *session, openrspd_client_id descriptor);

#endif
