/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

--*/

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

//
// Minimal platform abstractions.
//

#define PLATFORM_SOCKET_PORT 9009

typedef enum PLATFORM_EVENT_TYPE {
    PLATFORM_EVENT_TYPE_SOCKET_RECEIVE,
    PLATFORM_EVENT_TYPE_SOCKET_SEND,
} PLATFORM_EVENT_TYPE;

#ifdef _WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS 1
#include <windows.h>
#include <WinSock2.h>
#define CALL __cdecl
typedef HANDLE platform_thread;
#define PLATFORM_THREAD(FuncName, CtxVarName) DWORD WINAPI FuncName(_In_ void* CtxVarName)
#define PLATFORM_THREAD_RETURN(Status) return (DWORD)(Status)
void platform_thread_create(platform_thread* thread, LPTHREAD_START_ROUTINE func, void* ctx) {
    *thread = CreateThread(NULL, 0, func, ctx, 0, NULL);
}
void platform_thread_destroy(platform_thread thread) {
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
}
void platform_sleep(uint32_t milliseconds) { Sleep(milliseconds); }
uint64_t platform_perf_freq;
uint64_t platform_now_us(void) {
    uint64_t Count;
    (void)QueryPerformanceCounter((LARGE_INTEGER*)&Count);
    uint64_t High = (Count >> 32) * 1000000;
    uint64_t Low = (Count & 0xFFFFFFFF) * 1000000;
    return
        ((High / platform_perf_freq) << 32) +
        ((Low + ((High % platform_perf_freq) << 32)) / platform_perf_freq);
}
#if EC_PSN
#include <stdlib.h>
typedef DWORD (WSAAPI *PPROCESS_SOCKET_NOTIFICATIONS_ROUTINE)(
    HANDLE                   completionPort,
    UINT32                   registrationCount,
    SOCK_NOTIFY_REGISTRATION *registrationInfos,
    UINT32                   timeoutMs,
    ULONG                    completionCount,
    OVERLAPPED_ENTRY         *completionPortEntries,
    UINT32                   *receivedEntryCount
    );
HANDLE hws2_32 = NULL;
PPROCESS_SOCKET_NOTIFICATIONS_ROUTINE fn_ProcessSocketNotifications = NULL;
#endif
int platform_socket_close(SOCKET s) { return closesocket(s); }
void platform_init(void) {
    (void)QueryPerformanceFrequency((LARGE_INTEGER*)&platform_perf_freq);
#if EC_PSN
    hws2_32 = LoadLibrary("Ws2_32.dll");
    fn_ProcessSocketNotifications = (PPROCESS_SOCKET_NOTIFICATIONS_ROUTINE)GetProcAddress(hws2_32, "ProcessSocketNotifications");
    if (fn_ProcessSocketNotifications == NULL) {
        printf("ProcessSocketNotifications not available!\n");
        exit(0);
    }
#endif
    WSADATA WsaData; WSAStartup(MAKEWORD(2, 2), &WsaData);
}
#else // !_WIN32
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#define CALL
typedef pthread_t platform_thread;
#define PLATFORM_THREAD(FuncName, CtxVarName) void* FuncName(void* CtxVarName)
#define PLATFORM_THREAD_RETURN(Status) return NULL
void platform_thread_create(platform_thread* thread, void *(*func)(void *), void* ctx) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_create(thread, &attr, func, ctx);
    pthread_attr_destroy(&attr);
}
void platform_thread_destroy(platform_thread thread) {
    pthread_equal(thread, pthread_self());
    pthread_join(thread, NULL);
}
void platform_sleep(uint32_t milliseconds) { usleep(milliseconds * 1000); }
uint64_t platform_now_us(void) {
    struct timespec time = {0};
    (void)clock_gettime(CLOCK_MONOTONIC, &time);
    return (time.tv_sec * 1000000) + (time.tv_nsec / 1000);
}
typedef int SOCKET;
int platform_socket_close(SOCKET s) { return close(s); }
void platform_init(void) { }
#endif // !_WIN32

//
// Different implementations of the event queue interface.
//

#if EC_IOCP

typedef HANDLE eventq;
typedef struct eventq_sqe {
    OVERLAPPED;
    uint32_t type;
} eventq_sqe;
typedef OVERLAPPED_ENTRY eventq_cqe;

