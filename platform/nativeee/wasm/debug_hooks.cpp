// platform/nativeee (wasm side) -- the game-facing debug hooks the PS2 arm's pads.cpp defined (debug.h declares them).
#include "common.h"
#include "debug.h"

// Buttons held for the game's tests (debug.h): the pads arm ORs them into every report. 0 = none.
volatile u32 g_TestPadButtons = 0;
