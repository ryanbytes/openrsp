/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "duo_session.h"

#include <assert.h>
#include <stdio.h>

static void assert_event(const openrspd_duo_event *event,
                         openrspd_duo_event_kind kind, int target, int source)
{
    assert(event->kind == kind);
    assert(event->target_fd == target);
    assert(event->source_fd == source);
}

static void test_attach_and_initialise_order(void)
{
    openrspd_duo_session session;
    openrspd_duo_event event;
    openrspd_duo_session_init(&session);

    assert(openrspd_duo_acquire(&session, 10, OPENRSPD_DUO_ROLE_MASTER,
                                OPENRSPD_DUO_TUNER_A, 6000000u, &event) ==
           OPENRSPD_DUO_OK);
    assert(event.kind == OPENRSPD_DUO_EVENT_NONE);
    assert(openrspd_duo_acquire(&session, 11, OPENRSPD_DUO_ROLE_SLAVE,
                                OPENRSPD_DUO_TUNER_B,
                                OPENRSPD_DUO_RATE_UNSPECIFIED, &event) ==
           OPENRSPD_DUO_OK);
    assert_event(&event, OPENRSPD_DUO_EVENT_SLAVE_ATTACHED, 10, 11);
    assert(openrspd_duo_initialise(&session, 11, &event) ==
           OPENRSPD_DUO_START_PENDING);
    assert(event.kind == OPENRSPD_DUO_EVENT_NONE);

    assert(openrspd_duo_initialise(&session, 10, &event) == OPENRSPD_DUO_OK);
    assert_event(&event, OPENRSPD_DUO_EVENT_MASTER_INITIALISED, 11, 10);
    openrspd_client_id descriptor = OPENRSPD_INVALID_CLIENT;
    assert(openrspd_duo_route(&session, OPENRSPD_DUO_TUNER_A, &descriptor) == 1 &&
           descriptor == 10);
    assert(openrspd_duo_route(&session, OPENRSPD_DUO_TUNER_B, &descriptor) == 0);

    assert(openrspd_duo_initialise(&session, 11, &event) == OPENRSPD_DUO_OK);
    assert_event(&event, OPENRSPD_DUO_EVENT_SLAVE_INITIALISED, 10, 11);
    assert(openrspd_duo_route(&session, OPENRSPD_DUO_TUNER_B, &descriptor) == 1 &&
           descriptor == 11);
}

static void test_uninitialise_and_release_order(void)
{
    openrspd_duo_session session;
    openrspd_duo_event event;
    openrspd_duo_session_init(&session);
    assert(openrspd_duo_acquire(&session, 20, OPENRSPD_DUO_ROLE_MASTER,
                                OPENRSPD_DUO_TUNER_B, 8000000u, NULL) ==
           OPENRSPD_DUO_OK);
    assert(openrspd_duo_acquire(&session, 21, OPENRSPD_DUO_ROLE_SLAVE,
                                OPENRSPD_DUO_TUNER_A, 8000000u, NULL) ==
           OPENRSPD_DUO_OK);
    assert(openrspd_duo_initialise(&session, 20, NULL) == OPENRSPD_DUO_OK);
    assert(openrspd_duo_initialise(&session, 21, NULL) == OPENRSPD_DUO_OK);
    assert(openrspd_duo_uninitialise(&session, 20, &event) ==
           OPENRSPD_DUO_STOP_PENDING);
    assert(openrspd_duo_release(&session, 20, &event) == OPENRSPD_DUO_BUSY);
    assert(openrspd_duo_uninitialise(&session, 21, &event) == OPENRSPD_DUO_OK);
    assert_event(&event, OPENRSPD_DUO_EVENT_SLAVE_UNINITIALISED, 20, 21);
    assert(openrspd_duo_uninitialise(&session, 20, NULL) == OPENRSPD_DUO_OK);
    assert(openrspd_duo_release(&session, 20, &event) ==
           OPENRSPD_DUO_STOP_PENDING);
    assert(openrspd_duo_release(&session, 21, &event) == OPENRSPD_DUO_OK);
    assert_event(&event, OPENRSPD_DUO_EVENT_SLAVE_DETACHED, 20, 21);
    assert(openrspd_duo_release(&session, 20, NULL) == OPENRSPD_DUO_OK);
    assert(session.master_selected == 0u && session.slave_selected == 0u);
}

