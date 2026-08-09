#include <jni.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <locale.h>
#include <atomic>
#include <mutex>

#include <mpv/client.h>

#include <pthread.h>

extern "C" {
    #include <libavcodec/jni.h>
}

#include "log.h"
#include "jni_utils.h"
#include "event.h"
#include "node.h"
#include "globals.h"

#define ARRAYLEN(a) (sizeof(a)/sizeof(a[0]))

extern "C" {
    jni_func(jboolean, nativeCreate, jobject appctx);
    jni_func(jboolean, nativeInit);
    jni_func(jboolean, nativeDestroy);

    jni_func(void, command, jobjectArray jarray);
    jni_func(jobject, commandNode, jobjectArray jarray);
};

// Implemented in render.cpp. Keeping surface ownership in one place means final
// teardown can release it even when SurfaceHolder.surfaceDestroyed() was missed.
void release_surface(JNIEnv *env);

JavaVM *g_vm = nullptr;
mpv_handle *g_mpv = nullptr;
std::atomic<bool> g_event_thread_request_exit(false);
std::recursive_mutex g_mpv_mutex;

static pthread_t event_thread_id{};
static bool g_event_thread_started = false;
static bool g_mpv_initialized = false;
static bool g_destroy_in_progress = false;
static jobject g_appctx = nullptr;

static bool prepare_environment(JNIEnv *env, jobject appctx) {
    setlocale(LC_NUMERIC, "C");

    JavaVM *vm = nullptr;
    if (env->GetJavaVM(&vm) != JNI_OK || !vm) {
        ALOGE("failed to acquire JavaVM");
        return false;
    }
    g_vm = vm;
    if (av_jni_set_java_vm(g_vm, NULL) < 0) {
        ALOGE("failed to configure FFmpeg JavaVM");
        return false;
    }

    // FFmpeg stores this pointer without taking ownership, so it must be a
    // global reference. MPVLib passes applicationContext to avoid retaining an
    // Activity across player recreation.
    jobject global_appctx = env->NewGlobalRef(appctx);
    if (!global_appctx) {
        ALOGE("failed to create application context global reference");
        return false;
    }
    if (av_jni_set_android_app_ctx(global_appctx, NULL) < 0) {
        env->DeleteGlobalRef(global_appctx);
        ALOGE("failed to configure FFmpeg Android application context");
        return false;
    }

    if (g_appctx)
        env->DeleteGlobalRef(g_appctx);
    g_appctx = global_appctx;

    init_methods_cache(env);
    return true;
}

jni_func(jboolean, nativeCreate, jobject appctx) {
    std::lock_guard<std::recursive_mutex> lock(g_mpv_mutex);

    if (g_mpv || g_destroy_in_progress) {
        ALOGW("mpv create called while an instance already exists or is being destroyed");
        return JNI_FALSE;
    }

    if (!prepare_environment(env, appctx))
        return JNI_FALSE;

    g_mpv = mpv_create();
    if (!g_mpv) {
        ALOGE("mpv context creation failed");
        return JNI_FALSE;
    }

    g_mpv_initialized = false;
    g_event_thread_started = false;
    g_event_thread_request_exit = false;

    // use terminal log level but request verbose messages
    // this way --msg-level can be used to adjust later
    mpv_request_log_messages(g_mpv, "terminal-default");
    mpv_set_option_string(g_mpv, "msg-level", "all=v");
    return JNI_TRUE;
}

jni_func(jboolean, nativeInit) {
    std::lock_guard<std::recursive_mutex> lock(g_mpv_mutex);

    if (!g_mpv || g_destroy_in_progress) {
        ALOGE("mpv init called without a usable created context");
        return JNI_FALSE;
    }
    if (g_mpv_initialized || g_event_thread_started) {
        ALOGW("mpv init called more than once");
        return JNI_FALSE;
    }

    int result = mpv_initialize(g_mpv);
    if (result < 0) {
        ALOGE("mpv init failed: %s", mpv_error_string(result));
        mpv_destroy(g_mpv);
        g_mpv = nullptr;
        return JNI_FALSE;
    }
    g_mpv_initialized = true;

    g_event_thread_request_exit = false;
    if (pthread_create(&event_thread_id, NULL, event_thread, NULL) != 0) {
        ALOGE("event thread creation failed");
        mpv_terminate_destroy(g_mpv);
        g_mpv = nullptr;
        g_mpv_initialized = false;
        return JNI_FALSE;
    }
    g_event_thread_started = true;
    pthread_setname_np(event_thread_id, "event_thread");
    return JNI_TRUE;
}

