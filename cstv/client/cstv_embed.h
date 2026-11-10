// CS:TV client built into another preload (rawinput2: make CSTV_DIR=...). cstv_client.cpp is then
// compiled with -DCSTV_EMBEDDED: it has no constructor of its own, and the host preload calls these.
#pragma once

// From the host's constructor. Starts CS:TV in the game process (cstrike_linux64) unless the
// environment says RAWINPUT2_CSTV=0 or another copy (the standalone libcstv.so) got there first.
// Returns whether it started.
bool cstv_start();

// The on/off key (F9). Off: the video stops, the screen shows its placeholder, the chat panel hides
// and the connection to cstvd is closed (cstvd then quits by itself). On: reconnects and catches up
// with the server. Returns the new state. Safe to call from the main thread at any time.
bool cstv_toggle();

// cstv_start() returned true.
bool cstv_running();

// For a host that must decide when CS:TV hooks the game (bhoptools: after its own DispatchUserMessage
// hook has looked at the original function). Hooks right away on the calling thread if the game's
// interfaces are there (a thread keeps trying if not). Stays out if another CS:TV copy (the separate
// libcstv.so) is loaded too. Returns whether CS:TV runs.
bool cstv_start_now();

// On/off as a setting (bhoptools' Fixes tab, F9). Can be set before cstv_start_now(): off from the start
// means CS:TV never connects to cstvd until it's turned on.
void cstv_set_enabled(bool on);
bool cstv_enabled();
