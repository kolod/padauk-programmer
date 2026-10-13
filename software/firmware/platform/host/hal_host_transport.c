/* SPDX-License-Identifier: GPL-3.0-or-later */
/* TCP transport for pdk-sim: one client at a time; "port open" (DTR) = a client is connected. */
#include "hal_transport.h"
#include "host.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close close
#endif

static sock_t listener = SOCK_BAD;
static sock_t client = SOCK_BAD;

static void set_nonblocking(sock_t s)
{
#ifdef _WIN32
    u_long one = 1;
    ioctlsocket(s, FIONBIO, &one);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

bool host_transport_listen(const char *host, uint16_t port)
{
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return false;
#endif
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == SOCK_BAD)
        return false;
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = inet_addr(host);
    if (bind(listener, (struct sockaddr *)&a, sizeof a) != 0 || listen(listener, 1) != 0)
        return false;
    set_nonblocking(listener);
    return true;
}

void host_transport_shutdown(void)
{
    if (client != SOCK_BAD)
        sock_close(client);
    if (listener != SOCK_BAD)
        sock_close(listener);
    client = listener = SOCK_BAD;
}

/* After a disconnect the port reads "closed" at least once before the next client is accepted, so
 * the core sees the session end even when the host reconnects at once (like DTR going low on USB). */
static bool dropped;

static void drop_client(void)
{
    if (client != SOCK_BAD) {
        sock_close(client);
        client = SOCK_BAD;
        dropped = true;
    }
}

static void poll_accept(void)
{
    if (client != SOCK_BAD || listener == SOCK_BAD || dropped)
        return;
    sock_t c = accept(listener, NULL, NULL);
    if (c == SOCK_BAD)
        return;
    int one = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    set_nonblocking(c);
    client = c;
}

bool hal_transport_is_open(void)
{
    if (dropped) {
        dropped = false;
        return false;
    }
    poll_accept();
    return client != SOCK_BAD;
}

size_t hal_transport_read(uint8_t *buf, size_t max)
{
    /* no accept here: a new client is taken only by hal_transport_is_open(), so the session layer
     * sees every open/close transition before the client's data */
    if (client == SOCK_BAD)
        return 0;
    int n = recv(client, (char *)buf, (int)max, 0);
    if (n > 0)
        return (size_t)n;
    if (n == 0) {
        drop_client();   /* peer closed */
        return 0;
    }
#ifdef _WIN32
    if (WSAGetLastError() != WSAEWOULDBLOCK)
        drop_client();
#else
    if (errno != EAGAIN && errno != EWOULDBLOCK)
        drop_client();
#endif
    return 0;
}

void hal_transport_write(const uint8_t *buf, size_t len)
{
    while (len && client != SOCK_BAD) {
        int n = send(client, (const char *)buf, (int)len, 0);
        if (n > 0) {
            buf += n;
            len -= (size_t)n;
            continue;
        }
#ifdef _WIN32
        if (WSAGetLastError() == WSAEWOULDBLOCK) {
            host_sleep_us(100);
            continue;
        }
#else
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            host_sleep_us(100);
            continue;
        }
#endif
        drop_client();
    }
}
