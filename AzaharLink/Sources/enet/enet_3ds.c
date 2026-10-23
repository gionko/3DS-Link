/**
 @file  enet_3ds.c
 @brief ENet platform layer for the Nintendo 3DS (libctru BSD sockets).

 Derived from ENet's unix.c (MIT, Lee Salzman). libctru has no sendmsg/recvmsg, so the scatter
 /gather I/O is emulated with sendto/recvfrom; waiting uses poll().
*/
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <errno.h>

#include <3ds.h>

#include "azl/rcvbuf.h"
#include "azl/ticks.h"

#define ENET_BUILDING_LIB 1
#include "enet/enet.h"

#ifndef SOMAXCONN
#define SOMAXCONN 4 /* libctru does not define it; listen() is unused by a UDP client anyway */
#endif

static enet_uint32 timeBase = 0;

/* Set when poll() timed out: nothing was readable at that moment, so the recvfrom() that ENet issues
   first thing in the next enet_host_service() would only return EWOULDBLOCK -- a full round trip to the
   soc service for nothing (about a third of all the network thread's soc calls). Skipping it loses
   nothing: a datagram that arrived meanwhile makes the poll() that follows return at once. It is a
   one-shot flag, and ANY send clears it: a sendto() takes milliseconds, so what the poll() saw is old. */
static int pollFoundNothing = 0;

/* Every soc call is timed (two clock reads, a microsecond or so each against the hundreds a call takes): the time the
   thread spent inside a call is time it waited for the soc process, which is not CPU, and the CPU of the loop is what is
   left of it. (A sample of one call in four, scaled up, put the ENet share at 0 in 98% of the windows of PROVA 2.) */

/* System tick of the last datagram read: the gaps between them show the room relay's holds. */
static unsigned long long lastDatagramTick = 0;

/* Set to 0 (ini key "socket_buffers=0") to leave the soc service's default socket buffers alone. */
int azl_enet_socket_buffers = 1;

/* The SO_RCVBUF to ask for: 64 KB, lowered by soc.cpp to the ring the socket block holds and by the ini
   key rcvbuf_kb. Whatever it holds, azl_rcvbuf_limit() caps it at 64 KB when it is used: more makes the
   soc service fail (ENOMEM) or, above 128 KB, corrupt its own memory and crash (see azl/rcvbuf.h). */
int azl_enet_rcvbuf_bytes = AZL_RCVBUF_MAX;

void azl_c_log (const char * fmt, ...) __attribute__ ((format (printf, 1, 2)));
void azl_c_soc_call (int kind, unsigned long long ticks); /* kinds: 0 recv with data, 1 recv empty, 2 send, 3 poll */
void azl_c_room_datagram (unsigned bytes, unsigned long long gapTicks);
void azl_c_rcvbuf_result (int asked, int got);

/* Monotonic milliseconds from the ARM11 system tick. */
static enet_uint32
enet_3ds_now_ms (void)
{
    return (enet_uint32) azl_ticks_to_ms (svcGetSystemTick ());
}

int
enet_initialize (void)
{
    return 0;
}

void
enet_deinitialize (void)
{
}

enet_uint32
enet_host_random_seed (void)
{
    return (enet_uint32) svcGetSystemTick ();
}

enet_uint32
enet_time_get (void)
{
    return enet_3ds_now_ms () - timeBase;
}

void
enet_time_set (enet_uint32 newTimeBase)
{
    timeBase = enet_3ds_now_ms () - newTimeBase;
}

int
enet_address_set_host_ip (ENetAddress * address, const char * name)
{
    if (! inet_pton (AF_INET, name, & address -> host))
        return -1;

    return 0;
}

