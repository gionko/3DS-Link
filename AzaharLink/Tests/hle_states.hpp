// The states a console can be in and the commands the tests throw at it, shared by the tests that
// drive every command in every state through UdsHle (result_closure_test: the result must be one of
// the firmware's; command_matrix_test: the result must be exactly the firmware's).
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <thread>

#include "hle_harness.hpp"

// ---------------------------------------------------------------------------------------------
// One console in one state, with the room around it
// ---------------------------------------------------------------------------------------------
enum State { S0, S3, S6, S7, S9, S10, SV1, SV2 };
static const char* const kStateNames[] = {"S0", "S3", "S6", "S7", "S9", "S10", "SV1", "SV2"};

struct Env {
    Env() : buffer(0x4000), out(0x600) {
        own = MakeNetworkInfo(4, 6);
        net = MakeNetworkInfo(4, 0);
        net.host_mac_address = Mac{0x00, 0x1F, 0x32, 0x00, 0x00, 0x77}; // nobody there
        net.channel = 11;
        ds.reset(new HleConsole(room, 3));
        ds->kernel.fast_sleep = true;
        uds = ds->Open("nwm::UDS");
    }
    ~Env() {
        Release();
        ds->Close(uds);
    }

    void Enter(State s) {
        if (s == S0) {
            return;
        }
        Reply r = ds->Call(uds, req::Initialize(MakeNode(0xABCD, "Closure", 0), 0x14000, 0x400));
        CHECK_EQ(r.result(), 0u);
        switch (s) {
        case S3:
            break;
        case S6:
            r = ds->Call(uds, req::BeginHosting(&own));
            CHECK_EQ(r.result(), 0u);
            break;
        case S7: {
            ds->kernel.connect_mode = CountingKernel::kHold;
            connecting = std::thread([this] { connect_result = ds->Call(uds, req::Connect(1, &net)).result(); });
            for (int i = 0; i < 2000 && ds->kernel.held.load() == 0; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            CHECK(ds->console.engine->status() == NetworkStatus::Connecting);
            return; // the gate stays shut until Release()
        }
        case S9:
        case S10: {
            host.reset(new AzaharHost(room, 1, own, MakeNode(0x1111, "Host", 0)));
            net = host->network();
            r = ds->Call(uds, req::Connect(s == S9 ? 1 : 2, &net));
            CHECK_EQ(r.result(), 0u);
            room.WaitIdle();
            CHECK(ds->console.engine->status() ==
                  (s == S9 ? NetworkStatus::ConnectedAsClient : NetworkStatus::ConnectedAsSpectator));
            break;
        }
        case SV1:
        case SV2:
            ds->console.engine->StopService(s == SV1 ? kServiceWirelessOff : kServiceSleep);
            break;
        default:
            break;
        }
        ds->kernel.connect_mode = CountingKernel::kFail; // a Connect from now on times out at once
    }
    // Lets a held ConnectToNetwork go (it fails: nobody answered) and waits for its thread.
    void Release() {
        ds->kernel.connect_mode = CountingKernel::kFail;
        if (connecting.joinable()) {
            connecting.join();
        }
    }

    /// The node id the console has in the network it is in (7 when it has none).
    u16 OwnNodeId() {
        const u16 id = ds->console.engine->GetConnectionStatus().network_node_id;
        return id ? id : 7;
    }

    MockRoom room;
    std::unique_ptr<AzaharHost> host;
    std::unique_ptr<HleConsole> ds;
    Handle uds = 0;
    std::thread connecting;
    std::atomic<u32> connect_result{0};
    NetworkInfo own;
    NetworkInfo net;
    std::vector<u8> buffer; // scan results
    std::vector<u8> out;    // output static buffer
    std::vector<u8> data = std::vector<u8>(0x700, 0x5A);
};

// ---------------------------------------------------------------------------------------------
// The commands, in the argument variants that matter for their result
// ---------------------------------------------------------------------------------------------
struct Variant {
    u32 id;
    const char* name;
    std::function<void(Env&)> prep;                    // runs first (not checked)
    std::function<std::vector<u32>(Env&)> request;
};

static std::vector<Variant> Variants() {
    auto bind = [](Env& e) { e.ds->Call(e.uds, req::Bind(2, 0x5F4, 1, 0xFFFF)); };
    std::vector<Variant> v;
    auto add = [&](u32 id, const char* name, std::function<std::vector<u32>(Env&)> request,
                   std::function<void(Env&)> prep = nullptr) {
        v.push_back({id, name, std::move(prep), std::move(request)});
    };
    const NodeInfo node = MakeNode(0xABCD, "Closure", 0);

    add(0x01, "ok", [node](Env&) {
        std::vector<u32> q = req::Make(0x01, 11, 2);
        q[1] = 0x14000;
        node.Store(reinterpret_cast<u8*>(&q[2]));
        return q;
    });
    add(0x1B, "ok", [node](Env&) { return req::Initialize(node, 0x14000, 0x400); });
    add(0x1B, "unaligned-memory", [node](Env&) { return req::Initialize(node, 0x14001, 0x400); });
    add(0x02, "ok", [](Env&) { return req::Make(0x02, 0, 0); });
    add(0x03, "ok", [](Env&) { return req::Shutdown(); });
    add(0x04, "ok", [](Env& e) {
        std::vector<u32> q = req::Make(0x04, 16, 2);
        std::memcpy(&q[1], &e.own, 0x3C);
        return q;
    });
    add(0x1D, "ok", [](Env& e) { return req::BeginHosting(&e.own); });
    add(0x1D, "one-node", [](Env& e) {
        e.own.max_nodes = 1;
        return req::BeginHosting(&e.own);
    });
    add(0x1D, "appdata-201", [](Env& e) {
        e.own.application_data_size = 201;
        return req::BeginHosting(&e.own);
    });
    add(0x05, "node-3", [](Env&) { return req::Eject(3); });
    add(0x05, "node-1", [](Env&) { return req::Eject(1); });
    add(0x05, "all", [](Env&) { return req::Eject(0xFFFF); });
    add(0x06, "ok", [](Env&) { return req::Make(0x06, 0, 0); });
    add(0x07, "block-new", [](Env&) { return req::Words(0x07, 2, 1, 1); });
    add(0x08, "ok", [](Env&) { return req::Destroy(); });
    add(0x09, "ok", [](Env& e) {
        std::vector<u32> q = req::Make(0x09, 17, 2);
        std::memcpy(&q[1], &e.net, 0x3C);
        q[16] = 1;
        return q;
    });
    add(0x1E, "client", [](Env& e) { return req::Connect(1, &e.net); });
    add(0x1E, "spectator", [](Env& e) { return req::Connect(2, &e.net); });
    add(0x1E, "bad-type", [](Env& e) { return req::Connect(3, &e.net); });
    add(0x0A, "ok", [](Env&) { return req::Disconnect(); });
    add(0x0B, "ok", [](Env&) { return req::GetConnectionStatus(); });
    add(0x0C, "ok", [](Env&) { return req::Make(0x0C, 0, 0); });
    add(0x0D, "node-1", [](Env&) { return req::GetNodeInformation(1); });
    add(0x0D, "node-9", [](Env&) { return req::GetNodeInformation(9); });
    add(0x0D, "node-0", [](Env&) { return req::GetNodeInformation(0); }); // (nwm's empty slot: found, as zeros, in every state)
    add(0x0E, "garbage", [](Env& e) {
        std::vector<u32> q = req::DecryptBeacon(&e.net, e.data.data(), 0xFE, e.data.data(), 0xFE);
        q[0] = IPC_MakeHeader(0x0E, 0, 6);
        return q;
    });
    add(0x1F, "garbage", [](Env& e) { return req::DecryptBeacon(&e.net, e.data.data(), 0xFE, e.data.data(), 0xFE); });
    add(0x0F, "scan", [](Env& e) {
        return req::Scan(e.buffer.data(), static_cast<u32>(e.buffer.size()), 0x0016ED10, 1, e.ds->kernel.CreateEvent());
    });
    add(0x10, "10-bytes", [](Env& e) { return req::SetApplicationData(e.data.data(), 10); });
    add(0x10, "201-bytes", [](Env& e) { return req::SetApplicationData(e.data.data(), 201); });
    add(0x10, "null-0", [](Env&) { return req::SetApplicationData(nullptr, 0); });   // (the firmware never sees the pointer: size 0 clears the data)
    add(0x11, "ok", [](Env&) { return req::GetApplicationData(0xC9); });
    add(0x12, "ok", [](Env&) { return req::Bind(2, 0x2E30, 1, 0xFFFF); });
    add(0x12, "id-0", [](Env&) { return req::Bind(0, 0x2E30, 1, 0xFFFF); });
    add(0x12, "channel-0", [](Env&) { return req::Bind(2, 0x2E30, 0, 0xFFFF); });
    add(0x12, "small-buffer", [](Env&) { return req::Bind(2, 0x10, 1, 0xFFFF); });
    add(0x13, "bound", [](Env&) { return req::Unbind(2); }, bind);
    add(0x13, "unknown", [](Env&) { return req::Unbind(9); });
    add(0x13, "id-0", [](Env&) { return req::Unbind(0); });
    add(0x14, "bound-empty", [](Env&) { return req::Pull(2); }, bind);
    add(0x14, "unknown", [](Env&) { return req::Pull(9); });
    add(0x14, "id-0", [](Env&) { return req::Pull(0); });
    add(0x15, "10ms", [](Env&) { return req::Words(0x15, 2, 10000000, 0); });
    add(0x15, "1ms", [](Env&) { return req::Words(0x15, 2, 1000000, 0); });
    add(0x16, "profile-0", [](Env&) { return req::Words(0x16, 1, 0); });
    add(0x16, "profile-3", [](Env&) { return req::Words(0x16, 1, 3); });
    add(0x17, "to-host", [](Env& e) { return req::SendTo(1, 1, 0, e.data.data(), 20); });
    add(0x17, "to-self", [](Env& e) { return req::SendTo(e.OwnNodeId(), 1, 0, e.data.data(), 20); });
    add(0x17, "to-zero", [](Env& e) { return req::SendTo(0, 1, 0, e.data.data(), 20); });
    add(0x17, "to-absent", [](Env& e) { return req::SendTo(9, 1, 0, e.data.data(), 20); });
    add(0x17, "oversize", [](Env& e) { return req::SendTo(1, 1, 0, e.data.data(), 0x5C7); });
    add(0x18, "1500", [](Env&) { return req::Words(0x18, 1, 1500); });
    add(0x18, "0x20000", [](Env&) { return req::Words(0x18, 1, 0x20000); });
    add(0x19, "10", [](Env&) { return req::Words(0x19, 1, 10); });
    add(0x19, "0x20000", [](Env&) { return req::Words(0x19, 1, 0x20000); });
    add(0x1A, "ok", [](Env&) { return req::Make(0x1A, 0, 0); });
    add(0x1C, "ok", [](Env&) { return req::Words(0x1C, 1, 1); });
    add(0x20, "aggregator-0", [](Env&) { return req::Words(0x20, 1, 0); });
    add(0x21, "ok", [](Env&) { return req::Words(0x21, 2, 0x00210080, 0); });
    add(0x22, "ok", [](Env& e) {
        return req::ScanOnConnection(e.buffer.data(), static_cast<u32>(e.buffer.size()), 0x0016ED10, 1);
    });
    add(0x23, "ok", [](Env&) { return req::Make(0x23, 0, 0); });
    add(0x00, "id-0", [](Env&) { return std::vector<u32>{0x00000000}; });
    add(0x24, "id-0x24", [](Env&) { return req::Make(0x24, 0, 0); });
    return v;
}

// ---------------------------------------------------------------------------------------------
// One cell: a fresh console in `state`, the variant sent to it, the reply
// ---------------------------------------------------------------------------------------------
inline std::string CellName(const char* kind, State state, const Variant& variant) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s %s 0x%02lx %s", kind, kStateNames[state], static_cast<unsigned long>(variant.id),
                  variant.name);
    return buf;
}