typedef struct platform_socket {
    eventq_sqe recv_sqe;
    eventq_sqe send_sqe;
    SOCKET fd;
    WSABUF buf;
    struct sockaddr recv_addr;
    int recv_addr_len;
    uint8_t buffer[1500];
} platform_socket;

bool eventq_initialize(eventq* queue) {
    *queue = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    return *queue != NULL;
}
void eventq_cleanup(eventq* queue) {
    CloseHandle(*queue);
}
bool eventq_sqe_initialize(eventq* queue, eventq_sqe* sqe) { return true; }
void eventq_sqe_cleanup(eventq* queue, eventq_sqe* sqe) { }
bool eventq_socket_create(eventq* queue, platform_socket* sock) {
    sock->recv_sqe.type = PLATFORM_EVENT_TYPE_SOCKET_RECEIVE;
    sock->send_sqe.type = PLATFORM_EVENT_TYPE_SOCKET_SEND;
    sock->fd = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (sock->fd == (SOCKET)-1) return false;
    if (!SetFileCompletionNotificationModes((HANDLE)sock->fd, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS) ||
        *queue != CreateIoCompletionPort((HANDLE)sock->fd, *queue, (ULONG_PTR)sock, 0)) {
        closesocket(sock->fd);
        return false;
    }
    return true;
}
bool eventq_socket_receive_start(eventq* queue, platform_socket* sock) {
    int result;
    DWORD bytesRecv = 0;
    DWORD flags = 0;
    do {
        sock->recv_addr_len = sizeof(sock->recv_addr);
        sock->buf.buf = sock->buffer;
        sock->buf.len = sizeof(sock->buffer);
        ZeroMemory(&sock->recv_sqe, sizeof(OVERLAPPED));
        result = WSARecvFrom(sock->fd, &sock->buf, 1, &bytesRecv, &flags, &sock->recv_addr, &sock->recv_addr_len, (OVERLAPPED*)&sock->recv_sqe, NULL);
        if (result == SOCKET_ERROR) {
            if (WSAGetLastError() != WSA_IO_PENDING) {
                printf("WSARecvFrom failed with error: %d\n", WSAGetLastError());
                return false;
            }
        } else {
            printf("Receive complete, %u bytes\n", (uint32_t)bytesRecv);
        }
    } while (result != SOCKET_ERROR);
    return true;
}
void eventq_socket_receive_complete(eventq* queue, eventq_cqe* cqe) {
    printf("Receive complete, %u bytes\n", cqe->dwNumberOfBytesTransferred);
    eventq_socket_receive_start(queue, (platform_socket*)cqe->lpOverlapped);
}
void eventq_socket_send_start(eventq* queue, platform_socket* sock, uint32_t length) {
    sock->buf.buf = sock->buffer;
    sock->buf.len = length;
    ZeroMemory(&sock->send_sqe, sizeof(OVERLAPPED));
    printf("Sending %u bytes\n", sock->buf.len);
    int result = WSASend(sock->fd, &sock->buf, 1, NULL, 0, (OVERLAPPED*)&sock->send_sqe, NULL);
    if (result == SOCKET_ERROR) {
        if (WSAGetLastError() != WSA_IO_PENDING) {
            printf("WSASend failed with error: %d\n", WSAGetLastError());
        }
    }
}
void eventq_socket_send_complete(eventq* queue, eventq_cqe* cqe) { }
void eventq_enqueue(eventq* queue, eventq_sqe* sqe, uint32_t type, void* user_data, uint32_t status) {
    memset(sqe, 0, sizeof(*sqe));
    sqe->type = type;
    PostQueuedCompletionStatus(*queue, status, (ULONG_PTR)user_data, (OVERLAPPED*)sqe);
}
uint32_t eventq_dequeue(eventq* queue, eventq_cqe* events, uint32_t count, uint32_t wait_time) {
    uint32_t out_count;
    GetQueuedCompletionStatusEx(*queue, events, count, &out_count, wait_time, FALSE); // TODO - How to handle errors?
    return out_count;
}
void eventq_return(eventq* queue, uint32_t count) { }
uint32_t eventq_cqe_get_type(eventq_cqe* cqe) { return ((eventq_sqe*)cqe->lpOverlapped)->type; }
void* eventq_cqe_get_user_data(eventq_cqe* cqe) { return (void*)cqe->lpCompletionKey; }
uint32_t eventq_cqe_get_status(eventq_cqe* cqe) { return cqe->dwNumberOfBytesTransferred; }