int
enet_address_set_host (ENetAddress * address, const char * name)
{
    struct addrinfo hints, * resultList = NULL, * result = NULL;

    /* A dotted quad needs no resolver. */
    if (enet_address_set_host_ip (address, name) == 0)
        return 0;

    memset (& hints, 0, sizeof (hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo (name, NULL, & hints, & resultList) != 0)
        return -1;

    for (result = resultList; result != NULL; result = result -> ai_next)
    {
        if (result -> ai_family == AF_INET && result -> ai_addr != NULL &&
            result -> ai_addrlen >= sizeof (struct sockaddr_in))
        {
            struct sockaddr_in * sin = (struct sockaddr_in *) result -> ai_addr;

            address -> host = sin -> sin_addr.s_addr;

            freeaddrinfo (resultList);

            return 0;
        }
    }

    if (resultList != NULL)
        freeaddrinfo (resultList);

    return -1;
}

int
enet_address_get_host_ip (const ENetAddress * address, char * name, size_t nameLength)
{
    if (inet_ntop (AF_INET, & address -> host, name, nameLength) == NULL)
        return -1;
    return 0;
}

int
enet_address_get_host (const ENetAddress * address, char * name, size_t nameLength)
{
    /* No reverse lookups on the 3DS: report the numeric address. */
    return enet_address_get_host_ip (address, name, nameLength);
}

int
enet_socket_bind (ENetSocket socket, const ENetAddress * address)
{
    struct sockaddr_in sin;

    memset (& sin, 0, sizeof (struct sockaddr_in));

    sin.sin_family = AF_INET;

    if (address != NULL)
    {
       sin.sin_port = ENET_HOST_TO_NET_16 (address -> port);
       sin.sin_addr.s_addr = address -> host;
    }
    else
    {
       sin.sin_port = 0;
       sin.sin_addr.s_addr = INADDR_ANY;
    }

    return bind (socket, (struct sockaddr *) & sin, sizeof (struct sockaddr_in));
}

int
enet_socket_get_address (ENetSocket socket, ENetAddress * address)
{
    struct sockaddr_in sin;
    socklen_t sinLength = sizeof (struct sockaddr_in);

    if (getsockname (socket, (struct sockaddr *) & sin, & sinLength) == -1)
      return -1;

    address -> host = (enet_uint32) sin.sin_addr.s_addr;
    address -> port = ENET_NET_TO_HOST_16 (sin.sin_port);

    return 0;
}

int
enet_socket_listen (ENetSocket socket, int backlog)
{
    return listen (socket, backlog < 0 ? SOMAXCONN : backlog);
}

ENetSocket
enet_socket_create (ENetSocketType type)
{
    const ENetSocket created = socket (PF_INET, type == ENET_SOCKET_TYPE_DATAGRAM ? SOCK_DGRAM : SOCK_STREAM, 0);
    if (created < 0)
    {
        /* ENETDOWN 115 = network down; ENOMEM 12, ENOBUFS 105, EMFILE 24, EBUSY 16 = the soc service's
           pool or sessions (documented): worth telling apart in the log. */
        const int err = errno;
        azl_c_log ("soc: socket() failed, errno %d", err);
        errno = err;
    }
    return created;
}

/* The calls of the SO_RCVBUF negotiation (azl/rcvbuf.h) on a real socket. */
typedef struct
{
    ENetSocket socket;
    int before; /* the ring the socket had before (default), -1 if not read */
    int log;    /* log every attempt of this socket */
} AzlRcvbufSocket;

static int
azl_rcvbuf_set (void * ctx, int size, int * err)
{
    const int rc = setsockopt (((AzlRcvbufSocket *) ctx) -> socket, SOL_SOCKET, SO_RCVBUF, (char *) & size, sizeof (int));
    * err = rc == 0 ? 0 : errno;
    return rc;
}

static int
azl_rcvbuf_get (void * ctx)
{
    int got = -1;
    socklen_t len = sizeof (int);
    if (getsockopt (((AzlRcvbufSocket *) ctx) -> socket, SOL_SOCKET, SO_RCVBUF, (char *) & got, & len) != 0)
        return -1;
    return got;
}

static void
azl_rcvbuf_log (void * ctx, int candidate, int rc, int err, int got)
{
    const AzlRcvbufSocket * s = (const AzlRcvbufSocket *) ctx;
    if (s -> log)
        azl_c_log ("soc: SO_RCVBUF default %d, asked %d -> rc %d (errno %d), ring now %d",
                   s -> before, candidate, rc, err, got);
}

/* Asks the soc service for the receive ring (SO_RCVBUF) and reads back what it really gave. The request
   is capped at 64 KB whatever azl_enet_rcvbuf_bytes says. When the service answers ENOMEM and the ring
   really is smaller than the request, 32 KB and then 16 KB are tried; a refusal that left the ring as
   asked is not retried. The outcome goes to the plugin (azl_c_rcvbuf_result), which shows a shortfall on
   screen; the log has the first sockets and every change (nothing public documents what this service
   does with the option, and a reconnect loop makes a socket per attempt). */
static void
azl_set_rcvbuf (ENetSocket socket)
{
    static int sockets = 0, lastGot = -2;
    const int asked = azl_rcvbuf_limit (azl_enet_rcvbuf_bytes, 0);
    AzlRcvbufSocket s;
    azl_rcvbuf_io io;
    int got;

    if (asked <= 0)
        return;
    s.socket = socket;
    s.before = sockets < 4 ? azl_rcvbuf_get (& s) : -1;
    /* The first sockets and every change of the outcome are logged, not the thousandth reconnect. */
    s.log = sockets < 8;
    io.set = azl_rcvbuf_set;
    io.get = azl_rcvbuf_get;
    io.log = azl_rcvbuf_log;
    io.ctx = & s;
    got = azl_rcvbuf_negotiate (& io, asked);
    if (! s.log && got != lastGot)
        azl_c_log ("soc: SO_RCVBUF asked %d, ring now %d (it was %d on the earlier sockets)", asked, got, lastGot);
    ++ sockets;
    lastGot = got;
    azl_c_rcvbuf_result (asked, got);
}

int
enet_socket_set_option (ENetSocket socket, ENetSocketOption option, int value)
{
    int result = -1;
    switch (option)
    {
        case ENET_SOCKOPT_NONBLOCK:
            result = fcntl (socket, F_SETFL, (value ? O_NONBLOCK : 0) | (fcntl (socket, F_GETFL) & ~O_NONBLOCK));
            break;

        case ENET_SOCKOPT_BROADCAST:
            /* Not set: the soc service has no SO_BROADCAST for UDP (errno 109) and the room is a unicast
               server, so ENet's call on every new host would only be a failed round trip. */
            (void) value;
            result = 0;
            break;

        case ENET_SOCKOPT_REUSEADDR:
            result = setsockopt (socket, SOL_SOCKET, SO_REUSEADDR, (char *) & value, sizeof (int));
            break;

        case ENET_SOCKOPT_RCVBUF:
            /* ENet asks for 256 KB buffers, which crashes the soc process: see azl/rcvbuf.h. The receive
               side matters, though: a burst of relayed frames arriving while the network thread is busy
               would overflow the default ring of 4416 bytes. Failures are ignored: the default ring
               still works. */
            if (azl_enet_socket_buffers)
                azl_set_rcvbuf (socket);
            (void) value;
            result = 0;
            break;

        case ENET_SOCKOPT_SNDBUF:
            /* Never touched: the service already gives UDP sending 32 KB (documented) and refuses to
               change it (errno 109 = ENOPROTOOPT, a documented SetSockOpt error). */
            (void) value;
            result = 0;
            break;

        case ENET_SOCKOPT_NODELAY:
            result = setsockopt (socket, IPPROTO_TCP, TCP_NODELAY, (char *) & value, sizeof (int));
            break;

        default:
            /* RCVTIMEO / SNDTIMEO / TTL are not needed for a UDP client. */
            break;
    }
    return result == -1 ? -1 : 0;
}

int
enet_socket_get_option (ENetSocket socket, ENetSocketOption option, int * value)
{
    int result = -1;
    socklen_t len;
    switch (option)
    {
        case ENET_SOCKOPT_ERROR:
            len = sizeof (int);
            result = getsockopt (socket, SOL_SOCKET, SO_ERROR, value, & len);
            break;

        default:
            break;
    }
    return result == -1 ? -1 : 0;
}

int
enet_socket_connect (ENetSocket socket, const ENetAddress * address)
{
    struct sockaddr_in sin;
    int result;

    memset (& sin, 0, sizeof (struct sockaddr_in));

    sin.sin_family = AF_INET;
    sin.sin_port = ENET_HOST_TO_NET_16 (address -> port);
    sin.sin_addr.s_addr = address -> host;

    result = connect (socket, (struct sockaddr *) & sin, sizeof (struct sockaddr_in));
    if (result == -1 && errno == EINPROGRESS)
      return 0;

    return result;
}

ENetSocket
enet_socket_accept (ENetSocket socket, ENetAddress * address)
{
    int result;
    struct sockaddr_in sin;
    socklen_t sinLength = sizeof (struct sockaddr_in);

    result = accept (socket,
                     address != NULL ? (struct sockaddr *) & sin : NULL,
                     address != NULL ? & sinLength : NULL);

    if (result == -1)
      return ENET_SOCKET_NULL;

    if (address != NULL)
    {
        address -> host = (enet_uint32) sin.sin_addr.s_addr;
        address -> port = ENET_NET_TO_HOST_16 (sin.sin_port);
    }

    return result;
}

int
enet_socket_shutdown (ENetSocket socket, ENetSocketShutdown how)
{
    return shutdown (socket, (int) how);
}

void
enet_socket_destroy (ENetSocket socket)
{
    if (socket != -1)
      close (socket);
}

/* Largest datagram ENet ever builds (ENET_PROTOCOL_MAXIMUM_MTU). */
#define AZL_ENET_DGRAM_MAX 4096

int
enet_socket_send (ENetSocket socket,
                  const ENetAddress * address,
                  const ENetBuffer * buffers,
                  size_t bufferCount)
{
    struct sockaddr_in sin;
    unsigned char gather [AZL_ENET_DGRAM_MAX];
    size_t total = 0, i;
    int sentLength, savedErrno;
    unsigned long long startTick;

    for (i = 0; i < bufferCount; ++ i)
    {
        if (total + buffers [i].dataLength > sizeof (gather))
            return -1;
        memcpy (gather + total, buffers [i].data, buffers [i].dataLength);
        total += buffers [i].dataLength;
    }

    pollFoundNothing = 0; /* the send takes milliseconds: what the last poll() saw is old */
    startTick = svcGetSystemTick ();

    if (address != NULL)
    {
        memset (& sin, 0, sizeof (struct sockaddr_in));

        sin.sin_family = AF_INET;
        sin.sin_port = ENET_HOST_TO_NET_16 (address -> port);
        sin.sin_addr.s_addr = address -> host;

        sentLength = sendto (socket, gather, total, 0, (struct sockaddr *) & sin, sizeof (struct sockaddr_in));
    }
    else
        sentLength = send (socket, gather, total, 0);

    savedErrno = errno;
    azl_c_soc_call (2, svcGetSystemTick () - startTick);
    errno = savedErrno;

    if (sentLength == -1)
    {
       if (errno == EWOULDBLOCK)
         return 0;

       return -1;
    }

    return sentLength;
}

int
enet_socket_receive (ENetSocket socket,
                     ENetAddress * address,
                     ENetBuffer * buffers,
                     size_t bufferCount)
{
    struct sockaddr_in sin;
    socklen_t sinLength = sizeof (struct sockaddr_in);
    int recvLength, savedErrno;
    unsigned long long startTick, now;

    if (pollFoundNothing)
    {
        pollFoundNothing = 0;
        return 0;
    }

    startTick = svcGetSystemTick ();

    if (bufferCount == 1)
    {
        recvLength = recvfrom (socket, buffers [0].data, buffers [0].dataLength, 0,
                               (struct sockaddr *) & sin, & sinLength);
    }
    else
    {
        unsigned char scatter [AZL_ENET_DGRAM_MAX];
        size_t remaining, i, offset = 0;

        recvLength = recvfrom (socket, scatter, sizeof (scatter), 0,
                               (struct sockaddr *) & sin, & sinLength);
        if (recvLength > 0)
        {
            remaining = (size_t) recvLength;
            for (i = 0; i < bufferCount && remaining > 0; ++ i)
            {
                size_t chunk = remaining < buffers [i].dataLength ? remaining : buffers [i].dataLength;
                memcpy (buffers [i].data, scatter + offset, chunk);
                offset += chunk;
                remaining -= chunk;
            }
            if (remaining > 0)
                return -2;
        }
    }

    savedErrno = errno;
    now = svcGetSystemTick ();
    azl_c_soc_call (recvLength > 0 ? 0 : 1, now - startTick);
    if (recvLength > 0)
    {
        azl_c_room_datagram ((unsigned) recvLength, lastDatagramTick ? now - lastDatagramTick : 0);
        lastDatagramTick = now;
    }
    errno = savedErrno;

    if (recvLength == -1)
    {
        switch (errno)
        {
            case EWOULDBLOCK:
                return 0;
            case EINTR:
            case EMSGSIZE:
                return -2;
            default:
                return -1;
        }
    }

    if (address != NULL)
    {
        address -> host = (enet_uint32) sin.sin_addr.s_addr;
        address -> port = ENET_NET_TO_HOST_16 (sin.sin_port);
    }

    return recvLength;
}

int
enet_socketset_select (ENetSocket maxSocket, ENetSocketSet * readSet, ENetSocketSet * writeSet, enet_uint32 timeout)
{
    /* Only used by ENet's optional TCP helpers; the UDP client never calls it. */
    (void) maxSocket; (void) readSet; (void) writeSet; (void) timeout;
    return -1;
}

int
enet_socket_wait (ENetSocket socket, enet_uint32 * condition, enet_uint32 timeout)
{
    struct pollfd pollSocket;
    int pollCount;

    pollSocket.fd = socket;
    pollSocket.events = 0;
    pollSocket.revents = 0;

    if (* condition & ENET_SOCKET_WAIT_SEND)
      pollSocket.events |= POLLOUT;

    if (* condition & ENET_SOCKET_WAIT_RECEIVE)
      pollSocket.events |= POLLIN;

    {
        const unsigned long long startTick = svcGetSystemTick ();
        int savedErrno;
        pollCount = poll (& pollSocket, 1, (int) timeout);
        savedErrno = errno;
        azl_c_soc_call (3, svcGetSystemTick () - startTick);
        errno = savedErrno;
    }
    pollFoundNothing = pollCount == 0 && (pollSocket.events & POLLIN) != 0;

    if (pollCount < 0)
    {
        if (errno == EINTR && * condition & ENET_SOCKET_WAIT_INTERRUPT)
        {
            * condition = ENET_SOCKET_WAIT_INTERRUPT;

            return 0;
        }

        return -1;
    }

    * condition = ENET_SOCKET_WAIT_NONE;

    if (pollCount == 0)
      return 0;

    if (pollSocket.revents & POLLOUT)
      * condition |= ENET_SOCKET_WAIT_SEND;

    if (pollSocket.revents & POLLIN)
      * condition |= ENET_SOCKET_WAIT_RECEIVE;

    return 0;
}
