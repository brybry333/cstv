// What the CS:TV client needs from the operating system. Linux: implemented at the end of
// cstv_client.cpp (as it always was). Windows: cstv_platform_win.cpp, a file of its own because
// windows.h and the Source SDK headers don't mix.
#pragma once
#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#define PLAT_VSTDLIB "vstdlib"
#else
#define PLAT_VSTDLIB "libvstdlib"
#endif

double plat_now();                                          // seconds, monotonic
void *plat_interface(const char *module, const char *iface); // CreateInterface of "client" / "engine" / ...
void *plat_conmsg();                                        // printf-style console print from tier0, or null
void *plat_swap_slot(void *obj, int slot, void *fn);        // vtable slot swap; returns the old entry or null

// connection to cstvd (-1 = none)
intptr_t plat_connect();                                    // non-blocking, connected; -1 if cstvd isn't there
int plat_send(intptr_t s, const char *buf, int n);          // bytes sent; anything short = failed (never blocks)
int plat_recv(intptr_t s, char *buf, int n);                // > 0 data, 0 cstvd closed it, < 0 nothing right now
void plat_close(intptr_t s);

void *plat_map(const char *name, size_t size);              // cstvd's / the overlay's shared memory, or null
void plat_unmap(void *p, size_t size);
bool plat_in_game();                                        // this is the game process itself
bool plat_disabled();                                       // CSTV_DISABLE=1
bool plat_claim();                                          // the first CS:TV copy in this process: true (once)
void plat_spawn_helper();                                   // cstvd isn't answering: start it (Windows; Linux: systemd does)
bool plat_thread(void *(*fn)(void *));
void plat_sleep_ms(int ms);
