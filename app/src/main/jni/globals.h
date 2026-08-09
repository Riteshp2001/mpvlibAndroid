#pragma once

#include <atomic>
#include <mutex>

extern JavaVM *g_vm;
extern mpv_handle *g_mpv;
extern std::atomic<bool> g_event_thread_request_exit;
extern std::recursive_mutex g_mpv_mutex;