#elif EC_PSN

typedef HANDLE eventq;
typedef struct eventq_sqe {
    uint32_t type;
    void* user_data;
} eventq_sqe;
typedef OVERLAPPED_ENTRY eventq_cqe;

typedef struct platform_socket {
    eventq_sqe recv_sqe;
    SOCKET fd;
    struct sockaddr recv_addr;
    int recv_addr_len;
    uint8_t buffer[1500];
} platform_socket;

bool eventq_initialize(eventq* queue) {
    *queue = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    return *queue != NULL;
}
void eventq_cleanup(eventq* queue) {
    CloseHandle(*queue);
}
bool eventq_sqe_initialize(eventq* queue, eventq_sqe* sqe) { return true; }
void eventq_sqe_cleanup(eventq* queue, eventq_sqe* sqe) { }
bool eventq_socket_create(eventq* queue, platform_socket* sock) {
    sock->recv_sqe.type = PLATFORM_EVENT_TYPE_SOCKET_RECEIVE;
    sock->recv_sqe.user_data = sock;
    sock->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock->fd == (SOCKET)-1) return false;
    uint32_t nonBlocking = 1;
    if (ioctlsocket(sock->fd, FIONBIO, &nonBlocking) != 0) {
        closesocket(sock->fd);
        return false;
    }
    return true;
}
bool eventq_socket_receive_start(eventq* queue, platform_socket* sock) {
    SOCK_NOTIFY_REGISTRATION registration = {0};
    registration.completionKey = &sock->recv_sqe;
    registration.eventFilter = SOCK_NOTIFY_EVENT_IN;
    registration.operation = SOCK_NOTIFY_OP_ENABLE;
    registration.triggerFlags = SOCK_NOTIFY_TRIGGER_EDGE | SOCK_NOTIFY_TRIGGER_PERSISTENT;
    registration.socket = sock->fd;
    uint32_t Result = fn_ProcessSocketNotifications(*queue, 1, &registration, 0, 0, NULL, NULL);
    if (Result != ERROR_SUCCESS) {
        printf("ProcessSocketNotifications failed with error: %d\n", Result);
        return false;
    }
    if (registration.registrationResult != ERROR_SUCCESS) {
        printf("ProcessSocketNotifications failed with error: %d\n", registration.registrationResult);
        return false;
    }
    return true;
}
void eventq_socket_receive_complete(eventq* queue, eventq_cqe* cqe) {
    platform_socket* sock = ((eventq_sqe*)cqe->lpCompletionKey)->user_data;
    int result;
    do {
        sock->recv_addr_len = sizeof(sock->recv_addr);
        result = recvfrom(sock->fd, sock->buffer, sizeof(sock->buffer), 0, &sock->recv_addr, &sock->recv_addr_len);
        if (result <= 0) {
            if (WSAGetLastError() != WSAEWOULDBLOCK)
                printf("recvfrom failed, %d\n", WSAGetLastError());
            break;
        }
        printf("Receive complete, %d bytes\n", result);
    } while (true);
}
void eventq_socket_send_start(eventq* queue, platform_socket* sock, uint32_t length) {
    printf("Sending %u bytes\n", length);
    int result = send(sock->fd, sock->buffer, length, 0);
    if (result == SOCKET_ERROR) {
        printf("send failed with error: %d\n", WSAGetLastError());
    }
}
void eventq_socket_send_complete(eventq* queue, eventq_cqe* cqe) { }
void eventq_enqueue(eventq* queue, eventq_sqe* sqe, uint32_t type, void* user_data, uint32_t status) {
    memset(sqe, 0, sizeof(*sqe));
    sqe->type = type;
    sqe->user_data = user_data;
    PostQueuedCompletionStatus(*queue, status, (ULONG_PTR)sqe, (OVERLAPPED*)sqe);
}
uint32_t eventq_dequeue(eventq* queue, eventq_cqe* events, uint32_t count, uint32_t wait_time) {
    uint32_t out_count;
    GetQueuedCompletionStatusEx(*queue, events, count, &out_count, wait_time, FALSE); // TODO - How to handle errors?
    return out_count;
}
void eventq_return(eventq* queue, uint32_t count) { }
uint32_t eventq_cqe_get_type(eventq_cqe* cqe) { return ((eventq_sqe*)cqe->lpCompletionKey)->type; }
void* eventq_cqe_get_user_data(eventq_cqe* cqe) { return ((eventq_sqe*)cqe->lpCompletionKey)->user_data; }
uint32_t eventq_cqe_get_status(eventq_cqe* cqe) { return cqe->dwNumberOfBytesTransferred; }

