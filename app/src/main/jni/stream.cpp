#include <algorithm>
#include <new>
#include <jni.h>
#include <mpv/client.h>
#include <mpv/stream_cb.h>

#include "globals.h"
#include "jni_utils.h"
#include "log.h"
#include "stream.h"

static jclass stream_class;
static jmethodID open_method, read_method, seek_method, size_method, cancel_method, close_method;

// Keep callbacks on the same Java thread until the native thread exits.
// Threads already attached by another owner must not be detached here.
class StreamThreadAttachment {
public:
    explicit StreamThreadAttachment(JavaVM *vm) : vm(vm) {}
    ~StreamThreadAttachment() {
        vm->DetachCurrentThread();
    }

private:
    JavaVM *const vm;
};

static JNIEnv *get_stream_env()
{
    JNIEnv *env = nullptr;
    int result = g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (result == JNI_EDETACHED) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
            return nullptr;
        static thread_local StreamThreadAttachment attachment(g_vm);
    } else if (result != JNI_OK) {
        return nullptr;
    }
    return env;
}

static bool check_exception(JNIEnv *env, const char *operation)
{
    if (!env->ExceptionCheck())
        return false;
    env->ExceptionClear();
    ALOGE("ISO stream %s failed", operation);
    return true;
}

static int64_t stream_read(void *cookie, char *buffer, uint64_t size)
{
    if (size == 0)
        return 0;
    JNIEnv *env = get_stream_env();
    if (!env)
        return -1;
    jlong capacity = static_cast<jlong>(std::min<uint64_t>(size, 64 * 1024));
    jobject target = env->NewDirectByteBuffer(buffer, capacity);
    if (!target) {
        check_exception(env, "allocate buffer");
        return -1;
    }
    jint result = env->CallIntMethod(static_cast<jobject>(cookie), read_method, target);
    env->DeleteLocalRef(target);
    if (check_exception(env, "read") || result < 0 || result > capacity)
        return -1;
    return result;
}

static int64_t stream_seek(void *cookie, int64_t position)
{
    JNIEnv *env = get_stream_env();
    if (!env)
        return MPV_ERROR_GENERIC;
    jlong result = env->CallLongMethod(static_cast<jobject>(cookie), seek_method,
                                       static_cast<jlong>(position));
    return check_exception(env, "seek") ? MPV_ERROR_GENERIC : result;
}

static int64_t stream_size(void *cookie)
{
    JNIEnv *env = get_stream_env();
    if (!env)
        return MPV_ERROR_UNSUPPORTED;
    jlong result = env->CallLongMethod(static_cast<jobject>(cookie), size_method);
    if (check_exception(env, "size") || result < 0)
        return MPV_ERROR_UNSUPPORTED;
    return result;
}

static void stream_cancel(void *cookie)
{
    JNIEnv *env = get_stream_env();
    if (env) {
        env->CallVoidMethod(static_cast<jobject>(cookie), cancel_method);
        check_exception(env, "cancel");
    }
}

static void stream_close(void *cookie)
{
    JNIEnv *env = get_stream_env();
    if (env) {
        env->CallVoidMethod(static_cast<jobject>(cookie), close_method);
        check_exception(env, "close");
        env->DeleteGlobalRef(static_cast<jobject>(cookie));
    }
}

static int stream_open(void *, char *uri, mpv_stream_cb_info *info)
{
    JNIEnv *env = get_stream_env();
    if (!env)
        return MPV_ERROR_LOADING_FAILED;
    jstring java_uri = utf8_to_jstring(env, uri);
    if (!java_uri) {
        check_exception(env, "allocate URI");
        return MPV_ERROR_LOADING_FAILED;
    }
    jobject local = env->CallStaticObjectMethod(mpv_MPVLib, open_method, java_uri);
    env->DeleteLocalRef(java_uri);
    if (check_exception(env, "open") || !local)
        return MPV_ERROR_LOADING_FAILED;
    jobject stream = env->NewGlobalRef(local);
    if (!stream) {
        check_exception(env, "retain cursor");
        env->CallVoidMethod(local, close_method);
        check_exception(env, "close");
    }
    env->DeleteLocalRef(local);
    if (!stream)
        return MPV_ERROR_LOADING_FAILED;
    info->cookie = stream;
    info->read_fn = stream_read;
    info->seek_fn = stream_seek;
    info->size_fn = stream_size;
    info->cancel_fn = stream_cancel;
    info->close_fn = stream_close;
    return MPV_ERROR_SUCCESS;
}

int register_iso_stream(JNIEnv *env, mpv_handle *context)
{
    if (!stream_class) {
        jclass local = env->FindClass("is/xyz/mpv/MPVLib$Stream");
        if (!local)
            return MPV_ERROR_GENERIC;
        stream_class = static_cast<jclass>(env->NewGlobalRef(local));
        env->DeleteLocalRef(local);
        if (!stream_class)
            return MPV_ERROR_NOMEM;
    }
    open_method = env->GetStaticMethodID(mpv_MPVLib, "openStream",
                                        "(Ljava/lang/String;)Lis/xyz/mpv/MPVLib$Stream;");
    if (!open_method)
        return MPV_ERROR_GENERIC;
    read_method = env->GetMethodID(stream_class, "read", "(Ljava/nio/ByteBuffer;)I");
    if (!read_method)
        return MPV_ERROR_GENERIC;
    seek_method = env->GetMethodID(stream_class, "seek", "(J)J");
    if (!seek_method)
        return MPV_ERROR_GENERIC;
    size_method = env->GetMethodID(stream_class, "size", "()J");
    if (!size_method)
        return MPV_ERROR_GENERIC;
    cancel_method = env->GetMethodID(stream_class, "cancel", "()V");
    if (!cancel_method)
        return MPV_ERROR_GENERIC;
    close_method = env->GetMethodID(stream_class, "close", "()V");
    if (!close_method)
        return MPV_ERROR_GENERIC;
    return mpv_stream_cb_add_ro(context, "media3iso", nullptr, stream_open);
}
