// Stack and size probe of the fake Download Play lobby (azl/dlp_types.hpp, dlp_lobby.hpp, dlp_ipc.hpp) on the plugin's own
// target. Design: analysis/dlp-lobby-fake-2026-10-28/DESIGN-FASE1.md sez. 0 and 1.3.
//
// THIS FILE IS NEVER RUN. Tests/dlp_stack_check.sh compiles it with arm-none-eabi-g++ and the flags of the Makefile
// (-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -Os -fomit-frame-pointer -fno-strict-aliasing -fno-rtti
// -fno-exceptions -std=gnu++17) plus -fstack-usage and -fcallgraph-info=su, then reads the frame of every function and the
// sum of the frames from every function down to its first call into the network (LobbyNet, or the game's address
// mapper). The code of the three headers runs on the game thread that made the IPC call, and some of those threads have 464
// bytes below it: the PC tests (x86-64, ASan) cannot see that, and a local array or a lost noinline would pass them all.
//
// WHAT IT HOLDS:
//  * the sizes on ARM32 (the plugin's ABI) as assertions: the memory of the module is a few hundred bytes and a growth is a
//    decision, not an accident (raise a number HERE, on purpose, together with the header's own assertion);
//  * one noinline wrapper for every public function of DlpLobby and DlpService. A wrapper is the function as the plugin
//    calls it from another translation unit, with whatever the compiler inlines into it; the functions that are noinline in
//    the headers also get a frame of their own, and -fkeep-inline-functions makes every function of the headers appear in the
//    listing even if no wrapper names it (a new public function is measured before it has a wrapper).
// The plugin's own translation unit decides the real inlining (phase 3 measures it again with Tests/stack_usage.sh).

#if !defined(__arm__)
#error "Tests/dlp_stack_probe.cpp measures the plugin's target: compile it with arm-none-eabi-g++ (Tests/dlp_stack_check.sh)"
#endif

#include <new>

#include "azl/dlp_ipc.hpp"

using namespace azl;

#define PROBE_NOINLINE __attribute__((noinline))

// ---- the sizes, ARM32 (measured with arm-none-eabi-g++ 16.1) -------------------------------------------------------------
static_assert(sizeof(void*) == 4, "the plugin is ARM32: these sizes mean nothing on another target");
static_assert(sizeof(DlpLobby) <= 400, "DlpLobby: 400 bytes (the whole searcher and creator, no heap; 272 while the tables held four players); raise this on purpose");
static_assert(sizeof(DlpService) <= 40, "DlpService: 40 bytes (a lobby pointer, six counters, a few flags); raise this on purpose");
static_assert(sizeof(DlpLobby::Tables) == 296, "the tables are one union, 296 bytes at most (one of three shapes: the session of eight players)");
static_assert(sizeof(Group) == 40, "a lobby heard: 40 bytes, no u64");
static_assert(sizeof(NodeEntry) == 32, "a player as the network reports it: 32 bytes");
static_assert(sizeof(HeardGroup) <= 40, "a lobby heard is a local of a game thread: at most 40 bytes");
static_assert(sizeof(HostAnnounce) == 28, "the announcement as a structure: 28 bytes");
static_assert(sizeof(Identity) == 32, "who we are in the virtual network: 32 bytes");
static_assert(sizeof(DlpLobby::Track) == 8, "a client of the creator: 8 bytes");

