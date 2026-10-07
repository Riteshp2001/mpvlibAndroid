#pragma once

#include <atomic>
#include <mutex>

extern JavaVM *g_vm;
extern std::atomic<mpv_handle *> g_mpv;
extern std::atomic<bool> g_event_thread_started;
extern std::atomic<bool> g_shutdown_requested;
extern std::atomic<bool> g_force_shutdown;
// Protect handle users from concurrent creation and destruction.
extern std::mutex g_mpv_mutex;
