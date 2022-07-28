/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

--*/

#include "eventq.h"

typedef struct app_state {
    eventq queue;
    eventq_sqe shutdown_sqe;
    eventq_sqe echo_sqe;
    eventq_sqe timer_sqe;
    eventq_sqe create_socket_sqe;
    platform_socket app_socket;
    platform_thread thread;
} app_state;

typedef enum APP_EVENT_TYPE {
    APP_EVENT_TYPE_SHUTDOWN = APP_EVENT_TYPE_START,
    APP_EVENT_TYPE_ECHO,
    APP_EVENT_TYPE_START_TIMER,
    APP_EVENT_TYPE_CREATE_SOCKET,
} APP_EVENT_TYPE;

PLATFORM_THREAD(main_loop, context) {
    printf("Main loop start\n");
    app_state* state = (app_state*)context;
    bool running = true;
    eventq_cqe events[8];
    while (running) {
        uint32_t wait_time = platform_get_wait_time();
        uint32_t count = wait_time == 0 ? 0 : eventq_dequeue(&state->queue, events, 8, wait_time);
        if (count == 0) {
            platform_process_timeout();
        } else {
            for (uint32_t i = 0; i < count; ++i) {
                if (eventq_cqe_get_type(&events[i]) < APP_EVENT_TYPE_START) {
                    platform_process_event(&events[i]);
                } else {
                    switch ((APP_EVENT_TYPE)eventq_cqe_get_type(&events[i])) {
                    case APP_EVENT_TYPE_SHUTDOWN:
                        printf("Shutdown event received\n");
                        running = false;
                        break;
                    case APP_EVENT_TYPE_ECHO:
                        printf("Echo event received\n");
                        break;
                    case APP_EVENT_TYPE_START_TIMER:
                        platform_wait_time = eventq_cqe_get_status(&events[i]);
                        printf("Starting %u ms timer\n", platform_wait_time);
                        break;
                    case APP_EVENT_TYPE_CREATE_SOCKET:
                        if (eventq_cqe_get_status(&events[i])) {
                            if (!platform_socket_create_listener(&state->app_socket, &state->queue))
                                printf("Failed to create listener socket\n");
                        } else {
                            if (!platform_socket_create_client(&state->app_socket, &state->queue))
                                printf("Failed to create client socket\n");
                        }
                        break;
                    }
                }
                eventq_return(&state->queue, &events[i]);
            }
        }
    }
    printf("Main loop end\n");
    PLATFORM_THREAD_RETURN(0);
}

void start_main_loop(app_state* state) {
    if (!eventq_initialize(&state->queue)) printf("eventq_initialize failed\n");
    eventq_sqe_initialize(&state->queue, &state->shutdown_sqe);
    eventq_sqe_initialize(&state->queue, &state->echo_sqe);
    eventq_sqe_initialize(&state->queue, &state->timer_sqe);
    eventq_sqe_initialize(&state->queue, &state->create_socket_sqe);
    platform_thread_create(&state->thread, main_loop, state);
}

void stop_main_loop(app_state* state) {
    eventq_enqueue(&state->queue, &state->shutdown_sqe, APP_EVENT_TYPE_SHUTDOWN, NULL, 0);
    platform_thread_destroy(state->thread);
    eventq_sqe_cleanup(&state->queue, &state->shutdown_sqe);
    eventq_sqe_cleanup(&state->queue, &state->echo_sqe);
    eventq_sqe_cleanup(&state->queue, &state->timer_sqe);
    eventq_sqe_cleanup(&state->queue, &state->create_socket_sqe);
    eventq_cleanup(&state->queue);
}

int CALL main(int argc, char **argv) {
    platform_sockets_init();

    app_state state1 = {0}, state2 = {0};
    start_main_loop(&state1);
    start_main_loop(&state2);

    platform_sleep(1000);
    eventq_enqueue(&state1.queue, &state1.echo_sqe, APP_EVENT_TYPE_ECHO, NULL, 0);
    eventq_enqueue(&state1.queue, &state1.timer_sqe, APP_EVENT_TYPE_START_TIMER, NULL, 100);

    platform_sleep(1000);
    eventq_enqueue(&state1.queue, &state1.echo_sqe, APP_EVENT_TYPE_ECHO, NULL, 0);

    eventq_enqueue(&state1.queue, &state1.create_socket_sqe, APP_EVENT_TYPE_CREATE_SOCKET, NULL, 1);
    platform_sleep(100);
    eventq_enqueue(&state2.queue, &state2.create_socket_sqe, APP_EVENT_TYPE_CREATE_SOCKET, NULL, 0);
    platform_sleep(1000);

    stop_main_loop(&state1);
    stop_main_loop(&state2);

    return 0;
}
