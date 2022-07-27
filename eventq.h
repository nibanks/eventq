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
void platform_sockets_init(void) { WSADATA WsaData; WSAStartup(MAKEWORD(2, 2), &WsaData); }
int platform_socket_close(SOCKET s) { return closesocket(s); }
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
typedef int SOCKET;
void platform_sockets_init(void) { }
int platform_socket_close(SOCKET s) { return close(s); }
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
    eventq_sqe sqe;
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
void eventq_cleanup(eventq queue) {
    CloseHandle(queue);
}
bool eventq_sqe_initialize(eventq queue, eventq_sqe* sqe) { return true; }
void eventq_sqe_cleanup(eventq queue, eventq_sqe* sqe) { }
bool eventq_socket_create(eventq queue, platform_socket* sock) {
    sock->fd = WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (sock->fd == (SOCKET)-1) return false;
    if (queue != CreateIoCompletionPort((HANDLE)sock->fd, queue, (ULONG_PTR)sock, 0)) {
        closesocket(sock->fd);
        return false;
    }
    return true;
}
bool eventq_socket_receive_start(eventq queue, platform_socket* sock) {
    sock->sqe.type = PLATFORM_EVENT_TYPE_SOCKET_RECEIVE;
    sock->recv_addr_len = sizeof(sock->recv_addr);
    sock->buf.buf = sock->buffer;
    sock->buf.len = sizeof(sock->buffer);
    ZeroMemory(&sock->sqe, sizeof(OVERLAPPED));
    DWORD bytesRecv = 0;
    DWORD flags = 0;
    int result = WSARecvFrom(sock->fd, &sock->buf, 1, &bytesRecv, &flags, &sock->recv_addr, &sock->recv_addr_len, (OVERLAPPED*)&sock->sqe, NULL);
    if (result == SOCKET_ERROR) {
        if (WSAGetLastError() != WSA_IO_PENDING) {
            printf("WSARecvFrom failed with error: %d\n", WSAGetLastError());
            return false;
        }
    }
    return true;
}
void eventq_socket_receive_complete(eventq_cqe* cqe) {
    printf("Receive complete, %u bytes\n", cqe->dwNumberOfBytesTransferred);
}
bool eventq_socket_send_start(eventq queue, platform_socket* sock) {
    sock->sqe.type = PLATFORM_EVENT_TYPE_SOCKET_SEND;
    sock->buf.buf = sock->buffer;
    sock->buf.len = sizeof(sock->buffer) - 10;
    ZeroMemory(&sock->sqe, sizeof(OVERLAPPED));
    printf("Sending %u bytes\n", sock->buf.len);
    int result = WSASend(sock->fd, &sock->buf, 1, NULL, 0, (OVERLAPPED*)&sock->sqe, NULL);
    if (result == SOCKET_ERROR) {
        if (WSAGetLastError() != WSA_IO_PENDING) {
            printf("WSASend failed with error: %d\n", WSAGetLastError());
            return false;
        }
    }
    return true;
}
void eventq_socket_send_complete(eventq_cqe* cqe) {
    printf("Send complete\n");
}
void eventq_enqueue(eventq queue, eventq_sqe* sqe, uint32_t type, void* user_data, uint32_t status) {
    memset(sqe, 0, sizeof(*sqe));
    sqe->type = type;
    PostQueuedCompletionStatus(queue, status, (ULONG_PTR)user_data, (OVERLAPPED*)sqe);
}
uint32_t eventq_dequeue(eventq queue, eventq_cqe* events, uint32_t count, uint32_t wait_time) {
    uint32_t out_count;
    GetQueuedCompletionStatusEx(queue, events, count, &out_count, wait_time, FALSE); // TODO - How to handle errors?
    return out_count;
}
uint32_t eventq_cqe_get_type(eventq_cqe* cqe) { return ((eventq_sqe*)cqe->lpOverlapped)->type; }
void* eventq_cqe_get_user_data(eventq_cqe* cqe) { return (void*)cqe->lpCompletionKey; }
uint32_t eventq_cqe_get_status(eventq_cqe* cqe) { return cqe->dwNumberOfBytesTransferred; }

#elif EC_KQUEUE

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

typedef int eventq;
typedef struct eventq_sqe {
    uint32_t type;
    void* user_data;
    uint32_t status;
} eventq_sqe;
typedef struct kevent eventq_cqe;

typedef struct platform_socket {
    eventq_sqe sqe;
    SOCKET fd;
    struct sockaddr recv_addr;
    socklen_t recv_addr_len;
    uint8_t buffer[1500];
} platform_socket;

