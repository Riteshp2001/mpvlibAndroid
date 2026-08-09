#pragma once

#include <android/log.h>
#include <mutex>

#define DEBUG 1

#define LOG_TAG "mpv"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#if DEBUG
#define ALOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, __VA_ARGS__)
#else
#define ALOGV(...) (void)0
#endif

__attribute__((noreturn)) void die(const char *msg);

// Keep every JNI call that touches g_mpv serialized against final destruction.
// The mutex is recursive because a few helpers are shared by already-guarded calls.
#define CHECK_MPV_INIT() \
    std::lock_guard<std::recursive_mutex> mpv_lifecycle_lock(g_mpv_mutex); \
    do { \
        if (__builtin_expect(!g_mpv, 0)) \
            die("libmpv is not initialized"); \
    } while (0)