// ---- the public functions of DlpLobby, one frame each -------------------------------------------------------------------
PROBE_NOINLINE void p_Lobby_ctor(void* where, LobbyNet* net, const LobbyConfig& cfg) { ::new (where) DlpLobby(net, cfg); }
PROBE_NOINLINE u32 p_ClientInitialize(DlpLobby* l, const u16* name, u32 beacons) { return l->ClientInitialize(name, beacons); }
PROBE_NOINLINE u32 p_ClientFinalize(DlpLobby* l) { return l->ClientFinalize(); }
PROBE_NOINLINE u32 p_ClientStartScan(DlpLobby* l, u64 filter) { return l->ClientStartScan(filter); }
PROBE_NOINLINE u32 p_ClientStopScan(DlpLobby* l) { return l->ClientStopScan(); }
PROBE_NOINLINE u32 p_ClientTitleInfo(DlpLobby* l, const Mac& host, GroupView* out) { return l->ClientTitleInfo(host, out); }
PROBE_NOINLINE u32 p_ClientServerInfo(DlpLobby* l, const Mac& host, GroupView* out) { return l->ClientServerInfo(host, out); }
PROBE_NOINLINE u32 p_ClientTitleInfoInOrder(DlpLobby* l, bool restart, GroupView* out) { return l->ClientTitleInfoInOrder(restart, out); }
PROBE_NOINLINE u32 p_ClientDeleteScanInfo(DlpLobby* l, const Mac& host) { return l->ClientDeleteScanInfo(host); }
PROBE_NOINLINE u32 p_ClientJoinBegin(DlpLobby* l, const Mac& host, u32* result) { return static_cast<u32>(l->ClientJoinBegin(host, result)); }
PROBE_NOINLINE bool p_ClientJoinPoll(DlpLobby* l, u32* result) { return l->ClientJoinPoll(result); }
PROBE_NOINLINE u32 p_ClientStopSession(DlpLobby* l) { return l->ClientStopSession(); }
PROBE_NOINLINE u32 p_ClientStatusWord(const DlpLobby* l) { return l->ClientStatusWord(); }
PROBE_NOINLINE u32 p_ClientConnectingNodes(const DlpLobby* l, u8* ids, u32 capacity, u32* total) { return l->ClientConnectingNodes(ids, capacity, total); }
PROBE_NOINLINE u32 p_ClientNodeInfo(const DlpLobby* l, u16 node, NodeEntry* out) { return l->ClientNodeInfo(node, out); }
PROBE_NOINLINE u32 p_ClientPassphrase(const DlpLobby* l, u8* out) { return l->ClientPassphrase(out); }
PROBE_NOINLINE u32 p_ServerInitialize(DlpLobby* l, u32 max_clients, u32 child, const u16* name) { return l->ServerInitialize(max_clients, child, name); }
PROBE_NOINLINE u32 p_ServerFinalize(DlpLobby* l) { return l->ServerFinalize(); }
PROBE_NOINLINE u32 p_ServerStartAccepting(DlpLobby* l, bool manual, u32 channel) { return l->ServerStartAccepting(manual, channel); }
PROBE_NOINLINE u32 p_ServerEndAccepting(DlpLobby* l) { return l->ServerEndAccepting(); }
PROBE_NOINLINE u32 p_ServerState(const DlpLobby* l) { return l->ServerState(); }
PROBE_NOINLINE u32 p_ServerConnectingClients(const DlpLobby* l, u8* ids, u32 capacity, u32* total) { return l->ServerConnectingClients(ids, capacity, total); }
PROBE_NOINLINE u32 p_ServerClientInfo(DlpLobby* l, u16 node, NodeEntry* out) { return l->ServerClientInfo(node, out); }
PROBE_NOINLINE u32 p_ServerClientState(const DlpLobby* l, u16 node, u32* word) { return l->ServerClientState(node, word); }
PROBE_NOINLINE u32 p_ServerAcceptClient(DlpLobby* l, u16 node) { return l->ServerAcceptClient(node); }
PROBE_NOINLINE u32 p_ServerDisconnectClient(DlpLobby* l, u16 node) { return l->ServerDisconnectClient(node); }
PROBE_NOINLINE u32 p_ServerStartDistribution(DlpLobby* l) { return l->ServerStartDistribution(); }
PROBE_NOINLINE u32 p_ServerSendPassphrase(DlpLobby* l, const u8* nine) { return l->ServerSendPassphrase(nine); }
PROBE_NOINLINE bool p_OnGameUdsStarting(DlpLobby* l) { return l->OnGameUdsStarting(); }
PROBE_NOINLINE void p_Pump(DlpLobby* l) { l->Pump(); }
PROBE_NOINLINE void p_Poll(DlpLobby* l, bool force) { l->Poll(force); }
PROBE_NOINLINE void p_SetExternalPoll(DlpLobby* l, bool on) { l->SetExternalPoll(on); }
PROBE_NOINLINE void p_SetOnNet(DlpLobby* l, bool on) { l->SetOnNet(on); }
PROBE_NOINLINE void p_SetDefaultName(DlpLobby* l, const u16* name) { l->SetDefaultName(name); }
PROBE_NOINLINE bool p_active(const DlpLobby* l) { return l->active(); }
PROBE_NOINLINE LobbyNet* p_net(const DlpLobby* l) { return l->net(); }
PROBE_NOINLINE u32 p_bad_messages(const DlpLobby* l) { return l->bad_messages(); }
PROBE_NOINLINE u32 p_max_players(const DlpLobby* l) { return l->max_players(); }
PROBE_NOINLINE u32 p_client_state(const DlpLobby* l) { return l->client_state(); }
PROBE_NOINLINE u32 p_server_state(const DlpLobby* l) { return l->server_state(); }
PROBE_NOINLINE bool p_join_active(const DlpLobby* l) { return l->join_active(); }
// 1.1.4 (G1): the searcher's two functions of dlp:CLNT
PROBE_NOINLINE const Group* p_ClientScanEntry(const DlpLobby* l, u32 slot) { return l->ClientScanEntry(slot); }
PROBE_NOINLINE const Group* p_ClientGroupOf(const DlpLobby* l, const Mac& host) { return l->ClientGroupOf(host); }
PROBE_NOINLINE u32 p_ClientAdoptGroup(DlpLobby* l, const Group& g) { return l->ClientAdoptGroup(g); }