/// `check` sees the console after the command ran (the held Connect of S7 is still waiting).
inline Reply RunCell(Env& env, State state, const Variant& variant,
                     const std::function<void(Env&, const Reply&)>& check = nullptr) {
    env.Enter(state);
    if (variant.prep) {
        variant.prep(env);
    }
    const std::vector<u32> request = variant.request(env);
    Reply r;
    if (state == S7 && (variant.id == 0x09 || variant.id == 0x1E)) {
        // A second Connect waits like the first one: run it aside, and cut both short once
        // they are both in the wait (a build that refuses it at once never gets there).
        std::atomic<bool> done{false};
        std::thread second([&] {
            r = env.ds->Call(env.uds, request, env.out.data(), static_cast<u32>(env.out.size()));
            done = true;
        });
        for (int i = 0; i < 1000 && !done.load() && env.ds->kernel.held.load() < 2; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        env.Release();
        second.join();
    } else {
        r = env.ds->Call(env.uds, request, env.out.data(), static_cast<u32>(env.out.size()));
    }
    if (check) {
        check(env, r);
    }
    env.Release();
    return r;
}

inline const std::vector<State>& AllStates() {
    static const std::vector<State> states = {S0, S3, S6, S7, S9, S10, SV1, SV2};
    return states;
}