bool eventq_initialize(eventq* queue) {
    *queue = kqueue();
    return *queue != -1;
}
void eventq_cleanup(eventq queue) {
    close(queue);
}
bool eventq_sqe_initialize(eventq queue, eventq_sqe* sqe) { return true; }
void eventq_sqe_cleanup(eventq queue, eventq_sqe* sqe) { }
bool eventq_socket_create(eventq queue, platform_socket* sock) {
    return (sock->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) != (SOCKET)-1;
}
bool eventq_socket_receive_start(eventq queue, platform_socket* sock) {
    sock->sqe.type = PLATFORM_EVENT_TYPE_SOCKET_RECEIVE;
    struct kevent event = {0};
    EV_SET(&event, sock->fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, (void*)sock);
    return 0 <= kevent(queue, &event, 1, NULL, 0, NULL);
}
void eventq_socket_receive_complete(eventq_cqe* cqe) {
    platform_socket* sock = (platform_socket*)cqe->udata;
    int result = recvfrom(sock->fd, sock->buffer, sizeof(sock->buffer), 0, &sock->recv_addr, &sock->recv_addr_len);
    printf("Receive complete, %d bytes\n", result);
}
bool eventq_socket_send_start(eventq queue, platform_socket* sock) {
    printf("Sending %u bytes\n", (uint32_t)sizeof(sock->buffer)-10);
    return send(sock->fd, sock->buffer, sizeof(sock->buffer)-10, 0) != -1;
}
void eventq_socket_send_complete(eventq_cqe* cqe) {
}
void eventq_enqueue(eventq queue, eventq_sqe* sqe, uint32_t type, void* user_data, uint32_t status) {
    struct kevent event = {0};
    sqe->type = type;
    sqe->user_data = user_data;
    sqe->status = status;
    EV_SET(&event, queue, EVFILT_USER, EV_ADD | EV_CLEAR, NOTE_TRIGGER, 0, sqe);
    kevent(queue, &event, 1, NULL, 0, NULL);
}
#define CXPLAT_NANOSEC_PER_MS       (1000000)
#define CXPLAT_MS_PER_SECOND        (1000)
uint32_t eventq_dequeue(eventq queue, eventq_cqe* events, uint32_t count, uint32_t wait_time) {
    struct timespec timeout = {0, 0};
    if (wait_time != UINT32_MAX) {
        timeout.tv_sec += (wait_time / CXPLAT_MS_PER_SECOND);
        timeout.tv_nsec += ((wait_time % CXPLAT_MS_PER_SECOND) * CXPLAT_NANOSEC_PER_MS);
    }
    int result;
    do {
        result = kevent(queue, NULL, 0, events, count, wait_time == UINT32_MAX ? NULL : &timeout);
    } while ((result == -1L) && (errno == EINTR));
    return (uint32_t)result;
}
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
    eventq_sqe sqe;
    SOCKET fd;
    struct sockaddr recv_addr;
    int recv_addr_len;
    uint8_t buffer[1500];
} platform_socket;

bool eventq_initialize(eventq* queue) {
    *queue = epoll_create1(EPOLL_CLOEXEC);
    return *queue != -1;
}
void eventq_cleanup(eventq queue) {
    close(queue);
}
bool eventq_sqe_initialize(eventq queue, eventq_sqe* sqe) {
    sqe->fd = eventfd(0, EFD_CLOEXEC);
    if (sqe->fd == -1) return false;
    struct epoll_event event = { .events = EPOLLIN | EPOLLET, .data = { .ptr = sqe } };
    if (epoll_ctl(queue, EPOLL_CTL_ADD, sqe->fd, &event) != 0) {
        close(sqe->fd);
        return false;
    }
    return true;
}
void eventq_sqe_cleanup(eventq queue, eventq_sqe* sqe) {
    epoll_ctl(queue, EPOLL_CTL_DEL, sqe->fd, NULL);
    close(sqe->fd);
}
bool eventq_socket_create(eventq queue, platform_socket* sock) {
    return (sock->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) != (SOCKET)-1;
}
bool eventq_socket_receive_start(eventq queue, platform_socket* sock) {
    sock->sqe.type = PLATFORM_EVENT_TYPE_SOCKET_RECEIVE;
    struct epoll_event event = { .events = EPOLLIN | EPOLLET, .data = { .ptr = sock } };
    return 0 == epoll_ctl(queue, EPOLL_CTL_ADD, sock->fd, &event);
}
void eventq_socket_receive_complete(eventq_cqe* cqe) {
    platform_socket* sock = (platform_socket*)cqe->data.ptr;
    int result = recvfrom(sock->fd, sock->buffer, sizeof(sock->buffer), 0, &sock->recv_addr, &sock->recv_addr_len);
    printf("Receive complete, %d bytes\n", result);
}
bool eventq_socket_send_start(eventq queue, platform_socket* sock) {
    printf("Sending %u bytes\n", (uint32_t)sizeof(sock->buffer)-10);
    return send(sock->fd, sock->buffer, sizeof(sock->buffer)-10, 0) != -1;
}
void eventq_socket_send_complete(eventq_cqe* cqe) {
}
void eventq_enqueue(eventq queue, eventq_sqe* sqe, uint32_t type, void* user_data, uint32_t status) {
    sqe->type = type;
    sqe->user_data = user_data;
    sqe->status = status;
    eventfd_write(sqe->fd, 1);
}
uint32_t eventq_dequeue(eventq queue, eventq_cqe* events, uint32_t count, uint32_t wait_time) {
    const int timeout = wait_time == UINT32_MAX ? -1 : (int)wait_time;
    int result;
    do {
        result = epoll_wait(queue, events, count, timeout);
    } while ((result == -1L) && (errno == EINTR));
    return (uint32_t)result;
}
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
    eventq_sqe;
    SOCKET fd;
    struct sockaddr recv_addr;
    int recv_addr_len;
    uint8_t buffer[1500];
} platform_socket;

