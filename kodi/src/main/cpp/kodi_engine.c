/*
 * Starts libkodi.so without an Activity.
 *
 * Kodi's Android port is a NativeActivity: android_native_app_glue's ANativeActivity_onCreate()
 * starts android_main(), and the framework then drives it through ANativeActivity callbacks.
 * Here a Service builds the ANativeActivity itself, with a headless Java host object as the
 * "activity" (org.xbmc.kodi.Main implements the few Activity methods Kodi calls through JNI).
 */

#define _GNU_SOURCE // pipe2

#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window_jni.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TAG "KodiEngine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/*
 * The framework allocates ANativeActivity as the head of a private struct, and
 * ANativeActivity_finish() writes a command to an int member of that struct's tail. Kodi calls it
 * when it wants to quit. Every 32-bit slot of this tail holds the write end of our pipe, so the
 * request reaches finish_reader() whatever the member's offset is.
 */
#define TAIL_SLOTS 4096
#define CMD_FINISH 1

typedef struct
{
  ANativeActivity activity;
  int32_t tail[TAIL_SLOTS];
} EngineActivity;

enum
{
  EVENT_START = 1,
  EVENT_RESUME = 2,
  EVENT_PAUSE = 3,
  EVENT_STOP = 4,
  EVENT_DESTROY = 5,
  EVENT_FOCUS = 6,
  EVENT_CONFIGURATION = 7,
  EVENT_LOW_MEMORY = 8,
};

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static EngineActivity* s_engine;
static ANativeActivityCallbacks s_callbacks;
static int s_pipe[2] = {-1, -1};
static jclass s_nativeClass;
static jmethodID s_onFinishRequested;
static JavaVM* s_vm;

static char* dup_string(JNIEnv* env, jstring value)
{
  if (value == NULL)
    return strdup("");
  const char* chars = (*env)->GetStringUTFChars(env, value, NULL);
  char* copy = strdup(chars != NULL ? chars : "");
  if (chars != NULL)
    (*env)->ReleaseStringUTFChars(env, value, chars);
  return copy;
}

static void* finish_reader(void* unused)
{
  (void)unused;
  unsigned char work[64];
  for (;;)
  {
    ssize_t n = read(s_pipe[0], work, sizeof(work));
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    int32_t cmd;
    memcpy(&cmd, work, sizeof(cmd));
    LOGI("activity request %d", cmd);
    if (cmd != CMD_FINISH)
      continue;

    JNIEnv* env = NULL;
    JavaVMAttachArgs args = {JNI_VERSION_1_6, "KodiEngineFinish", NULL};
    if ((*s_vm)->AttachCurrentThread(s_vm, &env, &args) != JNI_OK)
      continue;
    (*env)->CallStaticVoidMethod(env, s_nativeClass, s_onFinishRequested);
    if ((*env)->ExceptionCheck(env))
      (*env)->ExceptionClear(env);
    (*s_vm)->DetachCurrentThread(s_vm);
  }
  return NULL;
}