// ---- the public functions of DlpService, one frame each -----------------------------------------------------------------
PROBE_NOINLINE void p_Service_ctor(void* where) { ::new (where) DlpService(); }
PROBE_NOINLINE void p_SetLobby(DlpService* s, DlpLobby* lobby) { s->SetLobby(lobby); }
PROBE_NOINLINE void p_SetChild(DlpService* s, bool on) { s->SetChild(on); }  // (1.1.4, G4: the prepared child's IsChild)
PROBE_NOINLINE u32 p_mode(const DlpService* s) { return static_cast<u32>(s->mode()); }
PROBE_NOINLINE u32 p_Dispatch(DlpService* s, DlpKind kind, u32* cmd, const u32* sbuf, const IpcResolver& r) { return static_cast<u32>(s->Dispatch(kind, cmd, sbuf, r)); }
PROBE_NOINLINE bool p_PollPending(DlpService* s, u32* cmd) { return s->PollPending(cmd); }
PROBE_NOINLINE bool p_join_pending(const DlpService* s) { return s->join_pending(); }
PROBE_NOINLINE void p_OnSessionClosed(DlpService* s, DlpKind kind) { s->OnSessionClosed(kind); }
// 1.1.4 (G1): dlp:CLNT - its commands are p_Dispatch's, whose kind is a parameter: Dispatch's chains above hold DispatchClnt's too - and what the
// hub asks of it
PROBE_NOINLINE bool p_ClntUnserved(DlpService* s, u32* cmd) { return s->ClntUnserved(cmd); }
PROBE_NOINLINE void p_ClntApplyDeferred(DlpService* s) { s->ClntApplyDeferred(); }
PROBE_NOINLINE void p_ClntLeave(DlpService* s) { s->ClntLeave(); }

// ---- the book of dlp:CLNT (azl/dlp_clnt_book.hpp, 1.1.4 G1): what the Download Play application's thread runs of it, and the two writers -----------
static_assert(sizeof(DlpClntBook) <= 1400, "the book of dlp:CLNT: 16 records, 4 titles and a few words (the Download Play application only); raise this on purpose");
PROBE_NOINLINE bool p_book_Deliver(DlpClntBook* b, const Group& g) { return b->Deliver(g); }
PROBE_NOINLINE bool p_book_Delivered(const DlpClntBook* b, const Mac& host) { return b->DeliveredThisScan(host); }
PROBE_NOINLINE void p_book_Refresh(DlpClntBook* b, const Group& g) { b->Refresh(g); }
PROBE_NOINLINE const Group* p_book_Find(const DlpClntBook* b, const Mac& host) { return b->Find(host); }
PROBE_NOINLINE void p_book_NoteTitle(DlpClntBook* b, const Mac& host, u32 low, const u8* f, u32 n) { b->NoteTitle(host, low, f, n); }
PROBE_NOINLINE u32 p_book_VerdictOf(DlpClntBook* b, const Group& g) { return static_cast<u32>(b->VerdictOf(g)); }
PROBE_NOINLINE const u8* p_book_BlockFor(const DlpClntBook* b, const Group& g) { return b->BlockFor(g); }
PROBE_NOINLINE u64 p_book_PreparedFor(const DlpClntBook* b, const Group& g) { return b->PreparedFor(g); }
PROBE_NOINLINE u32 p_book_Shown(const DlpClntBook* b, u32 w) { return b->Shown(w); }
PROBE_NOINLINE bool p_book_Defer(DlpClntBook* b, u32 id, u32 beacons, u32* res) { return b->Defer(id, beacons, res); }
PROBE_NOINLINE void p_book_Reset(DlpClntBook* b) { b->Reset(); }
PROBE_NOINLINE void p_WriteClntTitleInfo(u8* dst, const Group& g, const DlpClntBook::Title* t, const u8* block, dlp_children::Verdict v) {
    WriteClntTitleInfo(dst, g, t, block, v);
}
PROBE_NOINLINE void p_WriteClntServerInfo(u8* dst, const Group& g, dlp_children::Verdict v, u64 tick) { WriteClntServerInfo(dst, g, v, tick); }

PROBE_NOINLINE u32 p_counters(const DlpService* s) {
    return s->commands() + s->initializes() + s->finalizes() + s->refused() + s->unknown_commands() + s->bad_pointer() + s->last_id() + s->last_result();
}