bool eventq_initialize(eventq* queue) {
    io_uring_queue_init(4, queue, 0); // TODO - Can this fail?
    return true;
}
void eventq_cleanup(eventq queue) {
    // TODO - How to cleanup?
}
bool eventq_sqe_initialize(eventq queue, eventq_sqe* sqe) { }
void eventq_sqe_cleanup(eventq queue, eventq_sqe* sqe) { }
bool eventq_socket_create(eventq queue, platform_socket* sock) {
    return (sock->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) != (SOCKET)-1;
}
void eventq_enqueue(eventq queue, eventq_sqe* sqe, uint32_t type, void* user_data, uint32_t status) {
    sqe->type = type;
    sqe->user_data = user_data;
    sqe->status = status;
    struct io_uring_sqe *io_sqe = io_uring_get_sqe(queue);
    io_uring_prep_nop(io_sqe); // TODO - Check args
    io_uring_sqe_set_data(io_sqe, sqe);
    io_uring_submit(queue); // TODO - Extract to separate function?
}
uint32_t eventq_dequeue(eventq queue, eventq_cqe* events, uint32_t count, uint32_t wait_time) {
    io_uring_wait_cqe(queue, events); // TODO - multiple return and wait_time
    return 1;
}
uint32_t eventq_cqe_get_type(eventq_cqe* cqe) { return ((eventq_sqe*)cqe->user_data)->type; }
void* eventq_cqe_get_user_data(eventq_cqe* cqe) { return ((eventq_sqe*)cqe->user_data)->user_data; }
uint32_t eventq_cqe_get_status(eventq_cqe* cqe) { return ((eventq_sqe*)cqe->user_data)->status; }

#else

#error "Must define an execution context"

#endif

//
// Minimal abstraction layer for the "platform" that represents all execution
// logic underneath the application.
//

#define APP_EVENT_TYPE_START 0x8000

uint32_t platform_wait_time = UINT32_MAX;
uint32_t platform_get_wait_time(void) { return platform_wait_time; }
void platform_process_timeout(void) { printf("Timeout\n"); platform_wait_time = UINT32_MAX; }
void platform_process_event(eventq_cqe* cqe) {
    switch (eventq_cqe_get_type(cqe)) {
    case PLATFORM_EVENT_TYPE_SOCKET_RECEIVE:
        eventq_socket_receive_complete(cqe);
        break;
    case PLATFORM_EVENT_TYPE_SOCKET_SEND:
        eventq_socket_send_complete(cqe);
        break;
    default:
        printf("Unexpected Event %u\n", eventq_cqe_get_type(cqe));
        break;
    }
}
void platform_sleep(uint32_t milliseconds) {
#ifdef _WIN32
    Sleep(milliseconds);
#else
    usleep(milliseconds * 1000);
#endif
}

bool platform_socket_create_listener(platform_socket* sock, eventq queue) {
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

bool platform_socket_create_client(platform_socket* sock, eventq queue) {
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
    if (!eventq_socket_send_start(queue, sock)) {
        platform_socket_close(sock->fd);
        return false;
    }
    return true;
}
