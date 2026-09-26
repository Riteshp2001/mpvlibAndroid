/**
 * framegen_native.cpp
 *
 * JNI bridge for LSFG in mpvlibAndroid.
 *
 * Compilation modes:
 *   -DHAS_LSFG=1   → real Vulkan frame-gen pipeline active
 *   (no flag)      → stubs compile; APK UI shows "not supported"
 *
 * The render-loop integration lives in render.cpp which checks
 * g_frame_gen_enabled every frame after mpv_render_context_render().
 *
 * Vulkan path summary (--vo=gpu-next --gpu-api=vulkan --hwdec=mediacodec):
 *   mpv is switched to mpv_render_context_create() mode. We provide a
 *   VkDevice + VkQueue + VkSemaphore; mpv renders decoded frames
 *   (including MediaCodec HW surfaces imported as VkImages via AHardwareBuffer)
 *   into a VkImage we supply.  After each render we run LSFG compute shaders
 *   on frame[N-1] + frame[N] to generate (multiplier-1) intermediate frames,
 *   then present real + generated frames to our VkSwapchainKHR.
 */

#include <jni.h>
#include <android/log.h>
#include <atomic>
#include <mutex>
#include <string>

#ifdef HAS_LSFG
#include "frame_gen/lossless_dll.h"
#include "frame_gen/frame_gen.h"
#endif

#define LOG_TAG "FrameGenJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ── Shared state (read by render loop in render.cpp) ─────────────────────────
std::atomic<bool> g_frame_gen_enabled{false};
std::atomic<int>  g_frame_gen_multiplier{2};

// ── Storage root set from Java on create ─────────────────────────────────────
#ifdef HAS_LSFG
extern void FrameGen_SetStorageRoot(const std::string& root);
#endif

extern "C" {

// Called from MPVLib.create() / MPVLib.init() to give us the app files dir
JNIEXPORT void JNICALL
Java_app_gyrolet_mpvrx_ui_player_framegen_FrameGenNative_initStorageRoot(
        JNIEnv* env, jclass, jstring jpath) {
    const char* path = env->GetStringUTFChars(jpath, nullptr);
#ifdef HAS_LSFG
    FrameGen_SetStorageRoot(std::string(path));
#endif
    LOGI("Storage root: %s", path);
    env->ReleaseStringUTFChars(jpath, path);
}

JNIEXPORT jboolean JNICALL
Java_app_gyrolet_mpvrx_ui_player_framegen_FrameGenNative_supportsFrameGeneration(
        JNIEnv*, jobject) {
#ifdef HAS_LSFG
    return (jboolean)FrameGen::VulkanSupported();
#else
    return JNI_FALSE;
#endif
}

JNIEXPORT jstring JNICALL
Java_app_gyrolet_mpvrx_ui_player_framegen_FrameGenNative_getLosslessDllPath(
        JNIEnv* env, jclass) {
#ifdef HAS_LSFG
    return env->NewStringUTF(FrameGen::GetLosslessDllPath().c_str());
#else
    return env->NewStringUTF("");
#endif
}

JNIEXPORT jint JNICALL
Java_app_gyrolet_mpvrx_ui_player_framegen_FrameGenNative_validateLosslessDll(
        JNIEnv*, jclass) {
#ifdef HAS_LSFG
    return (jint)FrameGen::GetInstalledLosslessStatus();
#else
    return 1; // NotInstalled
#endif
}

JNIEXPORT jint JNICALL
Java_app_gyrolet_mpvrx_ui_player_framegen_FrameGenNative_prepareLosslessDll(
        JNIEnv*, jclass) {
#ifdef HAS_LSFG
    return (jint)FrameGen::BuildShaderCache();
#else
    return 1;
#endif
}

JNIEXPORT jboolean JNICALL
Java_app_gyrolet_mpvrx_ui_player_framegen_FrameGenNative_removeLosslessDll(
        JNIEnv*, jclass) {
#ifdef HAS_LSFG
    return (jboolean)FrameGen::RemoveInstalledLosslessDll();
#else
    return JNI_FALSE;
#endif
}

JNIEXPORT void JNICALL
Java_app_gyrolet_mpvrx_ui_player_framegen_FrameGenNative_setFrameGenEnabled(
        JNIEnv*, jobject, jboolean enabled, jint multiplier) {
    int clamped = (multiplier < 2) ? 2 : (multiplier > 4) ? 4 : (int)multiplier;
    g_frame_gen_enabled.store((bool)enabled);
    g_frame_gen_multiplier.store(clamped);
    LOGI("Frame gen %s × %d", enabled ? "ON" : "OFF", clamped);
}

} // extern "C"