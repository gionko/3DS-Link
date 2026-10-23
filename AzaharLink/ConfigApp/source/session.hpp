// What the app opens on the console to run: the CPU speed-up, the graphics, the services of the status line and the sockets. It is
// opened once when the app starts and closed when it ends. The sockets and the network status service are put away while the delivery
// module has the local wireless (SuspendNetwork), exactly as the module found a console when it was an app of its own.
#pragma once

namespace session {

// Opens, in this order: the CPU speed-up, the graphics (libctru, then citro3d/citro2d), cfgu, ac, am, the battery services
// (ptm:u, mcu::HWC; either may be refused) and the sockets. Fills ui::g.ac_ready and ui::g.soc_rc. Returns false when the graphics
// could not be started (gfx::InitError() says why): what was opened stays open then, so that ShowFatal() can print, and Stop()
// closes it.
bool Start();
// Closes what Start() opened, in the opposite order. Does nothing when nothing is open.
void Stop();
// Whether cfgu could be opened by Start() (the language and the profile name come from it).
bool CfguOk();

// Puts away the sockets and the network status service, and opens them again (the Titles Delivery's send and receive keep the local
// wireless to themselves between the two). Both do nothing when there is nothing to do.
void SuspendNetwork();
void ResumeNetwork();

// A text screen for when the graphics cannot be used: the message, and START to leave. Needs Start() to have been called.
void ShowFatal(const char* message);

} // namespace session
