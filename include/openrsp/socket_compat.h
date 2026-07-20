/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef OPENRSP_SOCKET_COMPAT_H
#define OPENRSP_SOCKET_COMPAT_H

#include <stddef.h>
#include <stdint.h>
#include <limits.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

typedef intptr_t openrsp_socket_t;
typedef WSAPOLLFD openrsp_pollfd;
typedef ULONG openrsp_nfds_t;

#define OPENRSP_INVALID_SOCKET ((openrsp_socket_t)-1)
#define OPENRSP_POLL_READ POLLRDNORM
#define OPENRSP_POLL_HANGUP POLLHUP
#define OPENRSP_POLL_ERROR POLLERR
#define OPENRSP_POLL_INVALID POLLNVAL

static inline int openrsp_socket_startup(void)
{
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0 ? 0 : -1;
}

static inline void openrsp_socket_cleanup(void)
{
    (void)WSACleanup();
}

static inline int openrsp_socket_close(openrsp_socket_t descriptor)
{
    return closesocket((SOCKET)descriptor);
}

static inline int openrsp_socket_last_error(void)
{
    return WSAGetLastError();
}

static inline int openrsp_socket_interrupted(int error)
{
    return error == WSAEINTR;
}

static inline int openrsp_socket_would_block(int error)
{
    return error == WSAEWOULDBLOCK;
}

static inline int openrsp_socket_timed_out(int error)
{
    return error == WSAETIMEDOUT || error == WSAEWOULDBLOCK;
}

static inline ptrdiff_t openrsp_socket_read(openrsp_socket_t descriptor,
                                            void *buffer, size_t bytes)
{
    int amount = bytes > (size_t)INT_MAX ? INT_MAX : (int)bytes;
    return (ptrdiff_t)recv((SOCKET)descriptor, (char *)buffer, amount, 0);
}

static inline ptrdiff_t openrsp_socket_write(openrsp_socket_t descriptor,
                                             const void *buffer, size_t bytes)
{
    int amount = bytes > (size_t)INT_MAX ? INT_MAX : (int)bytes;
    return (ptrdiff_t)send((SOCKET)descriptor, (const char *)buffer, amount, 0);
}

static inline int openrsp_socket_set_blocking(openrsp_socket_t descriptor,
                                              int blocking)
{
    u_long mode = blocking ? 0ul : 1ul;
    return ioctlsocket((SOCKET)descriptor, FIONBIO, &mode) == 0 ? 0 : -1;
}

static inline int openrsp_socket_poll(openrsp_pollfd *fds,
                                      openrsp_nfds_t count, int timeout_ms)
{
    fd_set readable;
    fd_set exceptional;
    FD_ZERO(&readable);
    FD_ZERO(&exceptional);
    for (openrsp_nfds_t index = 0u; index < count; ++index) {
        fds[index].revents = 0;
        if ((fds[index].events & OPENRSP_POLL_READ) != 0)
            FD_SET(fds[index].fd, &readable);
        if ((fds[index].events & OPENRSP_POLL_ERROR) != 0)
            FD_SET(fds[index].fd, &exceptional);
    }
    struct timeval timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000
    };
    int ready = select(0, &readable, NULL, &exceptional,
                       timeout_ms < 0 ? NULL : &timeout);
    if (ready <= 0) return ready;
    int events = 0;
    for (openrsp_nfds_t index = 0u; index < count; ++index) {
        if (FD_ISSET(fds[index].fd, &readable)) {
            fds[index].revents |= OPENRSP_POLL_READ;
            ++events;
        }
        if (FD_ISSET(fds[index].fd, &exceptional)) {
            fds[index].revents |= OPENRSP_POLL_ERROR;
            ++events;
        }
    }
    return events;
}

#else

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

typedef int openrsp_socket_t;
typedef struct pollfd openrsp_pollfd;
typedef nfds_t openrsp_nfds_t;

#define OPENRSP_INVALID_SOCKET (-1)
#define OPENRSP_POLL_READ POLLIN
#define OPENRSP_POLL_HANGUP POLLHUP
#define OPENRSP_POLL_ERROR POLLERR
#define OPENRSP_POLL_INVALID POLLNVAL

static inline int openrsp_socket_startup(void) { return 0; }
static inline void openrsp_socket_cleanup(void) {}
static inline int openrsp_socket_close(openrsp_socket_t descriptor)
{
    return close(descriptor);
}
static inline int openrsp_socket_last_error(void) { return errno; }
static inline int openrsp_socket_interrupted(int error) { return error == EINTR; }
static inline int openrsp_socket_would_block(int error)
{
    return error == EAGAIN || error == EWOULDBLOCK;
}
static inline int openrsp_socket_timed_out(int error)
{
    return error == EAGAIN || error == EWOULDBLOCK;
}
static inline ptrdiff_t openrsp_socket_read(openrsp_socket_t descriptor,
                                            void *buffer, size_t bytes)
{
    return read(descriptor, buffer, bytes);
}
static inline ptrdiff_t openrsp_socket_write(openrsp_socket_t descriptor,
                                             const void *buffer, size_t bytes)
{
#if defined(MSG_NOSIGNAL)
    return send(descriptor, buffer, bytes, MSG_NOSIGNAL);
#else
    return write(descriptor, buffer, bytes);
#endif
}
static inline int openrsp_socket_set_blocking(openrsp_socket_t descriptor,
                                              int blocking)
{
    int flags = fcntl(descriptor, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(descriptor, F_SETFL,
                 blocking ? flags & ~O_NONBLOCK : flags | O_NONBLOCK);
}
static inline int openrsp_socket_poll(openrsp_pollfd *fds,
                                      openrsp_nfds_t count, int timeout_ms)
{
    return poll(fds, count, timeout_ms);
}

#endif

#endif
