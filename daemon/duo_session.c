/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "duo_session.h"

#include <stddef.h>

static int valid_descriptor(openrspd_client_id descriptor)
{
    return descriptor >= 0;
}

static int valid_tuner(uint32_t tuner)
{
    return tuner == OPENRSPD_DUO_TUNER_A || tuner == OPENRSPD_DUO_TUNER_B;
}

static int valid_rate(uint32_t rate)
{
    return rate == 6000000u || rate == 8000000u;
}

static void clear_event(openrspd_duo_event *event)
{
    if (event == NULL) return;
    event->kind = OPENRSPD_DUO_EVENT_NONE;
    event->target_fd = OPENRSPD_INVALID_CLIENT;
    event->source_fd = OPENRSPD_INVALID_CLIENT;
    event->tuner = 0u;
}

static void emit_event(openrspd_duo_event *event, openrspd_duo_event_kind kind,
                       openrspd_client_id target_fd,
                       openrspd_client_id source_fd, uint32_t tuner)
{
    if (event == NULL) return;
    event->kind = kind;
    event->target_fd = target_fd;
    event->source_fd = source_fd;
    event->tuner = tuner;
}

static int descriptor_is_master(const openrspd_duo_session *session,
                                openrspd_client_id descriptor)
{
    return session != NULL && session->master_selected &&
           session->master_fd == descriptor;
}

static int descriptor_is_slave(const openrspd_duo_session *session,
                               openrspd_client_id descriptor)
{
    return session != NULL && session->slave_selected &&
           session->slave_fd == descriptor;
}

void openrspd_duo_session_init(openrspd_duo_session *session)
{
    if (session == NULL) return;
    session->master_fd = OPENRSPD_INVALID_CLIENT;
    session->slave_fd = OPENRSPD_INVALID_CLIENT;
    session->master_tuner = 0u;
    session->slave_tuner = 0u;
    session->sample_rate_hz = OPENRSPD_DUO_RATE_UNSPECIFIED;
    session->master_selected = 0u;
    session->slave_selected = 0u;
    session->master_initialised = 0u;
    session->slave_initialised = 0u;
}

void openrspd_duo_event_clear(openrspd_duo_event *event)
{
    clear_event(event);
}

openrspd_duo_result openrspd_duo_acquire(openrspd_duo_session *session,
                                         openrspd_client_id descriptor,
                                         openrspd_duo_role role,
                                         uint32_t tuner,
                                         uint32_t sample_rate_hz,
                                         openrspd_duo_event *event)
{
    clear_event(event);
    if (session == NULL || !valid_descriptor(descriptor) ||
        (role != OPENRSPD_DUO_ROLE_MASTER && role != OPENRSPD_DUO_ROLE_SLAVE) ||
        !valid_tuner(tuner))
        return OPENRSPD_DUO_INVALID;

    if (role == OPENRSPD_DUO_ROLE_MASTER) {
        if (!valid_rate(sample_rate_hz)) return OPENRSPD_DUO_INVALID;
        if (session->master_selected) {
            if (session->master_fd == descriptor &&
                session->master_tuner == tuner &&
                session->sample_rate_hz == sample_rate_hz)
                return OPENRSPD_DUO_OK;
            return OPENRSPD_DUO_BUSY;
        }
        if (session->slave_selected ||
            (session->slave_tuner != 0u && session->slave_tuner == tuner))
            return OPENRSPD_DUO_BUSY;
        session->master_fd = descriptor;
        session->master_tuner = tuner;
        session->sample_rate_hz = sample_rate_hz;
        session->master_selected = 1u;
        session->master_initialised = 0u;
        return OPENRSPD_DUO_OK;
    }

    /* A slave may reserve only after a master reservation exists.  It can be
     * selected while the master is still preparing, which is the state that
     * makes a subsequent Slave Init return StartPending. */
    if (!session->master_selected || session->master_fd == descriptor ||
        (sample_rate_hz != OPENRSPD_DUO_RATE_UNSPECIFIED &&
         sample_rate_hz != session->sample_rate_hz))
        return OPENRSPD_DUO_BUSY;
    if (tuner == session->master_tuner) return OPENRSPD_DUO_BUSY;
    if (session->slave_selected) {
        if (session->slave_fd == descriptor && session->slave_tuner == tuner)
            return OPENRSPD_DUO_OK;
        return OPENRSPD_DUO_BUSY;
    }
    session->slave_fd = descriptor;
    session->slave_tuner = tuner;
    session->slave_selected = 1u;
    session->slave_initialised = 0u;
    emit_event(event, OPENRSPD_DUO_EVENT_SLAVE_ATTACHED,
               session->master_fd, descriptor, tuner);
    return OPENRSPD_DUO_OK;
}

openrspd_duo_result openrspd_duo_initialise(openrspd_duo_session *session,
                                            openrspd_client_id descriptor,
                                            openrspd_duo_event *event)
{
    clear_event(event);
    if (session == NULL || !valid_descriptor(descriptor))
        return OPENRSPD_DUO_INVALID;
    if (descriptor_is_master(session, descriptor)) {
        if (session->master_initialised) return OPENRSPD_DUO_BUSY;
        session->master_initialised = 1u;
        if (session->slave_selected)
            emit_event(event, OPENRSPD_DUO_EVENT_MASTER_INITIALISED,
                       session->slave_fd, descriptor, session->slave_tuner);
        return OPENRSPD_DUO_OK;
    }
    if (!descriptor_is_slave(session, descriptor)) return OPENRSPD_DUO_INVALID;
    if (session->slave_initialised) return OPENRSPD_DUO_BUSY;
    if (!session->master_initialised) return OPENRSPD_DUO_START_PENDING;
    session->slave_initialised = 1u;
    emit_event(event, OPENRSPD_DUO_EVENT_SLAVE_INITIALISED,
               session->master_fd, descriptor, session->slave_tuner);
    return OPENRSPD_DUO_OK;
}

