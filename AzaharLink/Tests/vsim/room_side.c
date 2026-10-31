/* room_side.c: what Azahar's room does with one console, on top of ENet 1.3.18 (room.cpp): it accepts the connection, hands every frame
   to the console as a RELIABLE packet on channel 0 and flushes after each one (HandleWifiPacket, enet_host_flush(server)), reads what
   the console sends and throws it away (the other members are not modelled), and services its host whenever something arrives or 16 ms
   have gone by (ServerLoop: enet_host_service(server, &event, 16)). Compiled against the UPSTREAM headers. */
#include <string.h>
#include <enet/enet.h>

#include "vsim.h"

static ENetHost* server;
static ENetPeer* console;
static RoomStats st;

void room_init(void) {
    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = 24872;
    server = enet_host_create(&address, 4, 1, 0, 0);
    console = NULL;
    memset(&st, 0, sizeof st);
}

int room_connected(void) { return console != NULL && console->state == ENET_PEER_STATE_CONNECTED; }

void room_inject(uint32_t id, uint32_t size) {
    uint8_t buf[1600];
    if (size < 8) size = 8;
    if (size > sizeof buf) size = sizeof buf;
    memset(buf, 0xA5, size);
    memcpy(buf, &id, 4);
    ENetPacket* p = enet_packet_create(buf, size, ENET_PACKET_FLAG_RELIABLE);
    if (room_connected()) {
        enet_peer_send(console, 0, p);
    } else {
        enet_packet_destroy(p);
        return;
    }
    enet_host_flush(server);
}

void room_service(void) {
    ENetEvent ev;
    int guard = 0;
    while (enet_host_service(server, &ev, 0) > 0 && ++guard < 100000) {
        switch (ev.type) {
        case ENET_EVENT_TYPE_CONNECT:
            console = ev.peer;
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            ++st.rx_packets;
            enet_packet_destroy(ev.packet);
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            ++st.disconnects;
            console = NULL;
            break;
        default:
            break;
        }
    }
    if (server->peerCount > 0) {
        const ENetPeer* p = &server->peers[0];
        if (p->state == ENET_PEER_STATE_CONNECTED) {
            const uint32_t rto = p->roundTripTime + 4 * p->roundTripTimeVariance;
            if (p->roundTripTimeVariance > st.rtt_var_max) st.rtt_var_max = p->roundTripTimeVariance;
            if (rto > st.rto_max) st.rto_max = rto;
        }
    }
}

void room_get_stats(RoomStats* out) {
    st.connected = room_connected();
    if (server && server->peerCount > 0) {
        const ENetPeer* p = &server->peers[0];
        st.packets_lost = p->packetsLost;
        st.rtt = p->roundTripTime;
        st.rtt_var = p->roundTripTimeVariance;
        st.rto = p->roundTripTime + 4 * p->roundTripTimeVariance;
    }
    if (server) {
        st.total_sent_dgrams = server->totalSentPackets;
        st.total_recv_dgrams = server->totalReceivedPackets;
    }
    *out = st;
}
