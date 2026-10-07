#include <jni.h>
#include <stdio.h>
#include <string.h>
#include <locale.h>
#include <atomic>
#include <string>
#include <utility>
#include <vector>

#include <mpv/client.h>

#include <pthread.h>

extern "C" {
    #include <libavcodec/jni.h>
}

#include "log.h"
#include "jni_utils.h"
#include "event.h"
#include "node.h"
#include "request.h"
#include "globals.h"
#include "stream.h"

extern "C" {
    jni_func(void, create, jobject appctx);
    jni_func(void, init);
    jni_func(jint, destroyNative);
    jni_func(void, command, jobjectArray jarray);
    jni_func(jint, commandResult, jobjectArray jarray);
    jni_func(jobject, commandNode, jobjectArray jarray);
    jni_func(jint, destroy);
    jni_func(jint, enqueueCommand, jlong request_id, jobjectArray jarray);
    jni_func(jint, enqueueCommandLongResult, jlong request_id,
             jstring result_key, jobjectArray jarray);
};

JavaVM *g_vm;
std::atomic<mpv_handle *> g_mpv(NULL);
std::atomic<bool> g_event_thread_started(false);
std::atomic<bool> g_shutdown_requested(false);
std::atomic<bool> g_force_shutdown(false);
std::mutex g_mpv_mutex;

static pthread_t event_thread_id;
static jobject global_appctx;
static constexpr int kMaxCommandArguments = 128;

static void throw_error_code(JNIEnv *env, const char *action, int result,
                            const char *detail)
{
    char message[256];
    if (detail)
        snprintf(message, sizeof(message), "%s failed (%d: %s)", action, result, detail);
    else
        snprintf(message, sizeof(message), "%s failed (%d)", action, result);
    throw_java_exception(env, message);
}

static void destroy_mpv_context()
{
    mpv_handle *context = g_mpv.exchange(NULL);
    if (!context)
        return;
    mpv_terminate_destroy(context);
}

static bool prepare_environment(JNIEnv *env, jobject appctx) {
    setlocale(LC_NUMERIC, "C");

    JavaVM *next_vm = NULL;
    jint jni_result = env->GetJavaVM(&next_vm);
    if (jni_result != JNI_OK || !next_vm) {
        throw_error_code(env, "GetJavaVM", jni_result, NULL);
        return false;
    }
    int result = av_jni_set_java_vm(next_vm, NULL);
    if (result < 0) {
        throw_error_code(env, "av_jni_set_java_vm", result, NULL);
        return false;
    }
    g_vm = next_vm;

    jobject next_appctx = env->NewGlobalRef(appctx);
    if (!next_appctx) {
        if (!env->ExceptionCheck())
            throw_java_exception(env, "failed to retain android app context");
        return false;
    }
    result = av_jni_set_android_app_ctx(next_appctx, NULL);
    if (result < 0) {
        env->DeleteGlobalRef(next_appctx);
        throw_error_code(env, "av_jni_set_android_app_ctx", result, NULL);
        return false;
    }
    if (global_appctx)
        env->DeleteGlobalRef(global_appctx);
    global_appctx = next_appctx;

    if (!init_methods_cache(env)) {
        if (!env->ExceptionCheck())
            throw_java_exception(env, "failed to initialize java method cache");
        return false;
    }
    return true;
}

jni_func(void, create, jobject appctx) {
    std::lock_guard<std::mutex> lock(g_mpv_mutex);
    if (g_shutdown_requested) {
        throw_java_exception(env, "mpv shutdown is still in progress");
        return;
    }
    if (g_mpv) {
        throw_java_exception(env, "mpv is already initialized");
        return;
    }

    if (!prepare_environment(env, appctx))
        return;

    g_mpv = mpv_create();
    if (!g_mpv) {
        throw_java_exception(env, "context init failed");
        return;
    }

    int stream_result = register_iso_stream(env, g_mpv);
    if (stream_result < 0) {
        destroy_mpv_context();
        if (!env->ExceptionCheck())
            throw_error_code(env, "register ISO stream", stream_result, NULL);
        return;
    }

    // use terminal log level but request verbose messages
    // this way --msg-level can be used to adjust later
    mpv_request_log_messages(g_mpv, "terminal-default");
    mpv_set_option_string(g_mpv, "msg-level", "all=v");
}

jni_func(void, init) {
    std::lock_guard<std::mutex> lock(g_mpv_mutex);
    if (!g_mpv) {
        throw_java_exception(env, "mpv is not created");
        return;
    }

    int result = mpv_initialize(g_mpv);
    if (result < 0) {
        throw_error_code(env, "mpv_initialize", result, mpv_error_string(result));
        destroy_mpv_context();
        return;
    }

    g_force_shutdown = false;
    result = pthread_create(&event_thread_id, NULL, event_thread, NULL);
    if (result != 0) {
        throw_error_code(env, "pthread_create", result, strerror(result));
        destroy_mpv_context();
        return;
    }
    g_event_thread_started = true;
    pthread_setname_np(event_thread_id, "event_thread");
    result = pthread_detach(event_thread_id);
    if (result != 0)
        ALOGE("pthread_detach failed (%d: %s)", result, strerror(result));
}

