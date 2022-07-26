# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.

project(eventq)
cmake_minimum_required(VERSION 3.16)

if (WIN32)
    set(COMMON_DEFINES WIN32_LEAN_AND_MEAN)
    list(APPEND COMMON_DEFINES EC_IOCP=1)
elseif (APPLE)
    set(COMMON_DEFINES _GNU_SOURCE)
    list(APPEND COMMON_DEFINES EC_KQUEUE=1)
elseif (UNIX)
    set(COMMON_DEFINES _GNU_SOURCE)
    add_compile_options(-pthread)
    if (USE_IO_URING)
        list(APPEND COMMON_DEFINES EC_IO_URING=1)
    else ()
        list(APPEND COMMON_DEFINES EC_EPOLL=1)
    endif ()
endif ()

add_executable(eventq eventq.c)
target_compile_definitions(eventq PRIVATE ${COMMON_DEFINES})
