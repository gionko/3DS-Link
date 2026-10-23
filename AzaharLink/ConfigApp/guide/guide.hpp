// guide.hpp -- "How to get boot9.bin": a short guide with a picture of GodMode9's screen at each step and a few words under it. One module for the 3DS Link app
// and for the installer child of the Download Play (both are citro2d programs with the same gfx:: / col:: drawing functions): they share the steps, the
// pictures, the keys and the drawing. The plan is in DlpLink/docs/GUIDA-BOOT9.md; AzaharLink/ConfigApp/guide/README.md says how a program takes the module in.
//
// The viewer is a screen of its own: the page (or the installer) that opens it forwards the keys and the two draw calls and does nothing else until Update()
// says the viewer is closed. Keys inside the viewer: the steps are a vertical list, so L or D-pad UP = the step before, R or D-pad DOWN = the next one, B = close
// (the D-pad's left and right do nothing in the viewer; a page that uses up and down for its own rows must not pass the key press that opened the viewer on to it
// in the same frame: a page is pushed in one frame and first updated in a later one, which is how pages_guide.cpp is safe), and a tap on the three buttons of
// the footer does what the key does.
//
// The screens must be lit: the viewer draws and does not touch the backlights. A page that can put them out (the 3DS Link Delivery page: SELECT) opens the viewer
// only while they are on, as it does for any other key.
#pragma once
#include <3ds.h>

#include "guide_logic.hpp"

struct C2D_SpriteSheet_s; // citro2d's (C2D_SpriteSheet is a pointer to it)

namespace guide {

// (The steps: StepCount(), StepAt(), Move() and the rest of what needs no console are in guide_logic.hpp, which this file includes.)

// Does the SD card hold boot9.bin where a 3DS Link installed as a CIA looks for it (sdmc:/3ds/3DSLink and sdmc:/gm9/out: boot9.bin, or a name with "boot9" in it)?
// A quick look by name and size (a few file calls). The installer child asks this; the app uses its own lookup (g.boot9: setup::FindBoot9), which also knows the
// folder the app was started from and the path the user chose, and checks the content.
bool Boot9Found();

class Viewer {
public:
    ~Viewer() { Close(); }
    // Starts at the first step. false = the viewer cannot start (no memory for the first picture, or a build without pictures: -DGUIDE_NO_PICTURES): the
    // caller says so instead of showing an empty screen.
    bool Open();
    // Gives the picture back. Safe to call again, and when the viewer was never opened. Call it before gfx::Shutdown().
    void Close();
    // One frame of input: the keys pressed this frame, and the touch screen position of a stylus that went down this frame (-1, -1 = no tap). false once the
    // viewer is closed (B, or a tap on Close): the caller goes back to its own screen, and Close() has been done already.
    bool Update(u32 keys_down, int tap_x = -1, int tap_y = -1);
    bool is_open() const { return open_; }
    void DrawTop();    // the step's picture, its title and text (the caller has done gfx::BeginTop())
    void DrawBottom(); // the bar "How to get boot9.bin" with "Step n of N", the list of steps, the footer with the keys (the caller has done gfx::BeginBottom())
    int step() const { return step_; }

private:
    bool LoadPicture();  // the picture of the current step (one at a time); false when there is none to show
    void FreePicture();

    int step_ = 0;
    bool open_ = false;
    C2D_SpriteSheet_s* sheet_ = nullptr; // the picture of step_ (null: a text-only step, or no memory for it)
    int sheet_image_ = -1;               // which picture sheet_ is
};

}  // namespace guide