JNIEXPORT jboolean JNICALL Java_org_xbmc_kodi_engine_KodiNative_nativeCreate(JNIEnv* env,
                                                                             jclass clazz,
                                                                             jobject host,
                                                                             jobject assets,
                                                                             jstring internalPath,
                                                                             jstring externalPath,
                                                                             jstring obbPath,
                                                                             jint sdkVersion)
{
  pthread_mutex_lock(&s_lock);
  if (s_engine != NULL)
  {
    pthread_mutex_unlock(&s_lock);
    LOGE("Kodi is already running in this process");
    return JNI_FALSE;
  }

  // System.loadLibrary("kodi") ran first, so JNI_OnLoad bound Kodi's natives to the host's
  // classes; this only looks the library up again.
  void* kodi = dlopen("libkodi.so", RTLD_NOW);
  if (kodi == NULL)
  {
    pthread_mutex_unlock(&s_lock);
    LOGE("dlopen(libkodi.so) failed: %s", dlerror());
    return JNI_FALSE;
  }
  ANativeActivity_createFunc* onCreate =
      (ANativeActivity_createFunc*)dlsym(kodi, "ANativeActivity_onCreate");
  if (onCreate == NULL)
  {
    pthread_mutex_unlock(&s_lock);
    LOGE("libkodi.so has no ANativeActivity_onCreate");
    return JNI_FALSE;
  }

  if (pipe2(s_pipe, O_CLOEXEC) != 0)
  {
    pthread_mutex_unlock(&s_lock);
    LOGE("pipe2 failed: %s", strerror(errno));
    return JNI_FALSE;
  }

  (*env)->GetJavaVM(env, &s_vm);
  s_nativeClass = (jclass)(*env)->NewGlobalRef(env, clazz);
  s_onFinishRequested = (*env)->GetStaticMethodID(env, clazz, "onFinishRequested", "()V");

  EngineActivity* engine = (EngineActivity*)calloc(1, sizeof(EngineActivity));
  for (int i = 0; i < TAIL_SLOTS; ++i)
    engine->tail[i] = s_pipe[1];

  ANativeActivity* activity = &engine->activity;
  memset(&s_callbacks, 0, sizeof(s_callbacks));
  activity->callbacks = &s_callbacks;
  activity->vm = s_vm;
  activity->env = env;
  activity->clazz = (*env)->NewGlobalRef(env, host);
  activity->internalDataPath = dup_string(env, internalPath);
  activity->externalDataPath = dup_string(env, externalPath);
  activity->obbPath = dup_string(env, obbPath);
  activity->sdkVersion = sdkVersion;
  // AAssetManager_fromJava() needs the Java object to stay alive.
  activity->assetManager = AAssetManager_fromJava(env, (*env)->NewGlobalRef(env, assets));

  pthread_t reader;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_create(&reader, &attr, finish_reader, NULL);
  pthread_attr_destroy(&attr);

  s_engine = engine;
  pthread_mutex_unlock(&s_lock);

  LOGI("starting libkodi.so (sdk %d)", sdkVersion);
  onCreate(activity, NULL, 0);
  return JNI_TRUE;
}

JNIEXPORT void JNICALL Java_org_xbmc_kodi_engine_KodiNative_nativeLifecycle(JNIEnv* env,
                                                                           jclass clazz,
                                                                           jint event,
                                                                           jint arg)
{
  (void)env;
  (void)clazz;
  pthread_mutex_lock(&s_lock);
  EngineActivity* engine = s_engine;
  pthread_mutex_unlock(&s_lock);
  if (engine == NULL)
    return;

  ANativeActivity* activity = &engine->activity;
  ANativeActivityCallbacks* cb = activity->callbacks;
  switch (event)
  {
    case EVENT_START:
      if (cb->onStart)
        cb->onStart(activity);
      break;
    case EVENT_RESUME:
      if (cb->onResume)
        cb->onResume(activity);
      break;
    case EVENT_PAUSE:
      if (cb->onPause)
        cb->onPause(activity);
      break;
    case EVENT_STOP:
      if (cb->onStop)
        cb->onStop(activity);
      break;
    case EVENT_DESTROY:
      // Returns only if Kodi does not exit(0) the process first.
      if (cb->onDestroy)
        cb->onDestroy(activity);
      break;
    case EVENT_FOCUS:
      if (cb->onWindowFocusChanged)
        cb->onWindowFocusChanged(activity, arg);
      break;
    case EVENT_CONFIGURATION:
      if (cb->onConfigurationChanged)
        cb->onConfigurationChanged(activity);
      break;
    case EVENT_LOW_MEMORY:
      if (cb->onLowMemory)
        cb->onLowMemory(activity);
      break;
    default:
      LOGE("unknown lifecycle event %d", event);
      break;
  }
}

/** Default buffer size of a Surface packed as (width << 32 | height), or 0 when unknown. */
JNIEXPORT jlong JNICALL Java_org_xbmc_kodi_engine_KodiNative_nativeSurfaceSize(JNIEnv* env,
                                                                              jclass clazz,
                                                                              jobject surface)
{
  (void)clazz;
  if (surface == NULL)
    return 0;
  ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
  if (window == NULL)
    return 0;
  int32_t width = ANativeWindow_getWidth(window);
  int32_t height = ANativeWindow_getHeight(window);
  ANativeWindow_release(window);
  if (width <= 0 || height <= 0)
    return 0;
  return ((jlong)width << 32) | (jlong)(uint32_t)height;
}