jni_func(jboolean, nativeDestroy) {
    bool join_event_thread = false;
    pthread_t thread_to_join{};

    // Phase one: stop and wake the event loop, then release the mutex before
    // joining. An in-flight Java observer is allowed to finish and call back
    // into libmpv without deadlocking against this teardown.
    {
        std::lock_guard<std::recursive_mutex> lock(g_mpv_mutex);
        if (!g_mpv) {
            ALOGV("mpv destroy called but it's already destroyed");
            return JNI_TRUE;
        }
        if (g_destroy_in_progress) {
            ALOGW("mpv destroy is already in progress");
            return JNI_FALSE;
        }

        g_destroy_in_progress = true;
        if (g_event_thread_started) {
            if (pthread_equal(pthread_self(), event_thread_id)) {
                ALOGE("mpv destroy cannot run from the mpv event callback thread");
                g_destroy_in_progress = false;
                return JNI_FALSE;
            }
            g_event_thread_request_exit = true;
            mpv_wakeup(g_mpv);
            thread_to_join = event_thread_id;
            join_event_thread = true;
        }
    }

    if (join_event_thread) {
        int join_result = pthread_join(thread_to_join, NULL);
        if (join_result != 0) {
            ALOGE("failed to join mpv event thread: %d", join_result);
            std::lock_guard<std::recursive_mutex> lock(g_mpv_mutex);
            g_destroy_in_progress = false;
            return JNI_FALSE;
        }
    }

    // Phase two: no event callback can still be using the handle. Serialize the
    // last resource releases against any other JNI calls, then destroy exactly once.
    {
        std::lock_guard<std::recursive_mutex> lock(g_mpv_mutex);
        g_event_thread_started = false;
        release_surface(env);

        if (g_mpv_initialized)
            mpv_terminate_destroy(g_mpv);
        else
            mpv_destroy(g_mpv);

        g_mpv = nullptr;
        g_mpv_initialized = false;
        g_event_thread_request_exit = false;
        g_destroy_in_progress = false;
    }

    return JNI_TRUE;
}

jni_func(void, command, jobjectArray jarray) {
    CHECK_MPV_INIT();

    const char *arguments[128] = {0};
    jstring strings[128] = {0};
    int len = env->GetArrayLength(jarray);
    if (len >= ARRAYLEN(arguments))
        die("too many command arguments");

    for (int i = 0; i < len; ++i) {
        strings[i] = (jstring)env->GetObjectArrayElement(jarray, i);
        arguments[i] = env->GetStringUTFChars(strings[i], NULL);
    }

    mpv_command(g_mpv, arguments);

    for (int i = 0; i < len; ++i) {
        env->ReleaseStringUTFChars(strings[i], arguments[i]);
        env->DeleteLocalRef(strings[i]);
    }
}

jni_func(jobject, commandNode, jobjectArray jarray) {
    CHECK_MPV_INIT();

    int len = env->GetArrayLength(jarray);
    if (len == 0) die("commandNode called with empty array");
    if (len > 128) die("commandNode called with too many arguments");

    mpv_node args;
    args.format = MPV_FORMAT_NODE_ARRAY;
    args.u.list = (mpv_node_list*)malloc(sizeof(mpv_node_list));
    args.u.list->num = len;
    args.u.list->values = (mpv_node*)malloc(len * sizeof(mpv_node));
    jstring strings[128] = {0};

    for (int i = 0; i < len; ++i) {
        strings[i] = (jstring)env->GetObjectArrayElement(jarray, i);
        const char *str = env->GetStringUTFChars(strings[i], NULL);
        args.u.list->values[i].format = MPV_FORMAT_STRING;
        args.u.list->values[i].u.string = strdup(str);
        env->ReleaseStringUTFChars(strings[i], str);
        env->DeleteLocalRef(strings[i]);
    }

    mpv_node result;
    int error = mpv_command_node(g_mpv, &args, &result);

    for (int i = 0; i < len; ++i) free(args.u.list->values[i].u.string);
    free(args.u.list->values);
    free(args.u.list);

    if (error < 0) return NULL;

    jobject jresult = mpv_node_to_jobject(env, &result);
    mpv_free_node_contents(&result);

    return jresult;
}