#elif EC_KQUEUE

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <fcntl.h>

typedef int eventq;
typedef struct eventq_sqe {
    uint32_t type;
    void* user_data;
    uint32_t status;
} eventq_sqe;
typedef struct kevent eventq_cqe;

typedef struct platform_socket {
    eventq_sqe recv_sqe;
    eventq_sqe send_sqe;
    SOCKET fd;
    struct sockaddr recv_addr;
    socklen_t recv_addr_len;
    uint8_t buffer[1500];
} platform_socket;

bool eventq_initialize(eventq* queue) {
    *queue = kqueue();
    return *queue != -1;
}
void eventq_cleanup(eventq* queue) {
    close(*queue);
}
bool eventq_sqe_initialize(eventq* queue, eventq_sqe* sqe) { return true; }
void eventq_sqe_cleanup(eventq* queue, eventq_sqe* sqe) { }
bool eventq_socket_create(eventq* queue, platform_socket* sock) {
    sock->recv_sqe.type = PLATFORM_EVENT_TYPE_SOCKET_RECEIVE;
    sock->recv_sqe.user_data = sock;
    sock->send_sqe.type = PLATFORM_EVENT_TYPE_SOCKET_SEND;
    sock->send_sqe.user_data = sock;
    sock->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock->fd == (SOCKET)-1) return false;
    int Flags = fcntl(sock->fd, F_GETFL, NULL);
    if (Flags < 0 || fcntl(sock->fd, F_SETFL, Flags | O_NONBLOCK) < 0) {
        close(sock->fd);
        return false;
    }
    return true;
}
bool eventq_socket_receive_start(eventq* queue, platform_socket* sock) {
    struct kevent event = {0};
    EV_SET(&event, sock->fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, (void*)&sock->recv_sqe);
    return 0 <= kevent(*queue, &event, 1, NULL, 0, NULL);
}
void eventq_socket_receive_complete(eventq* queue, eventq_cqe* cqe) {
    platform_socket* sock = ((eventq_sqe*)cqe->udata)->user_data;
    int result;
    do {
        sock->recv_addr_len = sizeof(sock->recv_addr);
        result = recvfrom(sock->fd, sock->buffer, sizeof(sock->buffer), 0, &sock->recv_addr, &sock->recv_addr_len);
        if (result <= 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                printf("recvfrom failed, %d\n", errno);
            break;
        }
        printf("Receive complete, %d bytes\n", result);
    } while (true);
}
void eventq_socket_send_start(eventq* queue, platform_socket* sock, uint32_t length) {
    printf("Sending %u bytes\n", length);
    if (send(sock->fd, sock->buffer, length, 0) < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // TODO - Need to queue the send if we support multiple
            struct kevent event = {0};
            EV_SET(&event, sock->fd, EVFILT_WRITE, EV_ADD | EV_ONESHOT | EV_CLEAR, 0, 0, (void*)&sock->send_sqe);
            if (kevent(*queue, &event, 1, NULL, 0, NULL) < 0) {
                printf("kevent(send) failed, %d\n", errno);
            }
        } else {
            printf("send failed, %d\n", errno);
        }
    }
}
void eventq_socket_send_complete(eventq* queue, eventq_cqe* cqe) {
    // TODO - Send queued sends
}
void eventq_enqueue(eventq* queue, eventq_sqe* sqe, uint32_t type, void* user_data, uint32_t status) {
    struct kevent event = {0};
    sqe->type = type;
    sqe->user_data = user_data;
    sqe->status = status;
    EV_SET(&event, (uintptr_t)sqe, EVFILT_USER, EV_ADD | EV_CLEAR, NOTE_TRIGGER, 0, sqe);
    if (0 != kevent(*queue, &event, 1, NULL, 0, NULL)) {
        printf("kevent enqueue failed\n");
    }
}
uint32_t eventq_dequeue(eventq* queue, eventq_cqe* events, uint32_t count, uint32_t wait_time) {
    struct timespec timeout = {0, 0};
    if (wait_time != UINT32_MAX) {
        timeout.tv_sec = (wait_time / 1000);
        timeout.tv_nsec = ((wait_time % 1000) * 1000000);
    }
    int result;
    do {
        result = kevent(*queue, NULL, 0, events, count, wait_time == UINT32_MAX ? NULL : &timeout);
    } while ((result == -1L) && (errno == EINTR));
    return (uint32_t)result;
}
void eventq_return(eventq* queue, uint32_t count) { }
uint32_t eventq_cqe_get_type(eventq_cqe* cqe) { return ((eventq_sqe*)cqe->udata)->type; }
void* eventq_cqe_get_user_data(eventq_cqe* cqe) { return ((eventq_sqe*)cqe->udata)->user_data; }
uint32_t eventq_cqe_get_status(eventq_cqe* cqe) { return ((eventq_sqe*)cqe->udata)->status; }