openrspd_duo_result openrspd_duo_uninitialise(openrspd_duo_session *session,
                                              openrspd_client_id descriptor,
                                              openrspd_duo_event *event)
{
    clear_event(event);
    if (session == NULL || !valid_descriptor(descriptor))
        return OPENRSPD_DUO_INVALID;
    if (descriptor_is_master(session, descriptor)) {
        if (!session->master_initialised) return OPENRSPD_DUO_INVALID;
        if (session->slave_initialised) return OPENRSPD_DUO_STOP_PENDING;
        session->master_initialised = 0u;
        return OPENRSPD_DUO_OK;
    }
    if (!descriptor_is_slave(session, descriptor) ||
        !session->slave_initialised)
        return OPENRSPD_DUO_INVALID;
    session->slave_initialised = 0u;
    emit_event(event, OPENRSPD_DUO_EVENT_SLAVE_UNINITIALISED,
               session->master_fd, descriptor, session->slave_tuner);
    return OPENRSPD_DUO_OK;
}

openrspd_duo_result openrspd_duo_release(openrspd_duo_session *session,
                                         openrspd_client_id descriptor,
                                         openrspd_duo_event *event)
{
    clear_event(event);
    if (session == NULL || !valid_descriptor(descriptor))
        return OPENRSPD_DUO_INVALID;
    if (descriptor_is_master(session, descriptor)) {
        if (session->master_initialised) return OPENRSPD_DUO_BUSY;
        /* Keep explicit API release ordered: the master cannot release while
         * the slave still owns a selected handle, even if it is uninitialised. */
        if (session->slave_selected) return OPENRSPD_DUO_STOP_PENDING;
        openrspd_duo_session_init(session);
        return OPENRSPD_DUO_OK;
    }
    if (!descriptor_is_slave(session, descriptor)) return OPENRSPD_DUO_INVALID;
    if (session->slave_initialised) return OPENRSPD_DUO_BUSY;
    emit_event(event, OPENRSPD_DUO_EVENT_SLAVE_DETACHED,
               session->master_fd, descriptor, session->slave_tuner);
    session->slave_fd = OPENRSPD_INVALID_CLIENT;
    session->slave_tuner = 0u;
    session->slave_selected = 0u;
    session->slave_initialised = 0u;
    return OPENRSPD_DUO_OK;
}

openrspd_duo_result openrspd_duo_disconnect(openrspd_duo_session *session,
                                            openrspd_client_id descriptor,
                                            openrspd_duo_event *event)
{
    clear_event(event);
    if (session == NULL || !valid_descriptor(descriptor))
        return OPENRSPD_DUO_INVALID;
    if (descriptor_is_slave(session, descriptor)) {
        emit_event(event, OPENRSPD_DUO_EVENT_SLAVE_DLL_DISAPPEARED,
                   session->master_fd, descriptor, session->slave_tuner);
        session->slave_fd = OPENRSPD_INVALID_CLIENT;
        session->slave_tuner = 0u;
        session->slave_selected = 0u;
        session->slave_initialised = 0u;
        return OPENRSPD_DUO_OK;
    }
    if (descriptor_is_master(session, descriptor)) {
        if (session->slave_selected)
            emit_event(event, OPENRSPD_DUO_EVENT_MASTER_DLL_DISAPPEARED,
                       session->slave_fd, descriptor, session->slave_tuner);
        /* A direct-dual hardware stream has one shared owner.  A dead master
         * therefore invalidates the peer reservation instead of leaving an
         * orphaned slave that could receive stale tuner-B frames. */
        openrspd_duo_session_init(session);
        return OPENRSPD_DUO_OK;
    }
    return OPENRSPD_DUO_OK;
}

int openrspd_duo_route(const openrspd_duo_session *session, uint32_t tuner,
                       openrspd_client_id *descriptor)
{
    if (descriptor != NULL) *descriptor = OPENRSPD_INVALID_CLIENT;
    if (session == NULL || !valid_tuner(tuner)) return -1;
    if (session->master_selected && session->master_initialised &&
        session->master_tuner == tuner) {
        if (descriptor != NULL) *descriptor = session->master_fd;
        return session->master_fd >= 0 ? 1 : 0;
    }
    if (session->slave_selected && session->slave_initialised &&
        session->slave_tuner == tuner) {
        if (descriptor != NULL) *descriptor = session->slave_fd;
        return session->slave_fd >= 0 ? 1 : 0;
    }
    return 0;
}

openrspd_duo_role openrspd_duo_role_for_descriptor(
    const openrspd_duo_session *session, openrspd_client_id descriptor)
{
    if (descriptor_is_master(session, descriptor)) return OPENRSPD_DUO_ROLE_MASTER;
    if (descriptor_is_slave(session, descriptor)) return OPENRSPD_DUO_ROLE_SLAVE;
    return OPENRSPD_DUO_ROLE_NONE;
}

int openrspd_duo_write_failure_stops_stream(
    const openrspd_duo_session *session, openrspd_client_id descriptor)
{
    return openrspd_duo_role_for_descriptor(session, descriptor) !=
           OPENRSPD_DUO_ROLE_SLAVE;
}