jni_func(jint, destroy) {
    std::lock_guard<std::mutex> lock(g_mpv_mutex);
    mpv_handle *context = g_mpv.load();
    if (!context) {
        ALOGV("mpv destroy called but it's already destroyed");
        return MPV_ERROR_SUCCESS;
    }

    if (!g_event_thread_started) {
        destroy_mpv_context();
        release_requests(env);
        return MPV_ERROR_SUCCESS;
    }

    return enqueue_shutdown(env);
}

jni_func(jint, destroyNative) {
    int result = jni_func_name(destroy)(env, obj);
    return result == MPV_ERROR_SUCCESS && g_event_thread_started ? 1 : result;
}

static int run_command(JNIEnv *env, jobjectArray jarray, uint64_t request_id,
                       const std::string *result_key) {
    std::lock_guard<std::mutex> lock(g_mpv_mutex);
    if (!check_mpv_initialized())
        return MPV_ERROR_UNINITIALIZED;

    if (!jarray)
        return MPV_ERROR_INVALID_PARAMETER;

    int len = env->GetArrayLength(jarray);
    if (len >= kMaxCommandArguments)
        return MPV_ERROR_INVALID_PARAMETER;

    std::vector<std::string> utf8_arguments(static_cast<size_t>(len));
    for (int i = 0; i < len; ++i) {
        jstring argument = (jstring)env->GetObjectArrayElement(jarray, i);
        if (!argument)
            return MPV_ERROR_INVALID_PARAMETER;
        bool converted = jstring_to_utf8(env, argument, &utf8_arguments[i]);
        env->DeleteLocalRef(argument);
        if (!converted)
            return env->ExceptionCheck() ? MPV_ERROR_NOMEM : MPV_ERROR_INVALID_PARAMETER;
    }

    int result;
    if (request_id) {
        result = result_key
            ? enqueue_command_long_result(env, request_id, *result_key,
                                          std::move(utf8_arguments))
            : enqueue_command(env, request_id, std::move(utf8_arguments));
    } else {
        std::vector<const char *> arguments(static_cast<size_t>(len) + 1, NULL);
        for (int i = 0; i < len; ++i)
            arguments[i] = utf8_arguments[i].c_str();
        result = mpv_command(g_mpv, arguments.data());
    }
    if (result < 0)
        ALOGE("%s returned error %s",
              request_id ? "mpv_command_async" : "mpv_command",
              mpv_error_string(result));

    return result;
}

jni_func(jint, commandResult, jobjectArray jarray) {
    return run_command(env, jarray, 0, NULL);
}

jni_func(void, command, jobjectArray jarray) {
    run_command(env, jarray, 0, NULL);
}

jni_func(jint, enqueueCommand, jlong request_id, jobjectArray jarray) {
    if (request_id <= 0)
        return MPV_ERROR_INVALID_PARAMETER;
    return run_command(env, jarray, static_cast<uint64_t>(request_id), NULL);
}

jni_func(jint, enqueueCommandLongResult, jlong request_id, jstring result_key,
         jobjectArray jarray) {
    if (request_id <= 0 || !result_key)
        return MPV_ERROR_INVALID_PARAMETER;
    std::string utf8_result_key;
    if (!jstring_to_utf8(env, result_key, &utf8_result_key))
        return env->ExceptionCheck() ? MPV_ERROR_NOMEM : MPV_ERROR_INVALID_PARAMETER;
    if (utf8_result_key.empty())
        return MPV_ERROR_INVALID_PARAMETER;
    return run_command(env, jarray, static_cast<uint64_t>(request_id),
                       &utf8_result_key);
}

jni_func(jobject, commandNode, jobjectArray jarray) {
    std::lock_guard<std::mutex> lock(g_mpv_mutex);
    if (!check_mpv_initialized() || !jarray)
        return NULL;

    int len = env->GetArrayLength(jarray);
    if (len == 0 || len > kMaxCommandArguments)
        return NULL;

    std::vector<std::string> arguments(static_cast<size_t>(len));
    std::vector<mpv_node> values(static_cast<size_t>(len));
    for (int index = 0; index < len; ++index) {
        jstring argument = (jstring)env->GetObjectArrayElement(jarray, index);
        if (!argument)
            return NULL;
        bool converted = jstring_to_utf8(env, argument, &arguments[index]);
        env->DeleteLocalRef(argument);
        if (!converted)
            return NULL;
        values[index].format = MPV_FORMAT_STRING;
        values[index].u.string = const_cast<char *>(arguments[index].c_str());
    }

    mpv_node_list list{};
    list.num = len;
    list.values = values.data();
    mpv_node args{};
    args.format = MPV_FORMAT_NODE_ARRAY;
    args.u.list = &list;
    mpv_node result{};
    int error = mpv_command_node(g_mpv, &args, &result);

    if (error < 0) return NULL;

    jobject jresult = mpv_node_to_jobject(env, &result);
    mpv_free_node_contents(&result);

    return jresult;
}
