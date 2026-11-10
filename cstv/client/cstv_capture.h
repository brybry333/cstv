// In-game capture for "Start stream" (cstv_capture.cpp, Linux only for now). All on the game's main thread.
#pragma once

#ifdef _WIN32
inline void capture_start() {}
inline void capture_stop() {}
inline void capture_frame() {}
inline void capture_paint() {}
#else
void capture_start();   // cstvd: `capture 1`
void capture_stop();    // `capture 0`, or the connection to cstvd is gone
void capture_frame();   // FRAME_START
void capture_paint();   // MatSystemTopPanel painted (the HUD is drawn), before the CS:TV window draws
#endif