#elif EC_EPOLL

#include <sys/epoll.h>
#include <sys/eventfd.h>

typedef int eventq;
typedef struct eventq_sqe {
    int fd;
    uint32_t type;
    void* user_data;
    uint32_t status;
} eventq_sqe;
typedef struct epoll_event eventq_cqe;

typedef struct platform_socket {
    eventq_sqe recv_sqe;
    eventq_sqe send_sqe;
    SOCKET fd;
    struct sockaddr recv_addr;
    int recv_addr_len;
    uint8_t buffer[1500];
} platform_socket;

bool eventq_initialize(eventq* queue) {
    *queue = epoll_create1(EPOLL_CLOEXEC);
    return *queue != -1;
}
void eventq_cleanup(eventq* queue) {
    close(*queue);
}
bool eventq_sqe_initialize(eventq* queue, eventq_sqe* sqe) {
    sqe->fd = eventfd(0, EFD_CLOEXEC);
    if (sqe->fd == -1) return false;
    struct epoll_event event = { .events = EPOLLIN | EPOLLET, .data = { .ptr = sqe } };
    if (epoll_ctl(*queue, EPOLL_CTL_ADD, sqe->fd, &event) != 0) {
        close(sqe->fd);
        return false;
    }
    return true;
}
void eventq_sqe_cleanup(eventq* queue, eventq_sqe* sqe) {
    epoll_ctl(*queue, EPOLL_CTL_DEL, sqe->fd, NULL);
    close(sqe->fd);
}
bool eventq_socket_create(eventq* queue, platform_socket* sock) {
    sock->recv_sqe.type = PLATFORM_EVENT_TYPE_SOCKET_RECEIVE;
    sock->recv_sqe.user_data = sock;
    sock->send_sqe.type = PLATFORM_EVENT_TYPE_SOCKET_SEND;
    sock->send_sqe.user_data = sock;
    return (sock->fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP)) != (SOCKET)-1;
}
bool eventq_socket_receive_start(eventq* queue, platform_socket* sock) {
    struct epoll_event event = { .events = EPOLLIN, .data = { .ptr = &sock->recv_sqe } };
    return 0 == epoll_ctl(*queue, EPOLL_CTL_ADD, sock->fd, &event);
}
void eventq_socket_receive_complete(eventq* queue, eventq_cqe* cqe) {
    platform_socket* sock = (platform_socket*)cqe->data.ptr;
    int result;
    do {
        sock->recv_addr_len = sizeof(sock->recv_addr);
        result = recvfrom(sock->fd, sock->buffer, sizeof(sock->buffer), 0, &sock->recv_addr, &sock->recv_addr_len);
        if (result <= 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                printf("recvfrom failed, %d\n", errno);
            break;
        }
        printf("Receive complete, %d bytes\n", result);
    } while (true);
}
void eventq_socket_send_start(eventq* queue, platform_socket* sock, uint32_t length) {
    printf("Sending %u bytes\n", length);
    if (send(sock->fd, sock->buffer, length, 0) < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // TODO - Need to queue the send if we support multiple
            struct epoll_event event = { .events = EPOLLIN | EPOLLOUT, .data = { .ptr = &sock->send_sqe } };
            if (0 != epoll_ctl(*queue, EPOLL_CTL_ADD, sock->fd, &event)) {
                printf("epoll_ctl (send) failed\n");
            }
        } else {
            printf("send failed, %d\n", errno);
        }
    }
}
void eventq_socket_send_complete(eventq* queue, eventq_cqe* cqe) {
    // TODO - Send queued sends
}
void eventq_enqueue(eventq* queue, eventq_sqe* sqe, uint32_t type, void* user_data, uint32_t status) {
    sqe->type = type;
    sqe->user_data = user_data;
    sqe->status = status;
    eventfd_write(sqe->fd, 1);
}
uint32_t eventq_dequeue(eventq* queue, eventq_cqe* events, uint32_t count, uint32_t wait_time) {
    const int timeout = wait_time == UINT32_MAX ? -1 : (int)wait_time;
    int result;
    do {
        result = epoll_wait(*queue, events, count, timeout);
    } while ((result == -1L) && (errno == EINTR));
    return (uint32_t)result;
}
void eventq_return(eventq* queue, uint32_t count) { }
uint32_t eventq_cqe_get_type(eventq_cqe* cqe) { return ((eventq_sqe*)cqe->data.ptr)->type; }
void* eventq_cqe_get_user_data(eventq_cqe* cqe) { return ((eventq_sqe*)cqe->data.ptr)->user_data; }
uint32_t eventq_cqe_get_status(eventq_cqe* cqe) { return ((eventq_sqe*)cqe->data.ptr)->status; }

