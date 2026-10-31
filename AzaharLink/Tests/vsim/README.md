# vsim: a virtual-time model of the plugin's network thread

What it is for: to measure, on a PC and without a console, what a change to the ENet data path (Sources/enet, the loop of
`RoomClient::Run`) does to the delay of a frame inside the plugin and to the time the network thread is busy, with the real arrivals of the
game traces and the real costs of the soc calls from the plugin's own statistics windows. It is a model: it says what a change does
to the plugin's share of the delay, not what a console does; the console decides (one run on the console, the stats line of the plugin).

What is real: the ENet code of the tree it is built against (with `Sources/enet/enet_3ds.c` as platform layer, on a mocked libc) and,
on the room's side, ENet 1.3.18 unmodified, as Azahar's room runs it (its symbols renamed `r_enet_*` by `objcopy`, so that both live in
one program). What is modelled: the clock (microseconds; nothing sleeps), the wire (delay, jitter, loss, an outage that is silent or that
fails the calls), the soc calls (a mean and a spread per kind, a tail of stalls), the room's application (every frame is handed to the
console as a RELIABLE packet and flushed, as `room.cpp` does), the frames of the game (a trace) and the loop of `room_client.cpp`.

Files: `vsim.h`, `vnet.c` (libc, wire, tails, outage), `vsim_client.c` (the loop of `RoomClient::Run`, the keys, the metrics),
`room_plat.c`, `room_side.c` (the room), `shim/3ds.h`, `build.sh`, `sample/` (a 60 s trace of Tri Force Heroes as client and its profile).

    ENET_UP=/path/to/enet-1.3.18 bash Tests/vsim/build.sh /tmp/vsim_patched      # from the patched tree
    ENET_UP=...                  bash Tests/vsim/build.sh /tmp/vsim_original     # from a copy of the original tree
    /tmp/vsim_patched/vsim Tests/vsim/sample/TFH_friend_b1.cfg Tests/vsim/sample/TFH_friend_b1.trace

A configuration is `key=value` lines: the profile of the soc calls (`recv_us`, `recvE_us`, `send_us`, `pollr_us`, `wake_us`, `cv`,
`slow20_p`, ...), the wire (`up_base_us`, `up_sd_us`, `down_*`, `loss`), the run (`seconds`, `seed`, `outage_from_s`, `outage_to_s`,
`outage_silent`, `drop_connects`, `connect_rtt`) and the knobs of the patched tree (`skip_empty`, `max_dgrams`,
`ack_hold_events`, `ack_hold_ms`; 0 = ENet's own behaviour). A trace is lines `R <t_us> <size>` (the room hands the console a frame) and
`T <t_us> <size> <flag>` (the game sends a frame). The output is `key=value` lines (delay added by the plugin `added_ms_*`, end to end
`e2e_ms_*`, the thread's busy time `busy_ms_s`, the soc calls per second, the room's retransmissions...).

The traces and the profiles are made from the captures and the statistics windows of the sessions on the consoles (the tools that
made them are not part of the tree: they read the project's analysis files). The model is NOT calibrated to the logs in absolute terms:
for Star Rush it makes 250 to 265 rounds a second where the log windows say 187 to 199, and the busy time is a wall time that the windows
do not report. Read DIFFERENCES between two variants run on the same traces and seeds (paired), never an absolute value; the sign and
the size of a difference are what the model is good for, and a mechanism it shows (a call that is no longer made, an acknowledgement that
now rides on the reply) is then looked at on the console in the stats line of the plugin.
