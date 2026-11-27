package org.xbmc.kodi.engine;

import android.content.res.AssetManager;
import android.view.Surface;

/** JNI bridge to libkodiengine.so, which starts libkodi.so without an Activity. */
final class KodiNative
{
  static final int EVENT_START = 1;
  static final int EVENT_RESUME = 2;
  static final int EVENT_PAUSE = 3;
  static final int EVENT_STOP = 4;
  static final int EVENT_DESTROY = 5;
  static final int EVENT_FOCUS = 6;
  static final int EVENT_CONFIGURATION = 7;
  static final int EVENT_LOW_MEMORY = 8;

  private static volatile Runnable sFinishListener;
  private static volatile boolean sLoaded;

  private KodiNative()
  {
  }

  static void load()
  {
    // libkodi.so first: its JNI_OnLoad registers natives on org.xbmc.kodi classes, which only
    // resolve through the class loader of the caller of System.loadLibrary().
    System.loadLibrary("kodi");
    System.loadLibrary("kodiengine");
    sLoaded = true;
  }

  /** Whether the natives below can be called. */
  static boolean isLoaded()
  {
    return sLoaded;
  }

  static void setFinishListener(Runnable listener)
  {
    sFinishListener = listener;
  }

  /** Called by libkodiengine.so when Kodi asks to finish its (virtual) activity. */
  @SuppressWarnings("unused")
  static void onFinishRequested()
  {
    Runnable listener = sFinishListener;
    if (listener != null)
      listener.run();
  }

  /** Main thread only. */
  static native boolean nativeCreate(Object host, AssetManager assets, String internalPath,
                                     String externalPath, String obbPath, int sdkVersion);

  static native void nativeLifecycle(int event, int arg);

  /** Default buffer size of {@code surface} as (width << 32 | height), or 0. */
  static native long nativeSurfaceSize(Surface surface);
}