static void test_disconnect_and_validation(void)
{
    openrspd_duo_session session;
    openrspd_duo_event event;
    openrspd_duo_session_init(&session);
    assert(openrspd_duo_acquire(&session, 30, OPENRSPD_DUO_ROLE_MASTER,
                                OPENRSPD_DUO_TUNER_A, 6000000u, NULL) ==
           OPENRSPD_DUO_OK);
    assert(openrspd_duo_acquire(&session, 31, OPENRSPD_DUO_ROLE_SLAVE,
                                OPENRSPD_DUO_TUNER_B, 8000000u, NULL) ==
           OPENRSPD_DUO_BUSY);
    assert(openrspd_duo_acquire(&session, 31, OPENRSPD_DUO_ROLE_SLAVE,
                                OPENRSPD_DUO_TUNER_A, 6000000u, NULL) ==
           OPENRSPD_DUO_BUSY);
    assert(openrspd_duo_acquire(&session, 30, OPENRSPD_DUO_ROLE_SLAVE,
                                OPENRSPD_DUO_TUNER_B, 6000000u, NULL) ==
           OPENRSPD_DUO_BUSY);
    assert(openrspd_duo_disconnect(&session, 30, &event) == OPENRSPD_DUO_OK);
    assert(event.kind == OPENRSPD_DUO_EVENT_NONE);
    assert(session.master_selected == 0u && session.slave_selected == 0u);

    assert(openrspd_duo_acquire(&session, 40, OPENRSPD_DUO_ROLE_MASTER,
                                OPENRSPD_DUO_TUNER_A, 6000000u, NULL) ==
           OPENRSPD_DUO_OK);
    assert(openrspd_duo_acquire(&session, 41, OPENRSPD_DUO_ROLE_SLAVE,
                                OPENRSPD_DUO_TUNER_B, 0u, NULL) ==
           OPENRSPD_DUO_OK);
    assert(openrspd_duo_disconnect(&session, 40, &event) == OPENRSPD_DUO_OK);
    assert_event(&event, OPENRSPD_DUO_EVENT_MASTER_DLL_DISAPPEARED, 41, 40);
    assert(session.master_selected == 0u && session.slave_selected == 0u);

    assert(openrspd_duo_acquire(&session, 42, OPENRSPD_DUO_ROLE_MASTER,
                                OPENRSPD_DUO_TUNER_A, 6000000u, NULL) ==
           OPENRSPD_DUO_OK);
    assert(openrspd_duo_acquire(&session, 43, OPENRSPD_DUO_ROLE_SLAVE,
                                OPENRSPD_DUO_TUNER_B, 0u, NULL) ==
           OPENRSPD_DUO_OK);
    assert(openrspd_duo_write_failure_stops_stream(&session, 42));
    assert(!openrspd_duo_write_failure_stops_stream(&session, 43));
    assert(openrspd_duo_write_failure_stops_stream(&session, 99));
    assert(openrspd_duo_disconnect(&session, 43, &event) == OPENRSPD_DUO_OK);
    assert_event(&event, OPENRSPD_DUO_EVENT_SLAVE_DLL_DISAPPEARED, 42, 43);
    assert(openrspd_duo_role_for_descriptor(&session, 42) ==
           OPENRSPD_DUO_ROLE_MASTER);
}

int main(void)
{
    test_attach_and_initialise_order();
    test_uninitialise_and_release_order();
    test_disconnect_and_validation();
    puts("DUO_SESSION_OK");
    return 0;
}
