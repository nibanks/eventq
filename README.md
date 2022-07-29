# eventq

Explores the different platform execution models for IO.

[![Build](https://github.com/nibanks/eventq/actions/workflows/build.yml/badge.svg)](https://github.com/nibanks/eventq/actions/workflows/build.yml)

The "main loop" of the application layer looks like this:

```c
PLATFORM_THREAD(main_loop, context) {
    printf("Main loop start\n");
    app_state* state = (app_state*)context;
    bool running = true;
    eventq_cqe events[8];
    while (running) {
        uint32_t wait_time = platform_get_wait_time();
        uint32_t count = eventq_dequeue(&state->queue, events, 8, wait_time);
        if (count == 0) {
            platform_process_timeout();
        } else {
            for (uint32_t i = 0; i < count; ++i) {
                if (eventq_cqe_get_type(&events[i]) < APP_EVENT_TYPE_START) {
                    platform_process_event(&state->queue, &events[i]);
                } else {
                    switch ((APP_EVENT_TYPE)eventq_cqe_get_type(&events[i])) {
                    ...
                    }
                }
            }
            eventq_return(&state->queue, count);
        }
    }
    printf("Main loop end\n");
    PLATFORM_THREAD_RETURN(0);
}
```

Feel free to look at [eventq.h](./eventq.h) for the abstraction layers and [eventq.c](./eventq.c) for the application layer usage.

## IO Completion Ports

[IO Completion Ports](https://docs.microsoft.com/en-us/windows/win32/fileio/i-o-completion-ports), or IOCP, is the standard mechanism for asynchronous IO on Windows. Generally, it is used to return the completion of a previous asynchronous call made by the application.

To try it out, run the following (**on Windows**):

```Bash
git clone --recursive https://github.com/nibanks/eventq.git
cd eventq && mkdir build && cd build
cmake -G 'Visual Studio 17 2022' -A x64 ..
cmake --build .
./Debug/eventq.exe
```

## ProcessSocketNotifications

[ProcessSocketNotifications](https://docs.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-processsocketnotifications) (PSN for short) is a fairly new Windows socket API that allows for an epoll or kqueue like IO model. It also leverages IO completion ports, but is event driven instead of simply a completion of a previous call.

To try it out, run the following (**on Windows**):

```Bash
git clone --recursive https://github.com/nibanks/eventq.git
cd eventq && mkdir build && cd build
cmake -G 'Visual Studio 17 2022' -A x64 -DUSE_PSN=on ..
cmake --build .
./Debug/eventq.exe
```

## epoll

[epoll](https://man7.org/linux/man-pages/man7/epoll.7.html) is generally viewed as the industry standard way for handling asynchronous operations on not just socket, but all file descriptors, on Linux.

To try it out, run the following (**on Ubuntu**):

```Bash
git clone --recursive https://github.com/nibanks/eventq.git
cd eventq && mkdir build && cd build
cmake -G 'Unix Makefiles' -A x64 ..
cmake --build .
./eventq
```

## liburing

[liburing](https://github.com/axboe/liburing#readme) is a library built on top of [io_uring](https://kernel.dk/io_uring.pdf) which is a fairly new interface for creating shared ring buffers between kernel and user mode to reduce the number of syscalls required to operate on file descriptors, such as sockets.

To try it out, run the following (**on Ubuntu**):

```Bash
sudo apt-get install liburing-dev
git clone --recursive https://github.com/nibanks/eventq.git
cd eventq && mkdir build && cd build
cmake -G 'Unix Makefiles' -A x64 -DUSE_IO_URING=on ..
cmake --build .
./eventq
```

## kqueue

[kqueue](https://man.openbsd.org/kqueue.2) is generally viewed as the industry standard way for handling asynchronous operations on not just socket, but all file descriptors, on FreeBSD or mac.

To try it out, run the following (**on macOS**):

```Bash
git clone --recursive https://github.com/nibanks/eventq.git
cd eventq && mkdir build && cd build
cmake -G 'Unix Makefiles' -A x64 ..
cmake --build .
./eventq
```