// ---- the hub (phase 3): Includes/azl/dlp_hub.hpp and the public functions of DlpHub ---------------------------------------------------------
// Compiled only with -DDLP_PROBE_HUB (Tests/dlp_stack_check.sh, pass H), which also gives the defines and the libctru headers of the plugin's own
// build: the hub's header includes the UDS engine's, and sizeof(DlpHub) is not the same with a std::mutex as with the 4-byte LightLock the plugin
// has (-D__3DS__). The hub is in the plugin only for the games in Lobby mode: it costs a heap block of this size and nothing else. The functions of
// the hub itself live in Sources/azl/dlp_hub.cpp, and what a game thread pays in them is held by Tests/dlp_net_stack_check.sh (the frame of every
// function of that unit) and Tests/stack_usage.sh (the chains of the plugin's own unit, Tests/dlp_chain_report.py); here the surface of the class is
// listed - a wrapper for every public function, so that a new one is looked at before it is called from a game thread - and its sizes are held.
// The private functions (Post, NetAlive, Stamp, AlwaysSucceeds, EndsRole, RunHere, RunDeferred ...) cannot be named here: they have a budget line each in
// Tests/stack_usage.sh (the unit's .su listing of the plugin's own build).
#if defined(DLP_PROBE_HUB)
#include "azl/dlp_hub.hpp"
#include "azl/dlp_real_net.hpp"

#define HUB_NOINLINE __attribute__((noinline))

static_assert(sizeof(DlpHub) <= 536, "DlpHub: 536 bytes (the lobby 400, the service 40, the adapter 12, the slot and the counters; 408 while the tables held four players); raise this on purpose");
static_assert(sizeof(EngineLobbyNet) <= 12, "the adapter: a vtable pointer, the engine's and a flag");
// 1.1.3: the adapter of the real Download Play (a heap block of its own, only for a game in RealDlp mode; the hub keeps no member for it)
static_assert(sizeof(RealDlpNet) <= 2048, "RealDlpNet: one frame buffer, the probes' cache and some words (1936 bytes measured 2026-11-16 with the book and the children of G1, the wait of H5 - 60 bytes - and the ring of H3; 1840 measured with the dialogues of seven clients and the pointer to the title of an announcement, 2026-11-14; 1832 before it; 1784 with three clients); raise this on purpose");
static_assert(sizeof(IpcResolver) == 8, "the game's address mapper: a context and a function");
static_assert(DlpHub::kQueuedMs >= 1000 && DlpHub::kQueuedMs <= 4000, "a request nobody takes is given up after 1 to 4 seconds");
static_assert(DlpHub::kAliveMs <= DlpHub::kQueuedMs, "a network thread is dead before a request waiting for it gives up");

HUB_NOINLINE void p_hub_ctor(void* where, UdsEngine* engine, KernelOps* kernel, const LobbyConfig& cfg, const IpcResolver& r, LobbyNet* backend) {
    ::new (where) DlpHub(engine, kernel, cfg, r, backend);
}
HUB_NOINLINE bool p_hub_IsInline(DlpKind kind, u32 id) { return DlpHub::IsInline(kind, id); }
HUB_NOINLINE u32 p_hub_Serve(DlpHub* h, DlpKind kind, u32* cmd, const u32* sbuf) { return static_cast<u32>(h->Serve(kind, cmd, sbuf)); }
HUB_NOINLINE bool p_hub_PollPending(DlpHub* h, u32* cmd) { return h->PollPending(cmd); }
HUB_NOINLINE void p_hub_OnGameUdsStarting(DlpHub* h) { h->OnGameUdsStarting(); }
HUB_NOINLINE void p_hub_OnSessionClosed(DlpHub* h, DlpKind kind) { h->OnSessionClosed(kind); }
HUB_NOINLINE bool p_hub_EngineHeld(const DlpHub* h) { return h->EngineHeld(); }
HUB_NOINLINE void p_hub_SetPlayerName(DlpHub* h, const u16* name) { h->SetPlayerName(name); }
HUB_NOINLINE void p_hub_SetFriendSeed(DlpHub* h, u64 seed) { h->SetFriendSeed(seed); }
HUB_NOINLINE void p_hub_Tick(DlpHub* h) { h->Tick(); }
HUB_NOINLINE void p_hub_Stop(DlpHub* h) { h->Stop(); }
HUB_NOINLINE void p_hub_RequestClntLeave(DlpHub* h) { h->RequestClntLeave(); }  // (1.1.4, G1)
HUB_NOINLINE u32 p_hub_counters(const DlpHub* h) {
    return h->served_inline() + h->served_posted() + h->cancelled() + h->failed_fast() + h->missed_ticks() + h->ran_here() + h->deferred_closes();
}
#endif
