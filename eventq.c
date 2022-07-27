/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

--*/

#include "eventq.h"

eventq queue;
eventq_sqe shutdown_sqe;
eventq_sqe echo_sqe;
eventq_sqe timer_sqe;
eventq_sqe create_socket_sqe;
platform_socket app_socket;

typedef enum APP_EVENT_TYPE {
    APP_EVENT_TYPE_SHUTDOWN = APP_EVENT_TYPE_START,
    APP_EVENT_TYPE_ECHO,
    APP_EVENT_TYPE_START_TIMER,
    APP_EVENT_TYPE_CREATE_SOCKET,
} APP_EVENT_TYPE;

PLATFORM_THREAD(main_loop, context) {
    printf("Main loop start\n");
    bool running = true;
    eventq_cqe events[8];
    while (running) {
        uint32_t wait_time = platform_get_wait_time();
        uint32_t count = wait_time == 0 ? 0 : eventq_dequeue(queue, events, 8, wait_time);
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
                            if (!platform_socket_create_listener(&app_socket, queue))
                                printf("Failed to create listener socket\n");
                            else printf("Listener socket = %u\n", (uint32_t)app_socket.fd);
                        } else {
                            if (!platform_socket_create_client(&app_socket, queue))
                                printf("Failed to create client socket\n");
                            else printf("Client socket = %u\n", (uint32_t)app_socket.fd);
                        }
                        break;
                    }
                }
            }
        }
    }
    printf("Main loop end\n");
    PLATFORM_THREAD_RETURN(0);
}

platform_thread thread;

void start_main_loop() {
    printf("Starting main loop\n");
    eventq_initialize(&queue);
    eventq_sqe_initialize(queue, &shutdown_sqe);
    eventq_sqe_initialize(queue, &echo_sqe);
    eventq_sqe_initialize(queue, &timer_sqe);
    eventq_sqe_initialize(queue, &create_socket_sqe);
    platform_thread_create(&thread, main_loop, NULL);
}

void stop_main_loop() {
    printf("Stopping main loop\n");
    eventq_enqueue(queue, &shutdown_sqe, APP_EVENT_TYPE_SHUTDOWN, NULL, 0);
    platform_thread_destroy(thread);
    eventq_sqe_cleanup(queue, &shutdown_sqe);
    eventq_sqe_cleanup(queue, &echo_sqe);
    eventq_sqe_cleanup(queue, &timer_sqe);
    eventq_sqe_cleanup(queue, &create_socket_sqe);
    eventq_cleanup(queue);
}

int CALL main(int argc, char **argv) {
    platform_sockets_init();
    start_main_loop();

    /*platform_sleep(1000);
    printf("Sending echo event\n");
    eventq_enqueue(queue, &echo_sqe, APP_EVENT_TYPE_ECHO, NULL, 0);
    printf("Sending timer event\n");
    eventq_enqueue(queue, &timer_sqe, APP_EVENT_TYPE_START_TIMER, NULL, 100);

    platform_sleep(1000);
    printf("Sending echo event\n");
    eventq_enqueue(queue, &echo_sqe, APP_EVENT_TYPE_ECHO, NULL, 0);*/

    if (argc == 1) {
        printf("Sending create socket (listener)\n");
        eventq_enqueue(queue, &echo_sqe, APP_EVENT_TYPE_CREATE_SOCKET, NULL, 1);
    } else {
        printf("Sending create socket (client)\n");
        eventq_enqueue(queue, &echo_sqe, APP_EVENT_TYPE_CREATE_SOCKET, NULL, 0);
    }
    platform_sleep(10000);

    stop_main_loop();
    return 0;
}