#elif EC_IO_URING

#include <liburing.h>

typedef struct io_uring eventq;
typedef struct eventq_sqe {
    uint32_t type;
    void* user_data;
    uint32_t status;
} eventq_sqe;
typedef struct io_uring_cqe* eventq_cqe;

typedef struct platform_socket {
    eventq_sqe sqe;
    SOCKET fd;
    struct sockaddr recv_addr;
    int recv_addr_len;
    uint8_t buffer[1500];
} platform_socket;

bool eventq_initialize(eventq* queue) {
    return 0 == io_uring_queue_init(256, queue, 0);
}
void eventq_cleanup(eventq* queue) {
    io_uring_queue_exit(queue);
}
bool eventq_sqe_initialize(eventq* queue, eventq_sqe* sqe) { }
void eventq_sqe_cleanup(eventq* queue, eventq_sqe* sqe) { }
bool eventq_socket_create(eventq* queue, platform_socket* sock) {
    return (sock->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) != (SOCKET)-1;
}
bool eventq_socket_receive_start(eventq* queue, platform_socket* sock) {
    sock->sqe.type = PLATFORM_EVENT_TYPE_SOCKET_RECEIVE;
    struct io_uring_sqe *io_sqe = io_uring_get_sqe(queue);
    if (io_sqe == NULL) {
        printf("io_uring_get_sqe(recv) returned NULL\n");
        return false;
    }
    io_uring_prep_recv(io_sqe, sock->fd, sock->buffer, sizeof(sock->buffer), 0);
    io_uring_sqe_set_data(io_sqe, &sock->sqe);
    io_uring_submit(queue); // TODO - Extract to separate function?
    return true;
}
void eventq_socket_receive_complete(eventq* queue, eventq_cqe* cqe) {
    printf("Receive complete, %d bytes\n", (*cqe)->res);
    eventq_socket_receive_start(queue, (platform_socket*)(*cqe)->user_data);
}
void eventq_socket_send_start(eventq* queue, platform_socket* sock, uint32_t length) {
    sock->sqe.type = PLATFORM_EVENT_TYPE_SOCKET_SEND;
    printf("Sending %u bytes\n", length);
    struct io_uring_sqe *io_sqe = io_uring_get_sqe(queue);
    if (io_sqe == NULL) {
        printf("io_uring_get_sqe(send) returned NULL\n");
        return;
    }
    io_uring_prep_send(io_sqe, sock->fd, sock->buffer, length, 0);
    io_uring_sqe_set_data(io_sqe, &sock->sqe);
    io_uring_submit(queue); // TODO - Extract to separate function?
}
void eventq_socket_send_complete(eventq* queue, eventq_cqe* cqe) { }
void eventq_enqueue(eventq* queue, eventq_sqe* sqe, uint32_t type, void* user_data, uint32_t status) {
    sqe->type = type;
    sqe->user_data = user_data;
    sqe->status = status;
    struct io_uring_sqe *io_sqe = io_uring_get_sqe(queue);
    if (io_sqe == NULL) {
        printf("io_uring_get_sqe returned NULL\n");
        return;
    }
    io_uring_prep_nop(io_sqe);
    io_uring_sqe_set_data(io_sqe, sqe);
    io_uring_submit(queue); // TODO - Extract to separate function?
}
uint32_t eventq_dequeue(eventq* queue, eventq_cqe* events, uint32_t count, uint32_t wait_time) {
    int result = io_uring_peek_batch_cqe(queue, events, count);
    if (result > 0 || wait_time == 0) return result;
    if (wait_time != UINT32_MAX) {
        struct __kernel_timespec timeout;
        timeout.tv_sec = (wait_time / 1000);
        timeout.tv_nsec = ((wait_time % 1000) * 1000000);
        (void)io_uring_wait_cqe_timeout(queue, events, &timeout);
    } else {
        (void)io_uring_wait_cqe(queue, events);
    }
    return io_uring_peek_batch_cqe(queue, events, count);
}
void eventq_return(eventq* queue, uint32_t count) {
    io_uring_cq_advance(queue, count);
}
uint32_t eventq_cqe_get_type(eventq_cqe* cqe) { return ((eventq_sqe*)io_uring_cqe_get_data(*cqe))->type; }
void* eventq_cqe_get_user_data(eventq_cqe* cqe) { return ((eventq_sqe*)io_uring_cqe_get_data(*cqe))->user_data; }
uint32_t eventq_cqe_get_status(eventq_cqe* cqe) { return ((eventq_sqe*)io_uring_cqe_get_data(*cqe))->status; }

