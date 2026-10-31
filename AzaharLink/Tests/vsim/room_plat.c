/* room_plat.c: the platform layer of ENet 1.3.18 as the ROOM sees it in the model: a clock in microseconds that the driver moves, and a
   socket that is the end of the virtual wire. Compiled against the UPSTREAM headers; every enet_* symbol of it (like those of the rest
   of the room's ENet) is renamed r_enet_* by objcopy (Tests/vsim/build.sh) so that the plugin's own ENet can live in the same program. */
#define ENET_BUILDING_LIB 1
#include <stdio.h>
#include <string.h>
#include <enet/enet.h>

#include "vsim.h"

static uint64_t room_now_us = 1000000;
void room_set_now(uint64_t t) { room_now_us = t; }

int enet_initialize(void) { return 0; }
void enet_deinitialize(void) {}
enet_uint32 enet_host_random_seed(void) { return 12345u; }
enet_uint32 enet_time_get(void) { return (enet_uint32)(room_now_us / 1000ull); }
void enet_time_set(enet_uint32 t) { (void)t; }

int enet_address_set_host_ip(ENetAddress* address, const char* name) {
    unsigned a, b, c, d;
    if (sscanf(name, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return -1;
    address->host = (enet_uint32)(a | (b << 8) | (c << 16) | (d << 24));
    return 0;
}
int enet_address_set_host(ENetAddress* address, const char* name) { return enet_address_set_host_ip(address, name); }
int enet_address_get_host_ip(const ENetAddress* address, char* name, size_t nameLength) { (void)address; (void)name; (void)nameLength; return -1; }
int enet_address_get_host(const ENetAddress* address, char* name, size_t nameLength) { (void)address; (void)name; (void)nameLength; return -1; }

int enet_socket_bind(ENetSocket socket, const ENetAddress* address) { (void)socket; (void)address; return 0; }
int enet_socket_get_address(ENetSocket socket, ENetAddress* address) { (void)socket; address->host = 0x0100000Au; address->port = 24872; return 0; }
int enet_socket_listen(ENetSocket socket, int backlog) { (void)socket; (void)backlog; return 0; }
ENetSocket enet_socket_create(ENetSocketType type) { (void)type; return 1002; }
int enet_socket_set_option(ENetSocket socket, ENetSocketOption option, int value) { (void)socket; (void)option; (void)value; return 0; }
int enet_socket_get_option(ENetSocket socket, ENetSocketOption option, int* value) { (void)socket; (void)option; *value = 0; return 0; }
int enet_socket_connect(ENetSocket socket, const ENetAddress* address) { (void)socket; (void)address; return 0; }
ENetSocket enet_socket_accept(ENetSocket socket, ENetAddress* address) { (void)socket; (void)address; return ENET_SOCKET_NULL; }
int enet_socket_shutdown(ENetSocket socket, ENetSocketShutdown how) { (void)socket; (void)how; return 0; }
void enet_socket_destroy(ENetSocket socket) { (void)socket; }

int enet_socket_send(ENetSocket socket, const ENetAddress* address, const ENetBuffer* buffers, size_t bufferCount) {
    unsigned char gather[2048];
    size_t total = 0;
    (void)socket; (void)address;
    for (size_t i = 0; i < bufferCount; ++i) {
        if (total + buffers[i].dataLength > sizeof gather) return -1;
        memcpy(gather + total, buffers[i].data, buffers[i].dataLength);
        total += buffers[i].dataLength;
    }
    return vnet_room_send(gather, total, room_now_us);
}

int enet_socket_receive(ENetSocket socket, ENetAddress* address, ENetBuffer* buffers, size_t bufferCount) {
    (void)socket; (void)bufferCount;
    const int n = vnet_room_recv((uint8_t*)buffers[0].data, buffers[0].dataLength, room_now_us);
    if (n > 0 && address) {
        address->host = 0x0200000Au; /* 10.0.0.2 */
        address->port = 40001;
    }
    return n;
}

int enet_socket_wait(ENetSocket socket, enet_uint32* condition, enet_uint32 timeout) {
    (void)socket; (void)timeout;
    *condition = ENET_SOCKET_WAIT_NONE; /* the room is stepped by the model's events, it never sleeps in a poll */
    return 0;
}
int enet_socketset_select(ENetSocket maxSocket, ENetSocketSet* readSet, ENetSocketSet* writeSet, enet_uint32 timeout) {
    (void)maxSocket; (void)readSet; (void)writeSet; (void)timeout;
    return -1;
}
