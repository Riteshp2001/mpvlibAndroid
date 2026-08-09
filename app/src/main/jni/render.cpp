#include <jni.h>

#include <mpv/client.h>

#include "jni_utils.h"
#include "log.h"
#include "globals.h"

extern "C" {
    jni_func(void, attachSurface, jobject surface_);
    jni_func(void, detachSurface);
};

static jobject surface = nullptr;

// Release the JNI surface reference while the mpv handle is still valid.
// Called both from detachSurface() and the final native teardown so a missed
// SurfaceHolder callback cannot leak an Activity/Surface global reference.
void release_surface(JNIEnv *env) {
    if (g_mpv) {
        int64_t wid = 0;
        int result = mpv_set_option(g_mpv, "wid", MPV_FORMAT_INT64, &wid);
        if (result < 0)
            ALOGE("mpv_set_option(wid) returned error %s", mpv_error_string(result));
    }

    if (surface) {
        env->DeleteGlobalRef(surface);
        surface = nullptr;
    }
}

jni_func(void, attachSurface, jobject surface_) {
    CHECK_MPV_INIT();

    // Replacing an already attached surface must not leak the previous global ref.
    release_surface(env);

    surface = env->NewGlobalRef(surface_);
    if (!surface)
        die("invalid surface provided");

    int64_t wid = reinterpret_cast<intptr_t>(surface);
    int result = mpv_set_option(g_mpv, "wid", MPV_FORMAT_INT64, &wid);
    if (result < 0) {
        ALOGE("mpv_set_option(wid) returned error %s", mpv_error_string(result));
        env->DeleteGlobalRef(surface);
        surface = nullptr;
    }
}

jni_func(void, detachSurface) {
    CHECK_MPV_INIT();
    release_surface(env);
}