#else

#error "Must define an execution context"

#endif

//
// Minimal abstraction layer for the "platform" that represents all execution
// logic underneath the application.
//

#define APP_EVENT_TYPE_START 0x8000

uint64_t platform_now_us_cache = 0;
uint64_t platform_next_wait_time = UINT64_MAX;
// Returns the next wait time in milliseconds.
uint32_t platform_process_timers(void) {
    platform_now_us_cache = platform_now_us();
    if (platform_next_wait_time == UINT64_MAX) return UINT32_MAX;
    if (platform_now_us_cache >= platform_next_wait_time) {
        printf("Timeout\n");
        platform_next_wait_time = UINT64_MAX;
        return 0;
    }
    uint64_t wait_time = (platform_next_wait_time - platform_now_us_cache) / 1000;
    return wait_time > UINT32_MAX ? UINT32_MAX : (uint32_t)wait_time;
}
void platform_set_timer_ms(uint32_t wait_time_ms) {
    printf("Starting %u ms timer\n", wait_time_ms);
    platform_now_us_cache = platform_now_us();
    platform_next_wait_time = platform_now_us_cache + (wait_time_ms * 1000);
}
void platform_process_event(eventq* queue, eventq_cqe* cqe) {
    switch (eventq_cqe_get_type(cqe)) {
    case PLATFORM_EVENT_TYPE_SOCKET_RECEIVE:
        eventq_socket_receive_complete(queue, cqe);
        break;
    case PLATFORM_EVENT_TYPE_SOCKET_SEND:
        eventq_socket_send_complete(queue, cqe);
        break;
    default:
        printf("Unexpected Event %u\n", eventq_cqe_get_type(cqe));
        break;
    }
}

bool platform_socket_create_listener(platform_socket* sock, eventq* queue) {
    if (!eventq_socket_create(queue, sock)) return false;
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PLATFORM_SOCKET_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    int res = bind(sock->fd, (struct sockaddr*)&addr, sizeof(addr));
    if (res == -1) {
        platform_socket_close(sock->fd);
        return false;
    }
    if (!eventq_socket_receive_start(queue, sock)) {
        platform_socket_close(sock->fd);
        return false;
    }
    return true;
}

bool platform_socket_create_client(platform_socket* sock, eventq* queue) {
    if (!eventq_socket_create(queue, sock)) return false;
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = INADDR_ANY;
    int res = bind(sock->fd, (struct sockaddr*)&addr, sizeof(addr));
    if (res == -1) {
        platform_socket_close(sock->fd);
        return false;
    }
    addr.sin_port = htons(PLATFORM_SOCKET_PORT);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    res = connect(sock->fd, (struct sockaddr*)&addr, sizeof(addr));
    if (res == -1) {
        platform_socket_close(sock->fd);
        return false;
    }
    for (uint32_t i = 0; i < 50; ++i)
        eventq_socket_send_start(queue, sock, 1);
    return true;
}
